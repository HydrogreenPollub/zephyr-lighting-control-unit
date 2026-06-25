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

#define TURN_SIGNAL_ANIMATION_PERIOD_MS 750

static uint64_t render_timer;
static bool running_lights_modifier;
static bool brake_lights_modifier;
static int8_t indicator_direction_modifier;
static bool hazard_lights_modifier;

#ifdef LCU_FRONT_PCB
    #define TURN_SIGNAL_SYMMETRY_SPLIT_POINT 23
    #ifndef LCU_INSTANCE_ID
        #define LCU_INSTANCE_ID CANDEF_LCU_STATUS_INSTANCE_FRONT_CHOICE
    #endif
#elif defined(LCU_REAR_PCB)
    #define REAR_TURN_SIGNAL_WIDTH 15
    #define REAR_TURN_SIGNAL_STOP_LIGHT_MARGIN 2
    #define LCU_INSTANCE_ID CANDEF_LCU_STATUS_INSTANCE_BACK_CHOICE
#endif

/* SWU_LCU_INPUTS (0x302, extended) - lighting commands from steering wheel */
static const struct can_filter swu_lcu_inputs_filter = {
    .id    = CANDEF_SWU_LCU_INPUTS_FRAME_ID,
    .mask  = CAN_EXT_ID_MASK,
    .flags = CAN_FRAME_IDE,
};

/* Single filter matches both CMD and DATA DFU IDs */
static const struct can_filter dfu_filter = {
    .id    = LCU_DFU_CMD_ID,
    .mask  = 0x1FFFFFFEU,
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
    .pixels_left  = {{0}},
    .pixels_right = {{0}},
};

static volatile bool test_active;

static struct candef_mcu_lighting_t current_lighting = {0};
static struct candef_mcu_lighting_t previous_lighting = {0};
static volatile bool lighting_update_flag = false;



/* ── LED work ─────────────────────────────────────────────────────────────── */

static void tx_led_off_handler(struct k_work *work)
{
    if (!test_active) gpio_reset(&can.tx_led);
}

static void rx_led_off_handler(struct k_work *work)
{
    if (!test_active) gpio_reset(&can.rx_led);
}

static void lcu_can_rx_led_pulse(void)
{
    gpio_set(&can.rx_led);
    k_work_reschedule(&rx_led_off_work, K_MSEC(50));
}

static void lcu_can_tx_led_pulse(void)
{
    gpio_set(&can.tx_led);
    k_work_reschedule(&tx_led_off_work, K_MSEC(50));
}

/* ── CAN state ────────────────────────────────────────────────────────────── */

static const char *can_state_name(enum can_state state)
{
    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:  return "error-active";
    case CAN_STATE_ERROR_WARNING: return "error-warning";
    case CAN_STATE_ERROR_PASSIVE: return "error-passive";
    case CAN_STATE_BUS_OFF:       return "bus-off";
    case CAN_STATE_STOPPED:       return "stopped";
    default:                      return "unknown";
    }
}

static enum can_state get_current_can_state(void)
{
    enum can_state state = lcu_can_state;
    struct can_bus_err_cnt err_cnt;

    if (can_get_state(can.device, &state, &err_cnt) == 0) {
        lcu_can_state = state;
    }
    return state;
}

static void update_status_led_for_can_state(enum can_state state)
{
    switch (state) {
    case CAN_STATE_ERROR_ACTIVE:
        status_led_set(STATUS_LED_OPERATIONAL);
        break;
    case CAN_STATE_ERROR_WARNING:
    case CAN_STATE_ERROR_PASSIVE:
    case CAN_STATE_BUS_OFF:
    default:
        status_led_set(STATUS_LED_WARNING);
        break;
    }
}

static void bus_off_recovery_handler(struct k_work *work)
{
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
                                void *user_data)
{
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

static bool can_tx_allowed(void)
{
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

/* ── TX thread ────────────────────────────────────────────────────────────── */

static void lcu_can_tx_callback(const struct device *dev, int error, void *user_data)
{
    can_tx_result = error;
    k_sem_give(&can_tx_done_sem);
}

static void lcu_can_tx_thread(void *p1, void *p2, void *p3)
{
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

/* ── enqueue ──────────────────────────────────────────────────────────────── */

static void enqueue_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    struct can_frame frame = {};

    if (len > sizeof(frame.data)) {
        LOG_WRN("CAN payload too big: %u", len);
        return;
    }

    frame.id    = id;
    frame.dlc   = len;
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

/* ── status CAN ───────────────────────────────────────────────────────────── */

static void send_lcu_status(void)
{
    lcu_can_tx_led_pulse();
    struct candef_lcu_status_t frame = {
        .instance        = LCU_INSTANCE_ID,
        .fault           = CANDEF_LCU_STATUS_FAULT_OK_CHOICE,
        .headlight       = current_lighting.headlight,
        .position_light  = current_lighting.position_light,
        .brake_light     = current_lighting.brake_light,
        .left_indicator  = current_lighting.left_indicator,
        .right_indicator = current_lighting.right_indicator,
        .hazard          = current_lighting.hazard,
    };
    PACK_AND_ENQUEUE(LCU_STATUS, lcu_status, &frame);
}

/* ── Periodic thread ──────────────────────────────────────────────────────── */

static void lcu_can_periodic_thread(void *p1, void *p2, void *p3)
{
    uint8_t cnt_1000ms = 0;

    while (1) {
        k_sleep(K_MSEC(10));

        if (can_dfu_is_active()) {
            cnt_1000ms = 0;
            continue;
        }

        cnt_1000ms++;
        if (cnt_1000ms >= 50) {
            cnt_1000ms = 0;

            send_lcu_status();
        }
    }
}

/* ── CAN RX ───────────────────────────────────────────────────────────────── */

static void lcu_swu_lcu_inputs_rx_cb(const struct device *dev,
                                     struct can_frame *frame,
                                     void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    lcu_can_rx_led_pulse();

    if (frame->dlc >= CANDEF_SWU_LCU_INPUTS_LENGTH) {
        struct candef_swu_lcu_inputs_t swu_inputs;

        previous_lighting = current_lighting;
        candef_swu_lcu_inputs_unpack(&swu_inputs, frame->data, frame->dlc);
        current_lighting.headlight = swu_inputs.beam;
        current_lighting.position_light = swu_inputs.position;
        current_lighting.left_indicator = swu_inputs.left_indicator;
        current_lighting.right_indicator = swu_inputs.right_indicator;
        current_lighting.hazard = swu_inputs.hazard;
        current_lighting.brake_light = swu_inputs.brake_light;

        LOG_INF("SWU_LCU_INPUTS received: H=%d P=%d B=%d L=%d R=%d Hz=%d",
                current_lighting.headlight, current_lighting.position_light,
                current_lighting.brake_light, current_lighting.left_indicator,
                current_lighting.right_indicator, current_lighting.hazard);
        if (current_lighting.headlight == previous_lighting.headlight &&
            current_lighting.position_light == previous_lighting.position_light &&
            current_lighting.brake_light == previous_lighting.brake_light &&
            current_lighting.left_indicator == previous_lighting.left_indicator &&
            current_lighting.right_indicator == previous_lighting.right_indicator &&
            current_lighting.hazard == previous_lighting.hazard) {
            lighting_update_flag = 0;
        } else {
            lighting_update_flag = 1;
        }
    }
}

static void lcu_dfu_rx_cb(const struct device *dev,
                           struct can_frame *frame,
                           void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    lcu_can_rx_led_pulse();
    can_dfu_on_frame(frame);
}

/* ── Light rendering (front/rear, unchanged from doc 2) ───────────────────── */

#ifdef LCU_FRONT_PCB

static void render_running_lights(void) {
    led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels, running_lights_modifier*100, running_lights_modifier*100, running_lights_modifier*100);
    led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels, running_lights_modifier*100, running_lights_modifier*100, running_lights_modifier*100);
}

static void render_indicator(int8_t direction) {
    if (direction == -1) {
        float num_of_leds_on = ((float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT);
        if (num_of_leds_on == lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) {
            led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.left_strip, lights.pixels_left, 0, (int)min(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT, max(0,num_of_leds_on-1)), lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.left_strip, lights.pixels_left, max(TURN_SIGNAL_SYMMETRY_SPLIT_POINT, lights.num_pixels-(int)((num_of_leds_on*TURN_SIGNAL_SYMMETRY_SPLIT_POINT)/(float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT))), lights.num_pixels-1, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (direction == 1) {
        float num_of_leds_on = ((float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT);
        if (num_of_leds_on == lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) {
            led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.right_strip, lights.pixels_right, 0, (int)min(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT, max(0,num_of_leds_on-1)), lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.right_strip, lights.pixels_right, max(TURN_SIGNAL_SYMMETRY_SPLIT_POINT, lights.num_pixels-(int)((num_of_leds_on*TURN_SIGNAL_SYMMETRY_SPLIT_POINT)/(float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT))), lights.num_pixels-1, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (direction == 0) {
        float num_of_leds_on = ((float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT);
        if (num_of_leds_on == lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT) {
            led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.right_strip, lights.pixels_right, 0, (int)min(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT, max(0,num_of_leds_on-1)), lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.right_strip, lights.pixels_right, max(TURN_SIGNAL_SYMMETRY_SPLIT_POINT, lights.num_pixels-(int)((num_of_leds_on*TURN_SIGNAL_SYMMETRY_SPLIT_POINT)/(float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT))), lights.num_pixels-1, lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.left_strip, lights.pixels_left, 0, (int)min(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT, max(0,num_of_leds_on-1)), lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.left_strip, lights.pixels_left, max(TURN_SIGNAL_SYMMETRY_SPLIT_POINT, lights.num_pixels-(int)((num_of_leds_on*TURN_SIGNAL_SYMMETRY_SPLIT_POINT)/(float)(lights.num_pixels-TURN_SIGNAL_SYMMETRY_SPLIT_POINT))), lights.num_pixels-1, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (k_uptime_get() - render_timer > (int)((double)TURN_SIGNAL_ANIMATION_PERIOD_MS * 1.1)) {
        led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels, max(running_lights_modifier*100, brake_lights_modifier*255), max(running_lights_modifier*100, brake_lights_modifier*255), max(running_lights_modifier*100, brake_lights_modifier*255));
        led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels, max(running_lights_modifier*100, brake_lights_modifier*255), max(running_lights_modifier*100, brake_lights_modifier*255), max(running_lights_modifier*100, brake_lights_modifier*255));
        render_timer = k_uptime_get();
    }
}

static void render_brake_lights(void) {}

#elif defined(LCU_REAR_PCB)

static void render_running_lights(void) {
    led_strip_set_range(lights.left_strip, lights.pixels_left, 0, REAR_TURN_SIGNAL_WIDTH-1, lights.num_pixels, max(100*running_lights_modifier, brake_lights_modifier*255), 0, 0);
    led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels-1, lights.num_pixels, max(100*running_lights_modifier, brake_lights_modifier*255), 0, 0);
}

static void render_indicator(int8_t direction) {
    if (direction == -1) {
        float num_of_leds_on = (REAR_TURN_SIGNAL_WIDTH * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, REAR_TURN_SIGNAL_WIDTH);
        if (num_of_leds_on >= REAR_TURN_SIGNAL_WIDTH) {
            led_strip_set_range(lights.left_strip, lights.pixels_left, 0, REAR_TURN_SIGNAL_WIDTH - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.left_strip, lights.pixels_left, REAR_TURN_SIGNAL_WIDTH - num_of_leds_on, REAR_TURN_SIGNAL_WIDTH - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (direction == 1) {
        float num_of_leds_on = (REAR_TURN_SIGNAL_WIDTH * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, REAR_TURN_SIGNAL_WIDTH);
        if (num_of_leds_on >= REAR_TURN_SIGNAL_WIDTH) {
            led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels - REAR_TURN_SIGNAL_WIDTH + num_of_leds_on, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (direction == 0) {
        float num_of_leds_on = (REAR_TURN_SIGNAL_WIDTH * (float)(k_uptime_get() - render_timer))/(float)TURN_SIGNAL_ANIMATION_PERIOD_MS;
        num_of_leds_on = min(num_of_leds_on, REAR_TURN_SIGNAL_WIDTH);
        if (num_of_leds_on >= REAR_TURN_SIGNAL_WIDTH) {
            led_strip_set_range(lights.left_strip, lights.pixels_left, 0, REAR_TURN_SIGNAL_WIDTH - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
        } else {
            led_strip_set_range(lights.left_strip, lights.pixels_left, REAR_TURN_SIGNAL_WIDTH - num_of_leds_on, REAR_TURN_SIGNAL_WIDTH - 1, lights.num_pixels, 0XFF, 0x32, 0x00);
            led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels - REAR_TURN_SIGNAL_WIDTH + num_of_leds_on, lights.num_pixels, 0XFF, 0x32, 0x00);
        }
    }

    if (k_uptime_get() - render_timer > (int)((double)TURN_SIGNAL_ANIMATION_PERIOD_MS * 1.1)) {
        led_strip_set_range(lights.left_strip, lights.pixels_left, 0, REAR_TURN_SIGNAL_WIDTH-1, lights.num_pixels, max(running_lights_modifier*100, brake_lights_modifier*255), 0, 0);
        led_strip_set_range(lights.left_strip, lights.pixels_left, lights.num_pixels-REAR_TURN_SIGNAL_WIDTH, lights.num_pixels - 1, lights.num_pixels, max(running_lights_modifier*100, brake_lights_modifier*255), 0, 0);
        render_timer = k_uptime_get();
    }
}

static void render_brake_lights(void) {
    led_strip_set_range(lights.left_strip, lights.pixels_left, REAR_TURN_SIGNAL_WIDTH + REAR_TURN_SIGNAL_STOP_LIGHT_MARGIN, lights.num_pixels - REAR_TURN_SIGNAL_WIDTH - REAR_TURN_SIGNAL_STOP_LIGHT_MARGIN - 1, lights.num_pixels, brake_lights_modifier*255, 0, 0);
}

#endif

/* ── TEST BUTTON ──────────────────────────────────────────────────────────── */

static void on_test_button(void)
{
    LOG_INF("Test button");

    test_active = true;
    status_led_set_override(true);

    gpio_set(&can.rx_led);
    gpio_set(&can.tx_led);

    for (int j = 0; j < (int)(lights.num_pixels); j++) {
        led_strip_set_pixel(lights.left_strip, lights.pixels_left, (size_t)j, lights.num_pixels, 0x60*(bool)((j%3==0)), 0x60*(bool)((j%3==1)), 0x60*(bool)((j%3==2)));
        led_strip_set_pixel(lights.right_strip, lights.pixels_right, (size_t)j, lights.num_pixels, 0x60*(bool)((j%3==0)), 0x60*(bool)((j%3==1)), 0x60*(bool)((j%3==2)));
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        k_sleep(K_MSEC(10));
    }
    k_sleep(K_MSEC(100));
    led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
    led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
    led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
    led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

    gpio_reset(&can.rx_led);
    gpio_reset(&can.tx_led);

    uint64_t temp_timer;

    while (1) {
        /* running lights only */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 1;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* left indicator, no running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 0;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(-1);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* left indicator, with running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 1;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(-1);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* right indicator, no running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 0;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(1);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* right indicator, with running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 1;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(1);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* hazard lights, no running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 0;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(0);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);

        /* hazard lights, with running lights */
        temp_timer = k_uptime_get();
        render_timer = temp_timer;
        running_lights_modifier = 1;
        brake_lights_modifier = 0;
        while (k_uptime_get() - temp_timer < 5000) {
            render_running_lights();
            render_indicator(0);
            render_brake_lights();
            if (k_uptime_get() - temp_timer > 2500) brake_lights_modifier = 1;
            led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
    }

    status_led_set_override(false);
    test_active = false;
}

/* ── LIGHT INIT ───────────────────────────────────────────────────────────── */

static void lcu_lights_init(void)
{
    led_strip_init(lights.left_strip);
    led_strip_init(lights.right_strip);

    for (int i = 0; i < 256; i++) {
        led_strip_set_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels, i, i, i);
        led_strip_set_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels, i, i, i);
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
    }
    k_sleep(K_MSEC(1000));
    led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
    led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
    led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
    led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
}

/* ── INIT ─────────────────────────────────────────────────────────────────── */

void lcu_init(void)
{
    can_init(can.device, HYDROGREEN_CAN_BAUD_RATE);

    gpio_init(&can.rx_led, GPIO_OUTPUT_INACTIVE);
    gpio_init(&can.tx_led, GPIO_OUTPUT_INACTIVE);

    k_work_init_delayable(&tx_led_off_work, tx_led_off_handler);
    k_work_init_delayable(&rx_led_off_work, rx_led_off_handler);
    k_work_init_delayable(&bus_off_recovery_work, bus_off_recovery_handler);

    can_set_state_change_callback(can.device, can_state_change_cb, NULL);

    can_add_rx_filter_(can.device, lcu_swu_lcu_inputs_rx_cb, &swu_lcu_inputs_filter);
    can_add_rx_filter_(can.device, lcu_dfu_rx_cb, &dfu_filter);

    lcu_dfu_init();

    k_tid_t tx_tid = k_thread_create(
        &lcu_can_tx_thread_data,
        lcu_can_tx_stack,
        K_THREAD_STACK_SIZEOF(lcu_can_tx_stack),
        lcu_can_tx_thread,
        NULL, NULL, NULL,
        LCU_CAN_TX_THREAD_PRIORITY,
        0,
        K_NO_WAIT);
    k_thread_name_set(tx_tid, "can_tx");

    k_tid_t periodic_tid = k_thread_create(
        &lcu_can_periodic_thread_data,
        lcu_can_periodic_stack,
        K_THREAD_STACK_SIZEOF(lcu_can_periodic_stack),
        lcu_can_periodic_thread,
        NULL, NULL, NULL,
        LCU_CAN_PERIODIC_PRIORITY,
        0,
        K_NO_WAIT);
    k_thread_name_set(periodic_tid, "can_periodic");

    lcu_lights_init();

    static const struct gpio_dt_spec status_led_gpio =
        GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
    static const struct gpio_dt_spec test_btn_gpio =
        GPIO_DT_SPEC_GET(DT_ALIAS(button_test), gpios);

    status_led_init(&status_led_gpio, NULL);
    update_status_led_for_can_state(get_current_can_state());
    test_button_init(&test_btn_gpio, on_test_button);
}

void lcu_on_tick(void)
{
    while (1) {
        if (lighting_update_flag) {
            lighting_update_flag = false;
            render_timer = k_uptime_get();

            running_lights_modifier = current_lighting.position_light;
            brake_lights_modifier = current_lighting.brake_light;
            indicator_direction_modifier = current_lighting.left_indicator ? 1 : (current_lighting.right_indicator ? -1 : 0);
            hazard_lights_modifier = current_lighting.hazard;

            led_strip_clear_all_pixels(lights.left_strip, lights.pixels_left, lights.num_pixels);
            led_strip_clear_all_pixels(lights.right_strip, lights.pixels_right, lights.num_pixels);
        }
        render_running_lights();
        if (hazard_lights_modifier) {
            render_indicator(0);
        }else if (indicator_direction_modifier) {
            render_indicator(indicator_direction_modifier);
        }
        render_brake_lights();
        led_strip_flush(lights.left_strip, lights.pixels_left, lights.num_pixels);
        led_strip_flush(lights.right_strip, lights.pixels_right, lights.num_pixels);
        k_sleep(K_MSEC(10));
    }
   // k_sleep(K_FOREVER);
}
