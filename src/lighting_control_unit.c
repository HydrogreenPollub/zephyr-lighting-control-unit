//
// Created by inż. Dawid Pisarczyk on 28.12.2025.
//

#include <errno.h>

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
#define CAN_TX_PROBE_INTERVAL_MS      5000
#define LIGHT_RENDER_INTERVAL_MS      100
#define LIGHT_BLINK_INTERVAL_MS       500

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
static volatile enum can_state lcu_can_state = CAN_STATE_STOPPED;
static enum can_state last_suspended_log_state = CAN_STATE_STOPPED;
static int64_t last_can_tx_probe_ms;
static bool can_tx_suspended_logged;
static volatile bool lights_dirty = true;

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
    .left_strip  = DEVICE_DT_GET(LEFT_STRIP_NODE),
    .right_strip = DEVICE_DT_GET(RIGHT_STRIP_NODE),
    .num_pixels  = STRIP_NUM_PIXELS,
    .pixels_left  = {0},
    .pixels_right = {0},
    .lights_mask  = 0,
};

static volatile bool test_active;

static void tx_led_off_handler(struct k_work *work) { if (!test_active) gpio_reset(&can.tx_led); }
static void rx_led_off_handler(struct k_work *work) { if (!test_active) gpio_reset(&can.rx_led); }

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

static const char *can_state_name(enum can_state state) {
    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        return "error-active";
    case CAN_STATE_ERROR_WARNING:
        return "error-warning";
    case CAN_STATE_ERROR_PASSIVE:
        return "error-passive";
    case CAN_STATE_BUS_OFF:
        return "bus-off";
    case CAN_STATE_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

static enum can_state get_current_can_state(void) {
    enum can_state state = lcu_can_state;
    struct can_bus_err_cnt err_cnt;

    if (can_get_state(can.device, &state, &err_cnt) == 0) {
        lcu_can_state = state;
    }

    return state;
}

static bool can_tx_allowed(void) {
    enum can_state state = get_current_can_state();

    if (state == CAN_STATE_ERROR_ACTIVE) {
        can_tx_suspended_logged = false;
        return true;
    }

    if (state == CAN_STATE_ERROR_WARNING || state == CAN_STATE_ERROR_PASSIVE) {
        int64_t now = k_uptime_get();

        if (now - last_can_tx_probe_ms >= CAN_TX_PROBE_INTERVAL_MS) {
            last_can_tx_probe_ms = now;
            return true;
        }
    }

    k_msgq_purge(&lcu_can_tx_msgq);
    if (!can_tx_suspended_logged || state != last_suspended_log_state) {
        LOG_WRN("CAN TX suspended while bus is %s", can_state_name(state));
        can_tx_suspended_logged = true;
        last_suspended_log_state = state;
    }

    if (state == CAN_STATE_BUS_OFF) {
        k_work_reschedule(&bus_off_recovery_work, K_MSEC(100));
    }

    return false;
}

static void update_status_led_for_can_state(enum can_state state) {
    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        status_led_set(STATUS_LED_OPERATIONAL);
        break;
    case CAN_STATE_ERROR_WARNING:
    case CAN_STATE_ERROR_PASSIVE:
    case CAN_STATE_BUS_OFF:
        status_led_set(STATUS_LED_WARNING);
        break;
    default:
        status_led_set(STATUS_LED_WARNING);
        break;
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
    lcu_can_state = state;

    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        can_tx_suspended_logged = false;
        update_status_led_for_can_state(state);
        break;
    case CAN_STATE_ERROR_WARNING:
    case CAN_STATE_ERROR_PASSIVE:
        update_status_led_for_can_state(state);
        break;
    case CAN_STATE_BUS_OFF:
        update_status_led_for_can_state(state);
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

        if (!can_tx_allowed()) {
            k_sleep(K_MSEC(500));
            continue;
        }

        k_sem_reset(&can_tx_done_sem);
        int ret = can_send(can.device, &frame, K_MSEC(100), lcu_can_tx_callback, NULL);
        if (ret) {
            if (ret == -EAGAIN) {
                if (get_current_can_state() == CAN_STATE_ERROR_ACTIVE) {
                    LOG_WRN("CAN TX mailbox unavailable; dropping queued frames");
                }
                k_msgq_purge(&lcu_can_tx_msgq);
                k_sleep(K_MSEC(500));
            } else {
                LOG_ERR("CAN send failed: %d", ret);
            }
            continue;
        }

        if (k_sem_take(&can_tx_done_sem, K_MSEC(200)) != 0) {
            enum can_state state = get_current_can_state();
            if (state == CAN_STATE_ERROR_ACTIVE) {
                LOG_WRN("CAN TX timeout while bus is %s; dropping queued frames",
                        can_state_name(state));
            }
            k_msgq_purge(&lcu_can_tx_msgq);
            k_sleep(K_MSEC(500));
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

static void render_lights(bool blink_on) {
    struct candef_mcu_lighting_t lighting;
    uint8_t left_r = 0;
    uint8_t left_g = 0;
    uint8_t left_b = 0;
    uint8_t right_r = 0;
    uint8_t right_g = 0;
    uint8_t right_b = 0;

    candef_mcu_lighting_unpack(&lighting, &lights.lights_mask, sizeof(lights.lights_mask));

    if (lighting.position_light) {
        left_r = right_r = 0x10;
        left_g = right_g = 0x10;
        left_b = right_b = 0x10;
    }

    if (lighting.headlight) {
        left_r = right_r = 0x60;
        left_g = right_g = 0x60;
        left_b = right_b = 0x60;
    }

    if (lighting.brake_light) {
        left_r = right_r = 0x80;
        left_g = right_g = 0x00;
        left_b = right_b = 0x00;
    }

    if (blink_on && lighting.hazard) {
        left_r = right_r = 0x80;
        left_g = right_g = 0x40;
        left_b = right_b = 0x00;
    } else if (blink_on) {
        if (lighting.left_indicator) {
            left_r = 0x80;
            left_g = 0x40;
            left_b = 0x00;
        }
        if (lighting.right_indicator) {
            right_r = 0x80;
            right_g = 0x40;
            right_b = 0x00;
        }
    }

    int ret = led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels,
                                       left_r, left_g, left_b);
    if (ret != 0) {
        LOG_WRN("Failed to update left LED strip: %d", ret);
    }
    ret = led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels,
                                   right_r, right_g, right_b);
    if (ret != 0) {
        LOG_WRN("Failed to update right LED strip: %d", ret);
    }
}

/* ── Periodic CAN TX ──────────────────────────────────────────────────────── */

static void lcu_can_periodic_thread(void *p1, void *p2, void *p3) {
    uint8_t cnt_1000ms = 0;
    uint8_t light_render_ticks = 0;
    uint16_t blink_ticks = 0;
    bool blink_on = true;

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

        blink_ticks += 10;
        if (blink_ticks >= LIGHT_BLINK_INTERVAL_MS) {
            blink_ticks = 0;
            blink_on = !blink_on;
            lights_dirty = true;
        }

        light_render_ticks += 10;
        if (lights_dirty && light_render_ticks >= LIGHT_RENDER_INTERVAL_MS) {
            light_render_ticks = 0;
            lights_dirty = false;
            render_lights(blink_on);
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
        lights_dirty = true;
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

static void on_test_button(void) {
    LOG_INF("Test button pressed - lighting board LEDs");
    test_active = true;
    status_led_set_override(true);
    gpio_set(&can.rx_led);
    gpio_set(&can.tx_led);
    const uint32_t total_ms = 2000;
    const uint32_t phase_ms = total_ms / 3;

    /* R */
    led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels,
                             0x60, 0x00, 0x00);
    led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels,
                             0x60, 0x00, 0x00);
    k_sleep(K_MSEC(phase_ms));

    /* G */
    led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels,
                             0x00, 0x60, 0x00);
    led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels,
                             0x00, 0x60, 0x00);
    k_sleep(K_MSEC(phase_ms));

    /* B (remaining time to reach 2s total) */
    led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels,
                             0x00, 0x00, 0x60);
    led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels,
                             0x00, 0x00, 0x60);
    k_sleep(K_MSEC(total_ms - 2 * phase_ms));
    test_active = false;

    gpio_reset(&can.rx_led);
    gpio_reset(&can.tx_led);
    status_led_set_override(false);
    lights_dirty = true;
    LOG_INF("Test button: done");
}

/* ── Lights ───────────────────────────────────────────────────────────────── */

static void lcu_lights_init(void) {
    if (led_strip_init(lights.left_strip) != 0) {
        LOG_ERR("Left strip init failed");
    }
    if (led_strip_init(lights.right_strip) != 0) {
        LOG_ERR("Right strip init failed");
    }
    led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
    led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
    lights_dirty = true;
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
    update_status_led_for_can_state(get_current_can_state());
    if (test_button_init(&test_btn_gpio, on_test_button) != 0) {
        LOG_ERR("Test button initialization failed");
    }
}

void lcu_on_tick(void) {
    k_sleep(K_FOREVER);
}
