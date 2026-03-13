#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include "lighting_control_unit.h"

LOG_MODULE_REGISTER(main);
//
// static struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(can_rx_led), gpios);
//
// bool bul = false;

// void blink_led() {
//     //ccu_rs485_init();
//     //ccu_can_init();
//
//     gpio_init(&status_led, GPIO_OUTPUT_INACTIVE);
//
//     while (true) {
//         gpio_pin_toggle_dt(&status_led);
//         k_msleep(500);
//         LOG_INF("%d", (int)bul);
//         bul = !bul;
//     }
// }

int main(void) {
    lcu_init();

    while (1) {
        // k_sleep(K_FOREVER);
        lcu_on_tick();
    }
    return 0;
}
