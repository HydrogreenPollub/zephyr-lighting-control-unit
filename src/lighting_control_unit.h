//
// Created by inż. Dawid Pisarczyk on 28.12.2025.
//

#ifndef LIGHTING_CONTROL_UNIT_H
#define LIGHTING_CONTROL_UNIT_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/can.h>
#include "gpio.h"
#include "can.h"
#include "led_strip_ctrl.h"
#include "can_ids.h"

#define STRIP_NODE		DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS 16

typedef struct lcu_can_t {
    const struct device *device;
    struct gpio_dt_spec tx_led;
    struct gpio_dt_spec rx_led;
} lcu_can_t;

typedef struct lcu_lights_t {
    const struct device *const strip;
    struct led_rgb pixels[STRIP_NUM_PIXELS];
    uint8_t lights_mask;
    uint8_t num_pixels;
} lcu_lights_t;

void lcu_init(void);
void lcu_on_tick(void);

#endif //LIGHTING_CONTROL_UNIT_H
