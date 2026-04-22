#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "switch_thread.h"
#include "blink_thread.h"
#include "state_machines.h"

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(SWITCH0_NODE, gpios);

static struct gpio_callback btn_cb_data;
K_SEM_DEFINE(btn_sem, 0, 1);


/* callback function for button press interrupt  changes the blinking LED */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins){
    k_sem_give(&btn_sem);
}

/* handler for button press events  flipping the blinking LED */
void button_handler(void) {
    k_mutex_lock(&blink_led_mutex, K_FOREVER); /* take lock */
    printk("ISR: Button pressed! Toggling blinking LED.\n");
    if (blinking_led.pin == led0.pin) {
        blinking_led = led1;
        gpio_pin_set_dt(&led0, 0) ; /* ensure the other LED is off */
    } else {
        blinking_led = led0;
        gpio_pin_set_dt(&led1, 0); /* ensure the other LED is off */
    }
    k_mutex_unlock(&blink_led_mutex); /* release lock */
}

int init(void) {
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

    /* setup button interrupt */
    gpio_init_callback(&btn_cb_data, button_pressed, BIT(button0.pin));
    gpio_add_callback(button0.port, &btn_cb_data);
    gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);

    return 0;
}

K_THREAD_STACK_DEFINE(btn_stack, BTN_STACK_SIZE);
struct k_thread btn_thread_data;

void btn_thread_entry(void *p1, void *p2, void *p3)
{
    int ret = init();
    if (ret < 0) {  
        printk("Failed to initialize button GPIO, exiting thread.\n");
        return;
    }

    /*
     * This function IS the thread. When it returns, the thread dies.
     * So we loop forever — just like main().
     */
    while (1) {
        k_sem_take(&btn_sem, K_FOREVER);
        button_handler();
    }
}