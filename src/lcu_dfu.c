#include "lcu_dfu.h"
#include "status_led.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

static void on_dfu_start(void) { status_led_set(STATUS_LED_DFU); }
static void on_dfu_end(void)   { status_led_set(STATUS_LED_OPERATIONAL); }

static const struct can_dfu_cfg lcu_dfu_cfg = {
    .cmd_id   = LCU_DFU_CMD_ID,
    .data_id  = LCU_DFU_DATA_ID,
    .rsp_id   = LCU_DFU_RSP_ID,
    .can_dev  = DEVICE_DT_GET(DT_ALIAS(can)),
    .on_start = on_dfu_start,
    .on_end   = on_dfu_end,
};

void lcu_dfu_init(void) { can_dfu_init(&lcu_dfu_cfg); }
