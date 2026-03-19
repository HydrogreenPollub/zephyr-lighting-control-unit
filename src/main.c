#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include "lighting_control_unit.h"
#include "status_led.h"

LOG_MODULE_REGISTER(main);

int main(void) {
    LOG_INF("Lighting Control Unit has started");

    lcu_init();

    boot_write_img_confirmed();
    status_led_set(STATUS_LED_OPERATIONAL);

    while (1) {
        lcu_on_tick();
    }
    return 0;
}
