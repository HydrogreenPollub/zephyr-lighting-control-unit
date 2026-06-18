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
#include "candef.h"

#define LEFT_STRIP_NODE   DT_ALIAS(led_strip_left)
#define RIGHT_STRIP_NODE  DT_ALIAS(led_strip_right)

/* Pull the length dynamically from your Devicetree to stay synchronized */
#define STRIP_NUM_PIXELS  DT_PROP(LEFT_STRIP_NODE, chain_length)

typedef struct lcu_can_t {
    const struct device *device;
    struct gpio_dt_spec tx_led;
    struct gpio_dt_spec rx_led;
} lcu_can_t;

typedef struct lcu_lights_t {
    const struct device *left_strip;   /* Removed const here for easy runtime struct assignment */
    const struct device *right_strip;  /* Removed const here */
    struct led_rgb pixels_left[STRIP_NUM_PIXELS];
    struct led_rgb pixels_right[STRIP_NUM_PIXELS];
    struct candef_mcu_lighting_t lighting;
    uint8_t num_pixels;
} lcu_lights_t;

void lcu_init(void);
void lcu_on_tick(void);

#endif //LIGHTING_CONTROL_UNIT_H
