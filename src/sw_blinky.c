
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "blinky.h"
#include "sw_blinky.h"

static volatile struct gpio_dt_spec blinking_led; /* global variable to hold the currently blinking LED */
static volatile bool button_pressed_flag = false; /* flag to indicate button press, used for debouncing */
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(SWITCH0_NODE, gpios);

/*
 * This struct holds the callback metadata.
 * It gets registered with the GPIO driver and linked into
 * an internal list. Must be static/global — if it goes
 * out of scope, the driver holds a dangling pointer → crash.
 */
static struct gpio_callback btn_cb_data;

/* callback function for button press interrupt  changes the blinking LED */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins){
    printk("ISR: Button pressed! Toggling blinking LED.\n");

    if (blinking_led.pin == led0.pin) {
        blinking_led = led1;
    } else {
        blinking_led = led0;
    }
    button_pressed_flag = true;
}



int sw_blinky(void) {
    int ret;

    if (!gpio_is_ready_dt(&button0)) {
        printk("Error: GPIO device %s is not ready\n", button0.port->name);
        return -1;
    }

    ret = gpio_pin_configure_dt(&button0, GPIO_INPUT);
    if (ret < 0) {
        printk("Error: Failed to configure GPIO pin %d on device %s\n", button0.pin, button0.port->name);
        return -1;
    }

    led_init(); /* initialize the LEDs */
    printk("All LEDs initialized successfully!\n");

    /* setup button interrupt 

     * Configure the interrupt trigger:
     * GPIO_INT_EDGE_TO_ACTIVE = trigger on transition to logical active.
     * Since DT says ACTIVE_HIGH, this means rising edge.
     */
    gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&btn_cb_data, button_pressed, BIT(button0.pin));
    gpio_add_callback(button0.port, &btn_cb_data);
    printk("Button interrupt configured successfully!\n");



    /* put led 0 to high and led 1 to low */
    blinking_led = led0;

    while (1) {
        struct gpio_dt_spec current_led = blinking_led; /* snapshot cause the compiler complains about volatile */
        if (button_pressed_flag) {
            if (blinking_led.pin == led0.pin) {
                gpio_pin_set_dt(&led1, 0); /* ensure the other LED is off */
            } else {
                gpio_pin_set_dt(&led0, 0); /* ensure the other LED is off */
            }
            button_pressed_flag = false; /* reset the flag */
        }

        gpio_pin_toggle_dt(&current_led);    
        k_msleep(500); /* add a small delay to debounce the switch */    
    }

    return 0;
}