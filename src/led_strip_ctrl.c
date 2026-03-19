//
// Created by inż.Dawid Pisarczyk on 09.02.2026.
//
#include "led_strip_ctrl.h"

LOG_MODULE_REGISTER(led_strip_ctrl, LOG_LEVEL_INF);

int led_strip_init(const struct device *strip) {
    if (device_is_ready(strip)) {
        LOG_INF("Found LED strip device %s", strip->name);
        return 0;
    } else {
        LOG_ERR("LED strip device %s is not ready", strip->name);
        return -ENODEV;
    }
}

int led_strip_set_pixel(const struct device *strip, struct led_rgb *pixels, size_t pixel_index, size_t num_pixels, uint8_t r, uint8_t g, uint8_t b) {
    if (pixel_index >= num_pixels) {
        return -ERANGE;
    }
    pixels[pixel_index] = RGB(r, g, b);
    return led_strip_update_rgb(strip, pixels, num_pixels);
}

int led_strip_set_all_pixels(const struct device *strip, struct led_rgb *pixels, size_t num_pixels, uint8_t r, uint8_t g, uint8_t b) {
    memset(pixels, 0x00, num_pixels * sizeof(struct led_rgb));

    for (size_t i = 0; i < num_pixels; i++) {
        pixels[i] = RGB(r, g, b);

    }
    return led_strip_update_rgb(strip, pixels, num_pixels);
}

int led_strip_clear_all_pixels(const struct device *strip, struct led_rgb *pixels, size_t num_pixels) {
    memset(pixels, 0x00, num_pixels * sizeof(struct led_rgb));
    for (size_t i = 0; i < num_pixels; i++) {
        pixels[i] = RGB(0, 0, 0);
    }
    return led_strip_update_rgb(strip, pixels, num_pixels);
}



