//
// Created by inż.Dawid Pisarczyk on 09.02.2026.
//

#ifndef LED_STRIP_CTRL_H
#define LED_STRIP_CTRL_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <stdio.h>

#define SAMPLE_LED_UPDATE_DELAY 10
#define LED_MAX_BRIGHTNESS 0xFF
#define RGB(_r, _g, _b) ((struct led_rgb){ .r = (_r), .g = (_g), .b = (_b) })

int led_strip_init(const struct device *strip);
int led_strip_set_pixel(const struct device *strip, struct led_rgb *pixels, size_t pixel_index, size_t num_pixels, uint8_t r, uint8_t g, uint8_t b);
int led_strip_set_all_pixels(const struct device *strip, struct led_rgb *pixels, size_t num_pixels, uint8_t r, uint8_t g, uint8_t b);
int led_strip_clear_all_pixels(const struct device *strip, struct led_rgb *pixels, size_t num_pixels);

#endif //LED_STRIP_CTRL_H
