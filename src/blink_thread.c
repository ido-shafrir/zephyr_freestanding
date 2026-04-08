/*
this is the main file for blink thread 
it is responsible for 
- initializing the LEDs
- blinking the currently active LED
*/
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "blink_thread.h"

const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);


/* put all LEDs into a simple array for easier iteration */
const struct gpio_dt_spec all_leds[] = {led0, led1 };


/* initialize all LEDs */
int led_init(void){
    int ret;
    /* check if the GPIO device is ready before trying to use it */
    for (size_t i = 0; i < sizeof(all_leds) / sizeof(all_leds[0]); i++) {
        if (!gpio_is_ready_dt(&all_leds[i])){
            printk("Error: GPIO device %s is not ready\n", all_leds[i].port->name);
            return -1;
        }

        /* configure the GPIO pin as an output and set it to an initial state of 0 (off) */
        ret = gpio_pin_configure_dt(&all_leds[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            printk("Error %d: failed to configure GPIO pin %d on device %s\n", ret, all_leds[i].pin, all_leds[i].port->name);
            return ret;
        }

    }

    return 0;

}





K_THREAD_STACK_DEFINE(blink_stack, BLINK_STACK_SIZE);

struct k_thread blink_thread_data;

void blink_thread_entry(void *p1, void *p2, void *p3)
{
    int ret = led_init();
    if (ret < 0) {
        printk("Failed to initialize LEDs, exiting thread.\n");
        return;
    }

    while (1) {
        if (blink_enabled) {
            struct gpio_dt_spec current_led = blinking_led;
            gpio_pin_toggle_dt(&current_led);
        }
        k_msleep(500);
    }
}