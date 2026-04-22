#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "blinky.h"

/* resolve the LED0, and LED1 aliases to a device and pin number
 * on NUCLEO-H753ZI, led0 is aliased to &green_led, led1 is aliased to &blue_led
 * all of this can be found in the board's dts file.
 * this is not run-time code, the preprocessor will resolve these to the correct values at compile time.
 * LED0_NODE and LED1_NODE are defined in blinky.h
 */


/*
* GPIO_DT_SPEC_GET extracts the GPIO controller device and pin information from the device tree
* node specified by the first argument (e.g., LED0_NODE) and the second argument (gpios).
* It returns a struct gpio_dt_spec which contains the device pointer, pin number, and flags needed to control the GPIO pin.
*/
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

/* main blinky function, initializes the LEDs and then toggles them in an infinite loop with a delay */
int blinky(void) {
    int ret = led_init();
    if (ret < 0) {
        return ret;
    }

    printk("All LEDs initialized successfully!\n");

    while (1) {
        /* toggle pins with 1 0.5 sec delay */

        for (size_t i = 0; i < sizeof(all_leds) / sizeof(all_leds[0]); i++) {
            gpio_pin_toggle_dt(&all_leds[i]);
            k_msleep(500);
        }
           
    }    
    return 0;
}