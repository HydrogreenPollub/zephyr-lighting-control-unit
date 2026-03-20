//
// Created by inż. Dawid Pisarczyk on 28.12.2025.
//

//LIGHT_HAZARD, LIGHT_BEAM, LIGHT_POSITION, LIGHT_RIGHT_INDICATOR, LIGHT_LEFT_INDICATOR

#include "lighting_control_unit.h"

#include "../peripherals/gpio.h"

LOG_MODULE_REGISTER(lighting_control_unit, LOG_LEVEL_INF);

static const struct can_filter lcu_can_filters[] = {
	CAN_FILTER(CAN_ID_BRAKE_PEDAL_VOLTAGE),
	CAN_FILTER(CAN_ID_BUTTONS_LIGHTS_MASK),
};

lcu_can_t can = {
    .device = DEVICE_DT_GET(DT_ALIAS(can)),
    .rx_led = GPIO_DT_SPEC_GET(DT_ALIAS(can_rx_led), gpios),
	.tx_led = GPIO_DT_SPEC_GET(DT_ALIAS(can_tx_led), gpios),
};

lcu_lights_t lights = {
	.strip = DEVICE_DT_GET(DT_ALIAS(led_strip)),
	.num_pixels = STRIP_NUM_PIXELS,
	.pixels = {},
	.lights_mask = 0,
};

static void lcu_can_rx_callback(const struct device *dev, struct can_frame *frame, void *user_data) {

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_INF("CAN ID: 0x%03X, Data: %u", frame->id, frame->data[0]);

	switch((can_id_t)frame->id) {
		case CAN_ID_BRAKE_PEDAL_VOLTAGE:
			LOG_INF("BRAKE_PEDAL_VOLTAGE");
			break;
		case CAN_ID_BUTTONS_LIGHTS_MASK:
			break;
		default:
			break;
	}
}

static void lcu_can_init() {
    can_init(can.device, HYDROGREEN_CAN_BAUD_RATE);
    gpio_init(&can.rx_led, GPIO_OUTPUT_INACTIVE);
	gpio_init(&can.tx_led, GPIO_OUTPUT_INACTIVE);

	for (int i = 0; i < ARRAY_SIZE(lcu_can_filters); i++) {
    	can_add_rx_filter_(can.device, lcu_can_rx_callback, &lcu_can_filters[i]);
  	}
}

static void lcu_lights_init() {
	led_strip_init(lights.strip);
	led_strip_clear_all_pixels(lights.strip, lights.pixels, lights.num_pixels);
}

#define LCU_LIGHTS_LOGIC_STACK_SIZE 512
#define LCU_LIGHTS_LOGIC_PRIORITY 5

K_THREAD_STACK_DEFINE(lcu_lights_logic_stack_area, LCU_LIGHTS_LOGIC_STACK_SIZE);
struct k_thread lcu_lights_logic_thread_data;

static void lcu_lights_logic_thread(void *p1, void *p2, void *p3) {

}

#define LCU_LIGHTS_DRIVER_STACK_SIZE 512
#define LCU_LIGHTS_DRIVER_PRIORITY 5

K_THREAD_STACK_DEFINE(lcu_lights_driver_stack_area, LCU_LIGHTS_DRIVER_STACK_SIZE);
struct k_thread lcu_lights_driver_thread_data;

static void lcu_lights_driver_thread(void *p1, void *p2, void *p3) {

	while (1) {
		lcu_on_tick();
	}
}



void lcu_init() {
    lcu_can_init();
	lcu_lights_init();

    // k_tid_t lcu_lights_logic_tid = k_thread_create(&lcu_lights_logic_thread_data, lcu_lights_logic_stack_area,
    //                                  K_THREAD_STACK_SIZEOF(lcu_lights_logic_stack_area),
    //                                  lcu_lights_logic_thread,
    //                                  NULL, NULL, NULL,
    //                                  LCU_LIGHTS_LOGIC_PRIORITY, 0, K_NO_WAIT);
    //
 //    k_tid_t lcu_lights_driver_tid = k_thread_create(&lcu_lights_driver_thread_data, lcu_lights_driver_stack_area,
 //                                     K_THREAD_STACK_SIZEOF(lcu_lights_driver_stack_area),
 //                                     lcu_lights_driver_thread,
 //                                     NULL, NULL, NULL,
 //                                     LCU_LIGHTS_DRIVER_PRIORITY, 0, K_NO_WAIT);
 //
	// k_thread_name_set(lcu_lights_driver_tid, "lights_driver");
}

void lcu_on_tick() {
	gpio_set(&can.tx_led);
	k_sleep(K_MSEC(100));
	gpio_reset(&can.tx_led);
	k_sleep(K_MSEC(100));
}





