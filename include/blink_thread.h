
#ifndef BLINK_THREAD_H
#define BLINK_THREAD_H

#define BLINK_STACK_SIZE 1024
#define BLINK_PRIORITY   3     /* Lower priority than button */

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
extern const struct gpio_dt_spec led0;
extern const struct gpio_dt_spec led1;
extern volatile bool blink_enabled;
extern volatile struct gpio_dt_spec blinking_led;

extern struct k_thread blink_thread_data;
extern k_thread_stack_t blink_stack[];

/* entry point for the blink thread, this is where the blinking logic will be implemented */
void blink_thread_entry(void *p1, void *p2, void *p3); 
#endif /* BLINK_THREAD_H */

