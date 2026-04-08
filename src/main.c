#include <zephyr/kernel.h>
#include "blink_thread.h"
#include "switch_thread.h"
#include "state_machines.h"

/* mutex to protect access to shared state variables 
similar to python's threading.Lock, this ensures that only one thread can access the shared state variables at a time
*/
K_MUTEX_DEFINE(blink_led_mutex);  
K_MUTEX_DEFINE(blink_enabled_mutex);  

/* shared state variables */
volatile bool blink_enabled = true;
volatile struct gpio_dt_spec blinking_led;

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
