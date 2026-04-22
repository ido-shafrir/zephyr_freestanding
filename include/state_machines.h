#ifndef STATE_MACHINES_H
#define STATE_MACHINES_H

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

extern struct k_mutex blink_led_mutex; /* mutex to protect access to blinking LED state */
extern struct k_mutex blink_enabled_mutex; /* mutex to protect access to blink enabled state */
extern volatile bool blink_enabled;
extern volatile struct gpio_dt_spec blinking_led;

#endif /* STATE_MACHINES_H */
