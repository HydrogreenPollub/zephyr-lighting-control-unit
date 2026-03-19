//
// Created by inż. Dawid Pisarczyk on 28.12.2025.
//

#include "lighting_control_unit.h"
#include "lcu_dfu.h"
#include "status_led.h"
#include "test_button.h"
#include "gpio.h"
#include "candef.h"

LOG_MODULE_REGISTER(lighting_control_unit, LOG_LEVEL_INF);

/* ── CAN ──────────────────────────────────────────────────────────────────── */

#define LCU_CAN_TX_THREAD_STACK_SIZE  2048
#define LCU_CAN_TX_THREAD_PRIORITY    5
#define LCU_CAN_PERIODIC_STACK_SIZE   2048
#define LCU_CAN_PERIODIC_PRIORITY     5

K_THREAD_STACK_DEFINE(lcu_can_tx_stack, LCU_CAN_TX_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(lcu_can_periodic_stack, LCU_CAN_PERIODIC_STACK_SIZE);
struct k_thread lcu_can_tx_thread_data;
struct k_thread lcu_can_periodic_thread_data;

K_SEM_DEFINE(can_tx_done_sem, 0, 1);
K_MSGQ_DEFINE(lcu_can_tx_msgq, sizeof(struct can_frame), 32, 4);

static volatile int can_tx_result;
static struct k_work_delayable tx_led_off_work;
static struct k_work_delayable rx_led_off_work;
static struct k_work_delayable bus_off_recovery_work;
static struct k_work_delayable can_test_leds_off_work;

static const struct can_filter lcu_can_filters[] = {
    CAN_FILTER(CAN_ID_BRAKE_PEDAL_VOLTAGE),
    CAN_FILTER(CAN_ID_BUTTONS_LIGHTS_MASK),
};

/* MCU_LIGHTING (0x400, extended) — authoritative lighting commands from MCU */
static const struct can_filter mcu_lighting_filter = {
    .id    = CANDEF_MCU_LIGHTING_FRAME_ID,
    .mask  = CAN_EXT_ID_MASK,
    .flags = CAN_FRAME_IDE,
};

lcu_can_t can = {
    .device = DEVICE_DT_GET(DT_ALIAS(can)),
    .rx_led = GPIO_DT_SPEC_GET(DT_ALIAS(can_rx_led), gpios),
    .tx_led = GPIO_DT_SPEC_GET(DT_ALIAS(can_tx_led), gpios),
};

lcu_lights_t lights = {
    .strip = DEVICE_DT_GET(DT_ALIAS(led_strip)),
    .num_pixels = STRIP_NUM_PIXELS,
    .pixels = {},
    .lights_mask = 0,
};

static void tx_led_off_handler(struct k_work *work) { gpio_reset(&can.tx_led); }
static void rx_led_off_handler(struct k_work *work) { gpio_reset(&can.rx_led); }
static void can_test_leds_off_handler(struct k_work *work) {
    gpio_reset(&can.rx_led);
    gpio_reset(&can.tx_led);
}

static void bus_off_recovery_handler(struct k_work *work) {
    int ret = can_recover(can.device, K_MSEC(100));
    if (ret != 0 && ret != -ENOTSUP) {
        LOG_WRN("CAN recovery failed (%d), retrying in 1s", ret);
        k_work_reschedule(&bus_off_recovery_work, K_SECONDS(1));
    } else {
        LOG_INF("CAN bus recovered");
        status_led_set(STATUS_LED_OPERATIONAL);
    }
}

static void can_state_change_cb(const struct device *dev,
                                enum can_state state,
                                struct can_bus_err_cnt err_cnt,
                                void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    LOG_WRN("CAN state: %d (tx_err=%d rx_err=%d)",
            state, err_cnt.tx_err_cnt, err_cnt.rx_err_cnt);

    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        status_led_set(STATUS_LED_OPERATIONAL);
        break;
    case CAN_STATE_ERROR_WARNING:
    case CAN_STATE_ERROR_PASSIVE:
        status_led_set(STATUS_LED_WARNING);
        break;
    case CAN_STATE_BUS_OFF:
        status_led_set(STATUS_LED_BUS_OFF);
        k_msgq_purge(&lcu_can_tx_msgq);
        k_work_reschedule(&bus_off_recovery_work, K_MSEC(100));
        break;
    default:
        break;
    }
}

static void lcu_can_tx_callback(const struct device *dev, int error, void *user_data) {
    can_tx_result = error;
    k_sem_give(&can_tx_done_sem);
}

static void lcu_can_tx_thread(void *p1, void *p2, void *p3) {
    struct can_frame frame = {0};
    LOG_INF("CAN TX thread started");

    while (1) {
        k_msgq_get(&lcu_can_tx_msgq, &frame, K_FOREVER);

        int ret = can_send(can.device, &frame, K_MSEC(100), lcu_can_tx_callback, NULL);
        if (ret) {
            LOG_ERR("CAN send failed: %d", ret);
            continue;
        }

        if (k_sem_take(&can_tx_done_sem, K_MSEC(200)) != 0) {
            LOG_ERR("CAN TX timeout");
            continue;
        }

        if (can_tx_result != 0) {
            LOG_ERR("CAN TX error: %d", can_tx_result);
            continue;
        }

        gpio_set(&can.tx_led);
        k_work_reschedule(&tx_led_off_work, K_MSEC(50));
    }
}

/* ── Enqueue helper (same pattern as CCU) ─────────────────────────────────── */

static void enqueue_frame(uint32_t id, const uint8_t *data, uint8_t len) {
    struct can_frame frame = {};
    if (len > sizeof(frame.data)) {
        LOG_WRN("CAN payload too big: %u", len);
        return;
    }
    frame.id = id;
    frame.dlc = len;
    frame.flags = CAN_FRAME_IDE;
    memcpy(frame.data, data, len);
    if (k_msgq_put(&lcu_can_tx_msgq, &frame, K_NO_WAIT) != 0) {
        LOG_WRN("CAN TX queue full");
    }
}

#define PACK_AND_ENQUEUE(UPPER_NAME, lower_name, struct_ptr)               \
    do {                                                                   \
        uint8_t _buf[CANDEF_##UPPER_NAME##_LENGTH];                        \
        candef_##lower_name##_pack(_buf, (struct_ptr), sizeof(_buf));      \
        enqueue_frame(CANDEF_##UPPER_NAME##_FRAME_ID, _buf, sizeof(_buf)); \
    } while (0)

static void send_lcu_status(void) {
    struct candef_mcu_lighting_t lighting;
    candef_mcu_lighting_unpack(&lighting, &lights.lights_mask, sizeof(lights.lights_mask));

    struct candef_lcu_status_t frame = {
        .headlight       = lighting.headlight,
        .position_light  = lighting.position_light,
        .brake_light     = lighting.brake_light,
        .left_indicator  = lighting.left_indicator,
        .right_indicator = lighting.right_indicator,
        .hazard          = lighting.hazard,
    };
    PACK_AND_ENQUEUE(LCU_STATUS, lcu_status, &frame);
}

/* ── Periodic CAN TX ──────────────────────────────────────────────────────── */

static void lcu_can_periodic_thread(void *p1, void *p2, void *p3) {
    uint8_t cnt_1000ms = 0;
    while (1) {
        k_sleep(K_MSEC(10));

        if (can_dfu_is_active()) {
            cnt_1000ms = 0;
            continue;
        }

        cnt_1000ms++;
        if (cnt_1000ms >= 100) {
            cnt_1000ms = 0;
            send_lcu_status();
        }
    }
}

/* ── CAN RX callbacks ─────────────────────────────────────────────────────── */

static void lcu_can_rx_callback(const struct device *dev, struct can_frame *frame, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    gpio_set(&can.rx_led);
    k_work_reschedule(&rx_led_off_work, K_MSEC(50));

    LOG_INF("CAN ID: 0x%03X, Data: %u", frame->id, frame->data[0]);

    switch ((can_id_t)frame->id) {
    case CAN_ID_BRAKE_PEDAL_VOLTAGE:
        LOG_INF("BRAKE_PEDAL_VOLTAGE");
        break;
    case CAN_ID_BUTTONS_LIGHTS_MASK:
        break;
    default:
        break;
    }
}

static void lcu_mcu_lighting_rx_cb(const struct device *dev, struct can_frame *frame, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    gpio_set(&can.rx_led);
    k_work_reschedule(&rx_led_off_work, K_MSEC(50));

    if (frame->dlc >= CANDEF_MCU_LIGHTING_LENGTH) {
        struct candef_mcu_lighting_t msg;
        candef_mcu_lighting_unpack(&msg, frame->data, frame->dlc);
        lights.lights_mask = frame->data[0];
        LOG_INF("MCU_LIGHTING: 0x%02X (H=%d P=%d B=%d L=%d R=%d Hz=%d)",
                lights.lights_mask, msg.headlight, msg.position_light,
                msg.brake_light, msg.left_indicator, msg.right_indicator,
                msg.hazard);
    }
}

static void lcu_dfu_rx_cb(const struct device *dev, struct can_frame *frame, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    gpio_set(&can.rx_led);
    k_work_reschedule(&rx_led_off_work, K_MSEC(50));

    can_dfu_on_frame(frame);
}

/* Single filter matches both CMD and DATA DFU IDs */
static const struct can_filter dfu_filter = {
    .id    = LCU_DFU_CMD_ID,
    .mask  = 0x1FFFFFFEU,
    .flags = CAN_FRAME_IDE,
};

/* ── Test button ──────────────────────────────────────────────────────────── */

void lcu_can_test(void) {
    gpio_set(&can.rx_led);
    gpio_set(&can.tx_led);
    k_work_reschedule(&can_test_leds_off_work, K_SECONDS(2));

    send_lcu_status();

    LOG_INF("CAN test: LEDs on, status frame enqueued");
}

static void on_test_button(void) {
    LOG_INF("Test button pressed — lighting all LEDs, sending status");
    status_led_set_override(true);
    lcu_can_test();
    k_sleep(K_SECONDS(2));
    status_led_set_override(false);
    LOG_INF("Test button: done");
}

/* ── Lights ───────────────────────────────────────────────────────────────── */

static void lcu_lights_init(void) {
    led_strip_init(lights.strip);
    led_strip_clear_all_pixels(lights.strip, lights.pixels, lights.num_pixels);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void lcu_init(void) {
    /* CAN bus */
    can_init(can.device, HYDROGREEN_CAN_BAUD_RATE);
    gpio_init(&can.rx_led, GPIO_OUTPUT_INACTIVE);
    gpio_init(&can.tx_led, GPIO_OUTPUT_INACTIVE);
    k_work_init_delayable(&tx_led_off_work, tx_led_off_handler);
    k_work_init_delayable(&rx_led_off_work, rx_led_off_handler);
    k_work_init_delayable(&bus_off_recovery_work, bus_off_recovery_handler);
    k_work_init_delayable(&can_test_leds_off_work, can_test_leds_off_handler);
    can_set_state_change_callback(can.device, can_state_change_cb, NULL);

    for (int i = 0; i < ARRAY_SIZE(lcu_can_filters); i++) {
        can_add_rx_filter_(can.device, lcu_can_rx_callback, &lcu_can_filters[i]);
    }
    can_add_rx_filter_(can.device, lcu_mcu_lighting_rx_cb, &mcu_lighting_filter);
    can_add_rx_filter_(can.device, lcu_dfu_rx_cb, &dfu_filter);
    lcu_dfu_init();

    k_tid_t tx_tid = k_thread_create(
        &lcu_can_tx_thread_data, lcu_can_tx_stack,
        K_THREAD_STACK_SIZEOF(lcu_can_tx_stack),
        lcu_can_tx_thread, NULL, NULL, NULL,
        LCU_CAN_TX_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(tx_tid, "can_tx");

    k_tid_t periodic_tid = k_thread_create(
        &lcu_can_periodic_thread_data, lcu_can_periodic_stack,
        K_THREAD_STACK_SIZEOF(lcu_can_periodic_stack),
        lcu_can_periodic_thread, NULL, NULL, NULL,
        LCU_CAN_PERIODIC_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(periodic_tid, "can_periodic");

    /* Lights */
    lcu_lights_init();

    /* Board peripherals */
    static const struct gpio_dt_spec status_led_gpio =
        GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
    static const struct gpio_dt_spec test_btn_gpio =
        GPIO_DT_SPEC_GET(DT_ALIAS(button_test), gpios);

    status_led_init(&status_led_gpio, NULL);
    test_button_init(&test_btn_gpio, on_test_button);
}

void lcu_on_tick(void) {
    k_sleep(K_FOREVER);
}
