#include "can.h"

#include <string.h>

LOG_MODULE_REGISTER(can);


void can_tx_callback(const struct device *dev, int error, void *user_data) {
    char *sender = (char *) user_data;

    if (error == -ENETUNREACH) {
        LOG_ERR("CAN network unreachable, error: %d, sender: %s", error, sender);
        return;
    }

    if (error != 0) {
        LOG_ERR("Sending failed [%d] Sender: %s", error, sender);
    }

    LOG_INF("Send succes");
}

int can_send_(const struct device *can_dev, uint16_t id, uint8_t *data, uint8_t data_len) {
    struct can_frame frame = { 0 };
    frame.id = id;
    frame.dlc = data_len;
    frame.flags = 0;

    memcpy(frame.data, data, data_len);

    return can_send(can_dev, &frame, K_FOREVER, NULL, NULL);
}

int can_send_float(const struct device *can_dev, uint16_t id, float value) {
    return can_send_(can_dev, id, (uint8_t *) &value, sizeof(float));
}

// void can_rx_callback(const struct device *dev, struct can_frame *frame, void *user_data) {
//     char *receiver = (char *) user_data;
//     printf("Odebrano:%s; %u\n", receiver, frame->data[0]);
// }

int can_add_rx_filter_(const struct device *can_dev, can_rx_callback_t can_rx_callback, const struct can_filter *filter) {
    int  filter_id = can_add_rx_filter(can_dev, can_rx_callback, NULL, filter);
    if (filter_id < 0) {
        LOG_ERR("Unable to add rx filter [%d]", filter_id);
    }
    return filter_id;
}

// struct can_filter my_filter = {
//     .flags = 0U,
//     .id = 0x123,
//     .mask = CAN_STD_ID_MASK
// };

int can_init(const struct device *can_dev, uint32_t baudrate) {
    struct can_timing timing;
    int ret;
    ret = can_calc_timing(can_dev, &timing, baudrate, 875);
    if (ret > 0) {
        LOG_INF("Sample-Point error: %d", ret);
    }
    if (ret < 0) {
        LOG_ERR("Failed to calc a valid timing");
        return -1;
    }

    ret = can_set_timing(can_dev, &timing);
    if (ret != 0) {
        LOG_ERR("Failed to set timing");
    }

    ret = can_start(can_dev);
    if (ret != 0) {
        LOG_ERR("Failed to start CAN controller");
    }
    if (ret == 0) {
        LOG_INF("CAN initialized correctly");
    }
    return ret;
}
