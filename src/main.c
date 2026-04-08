#include <zephyr/kernel.h>
#include "blink_thread.h"
#include "switch_thread.h"

volatile bool blink_enabled = true;
volatile struct gpio_dt_spec blinking_led; /* global variable to hold the currently blinking LED */


int main(void)
{
    printk("Starting Zephyr Blinky with Threads Example\n");
    printk("blinking thread priority: %d, button thread priority: %d\n", BLINK_PRIORITY, BTN_PRIORITY);

    /* Initialize the blinking LED to led0 by default */
    blinking_led = led0;

    /* Create the blink thread */
    k_thread_create(&blink_thread_data, blink_stack, BLINK_STACK_SIZE,
                    blink_thread_entry, NULL, NULL, NULL,
                    BLINK_PRIORITY, 0, K_NO_WAIT);

    /* Create the button thread */
    k_thread_create(&btn_thread_data, btn_stack, BTN_STACK_SIZE,
                    btn_thread_entry, NULL, NULL, NULL,
                    BTN_PRIORITY, 0, K_NO_WAIT);
    printk("System started: 3 threads (main + button + blink)\n");

    while (1) {
        /* Main thread can perform other tasks or just sleep */
        k_sleep(K_FOREVER);
    }
    return 0;
}
