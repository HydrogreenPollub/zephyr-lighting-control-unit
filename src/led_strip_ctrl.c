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

/* ── Buffer-only writes (no hardware update) ────────────────────────────── */

int led_strip_set_pixel(const struct device *strip, struct led_rgb *pixels,
                        size_t pixel_index, size_t num_pixels,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (pixel_index >= num_pixels) {
        return -ERANGE;
    }
    pixels[pixel_index] = RGB(r, g, b);
    return 0;
}

int led_strip_set_all_pixels(const struct device *strip, struct led_rgb *pixels,
                              size_t num_pixels,
                              uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i < num_pixels; i++) {
        pixels[i] = RGB(r, g, b);
    }
    return 0;
}

int led_strip_set_range(const struct device *strip, struct led_rgb *pixels,
                        size_t range_start, size_t range_end, size_t num_pixels,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (range_start >= num_pixels) {
        return -ERANGE;
    }
    if (range_end >= num_pixels) {
        range_end = num_pixels - 1;
    }
    if (range_start > range_end) {
        return 0;
    }
    for (size_t i = range_start; i <= range_end; i++) {
        pixels[i] = RGB(r, g, b);
    }
    return 0;
}

int led_strip_clear_all_pixels(const struct device *strip, struct led_rgb *pixels,
                                size_t num_pixels) {
    memset(pixels, 0x00, num_pixels * sizeof(struct led_rgb));
    return 0;
}

/* ── Hardware flush ─────────────────────────────────────────────────────── */

int led_strip_flush(const struct device *strip, struct led_rgb *pixels,
                    size_t num_pixels) {
    return led_strip_update_rgb(strip, pixels, num_pixels);
}