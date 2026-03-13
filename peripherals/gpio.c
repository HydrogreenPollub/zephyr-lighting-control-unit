#include "gpio.h"

LOG_MODULE_REGISTER(gpio);

int gpio_init(struct gpio_dt_spec *gpio, gpio_flags_t extra_flags) {

    if (!gpio_is_ready_dt(gpio)) {
        LOG_ERR("Error: GPIO device %s is not ready", gpio->port->name);
        return -1; // Indicate error
    } else {
        LOG_INF("GPIO device %s is ready", gpio->port->name);
    }

    int ret = gpio_pin_configure_dt(gpio, extra_flags);
    if (ret < 0) {
        LOG_ERR("Error %d: failed to configure pin %d", ret, gpio->pin);
        return -1; // Indicate error
    } else {
        LOG_INF("Successfully configured pin %d", gpio->pin);
    }

    return ret;
}

int gpio_reset(struct gpio_dt_spec *gpio) {
    return gpio_pin_set_dt(gpio, 0);
}

int gpio_set(struct gpio_dt_spec *gpio) {
    return gpio_pin_set_dt(gpio, 1);
}

int gpio_set_interrupt(struct gpio_dt_spec *gpio, gpio_flags_t flags, struct gpio_callback *gpio_cb_data, gpio_callback_handler_t handler) {

    gpio_init_callback(gpio_cb_data, handler, BIT(gpio->pin));
    gpio_add_callback(gpio->port, gpio_cb_data);
    int ret = gpio_pin_interrupt_configure_dt(gpio, flags);
    if (ret != 0) {
        printf("Error %d: failed to configure interrupt on %s pin %d\n",
            ret, gpio->port->name, gpio->pin);
        return 0;
    }
    printf("interrupt ok\n");
    return 1;
}
