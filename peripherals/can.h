#ifndef CAN_H
#define CAN_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "can_ids.h"
#include <stdio.h>


#define CAN_FILTER(_id)                                                        \
{ .id = (_id), .mask = CAN_STD_ID_MASK, .flags = 0U }

int can_send_(const struct device *can_dev, uint16_t id, uint8_t *data, uint8_t data_len);
int can_init(const struct device *can_dev, uint32_t baudrate);
int can_send_float(const struct device *can_dev, uint16_t id, float value);
int can_add_rx_filter_(const struct device *can_dev, can_rx_callback_t can_rx_callback, const struct can_filter *filter);

#endif //CAN_H
