#include <zephyr/kernel.h>
#include "blink_thread.h"
#include "switch_thread.h"
#include "state_machines.h"

/*
 * ============================================================================
 * Optional module headers
 * ----------------------------------------------------------------------------
 * Uncomment the includes for the modules you enable in CMakeLists.txt and
 * prj.conf.  See docs/feature_selection_guide.md for the per-feature recipe.
 * ============================================================================
 */
/* #include "config_store.h" */
/* #include "w5500_net.h" */
/* #include "command_uart.h" */
/* #include "rest_api.h" */
/* #include "ota.h" */
/* #include "event_log.h" */
/* #include "time_service.h" */

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

    /*
     * ========================================================================
     * Optional module thread spawns
     * ------------------------------------------------------------------------
     * Each block below corresponds to an optional feature module shipped in
     * src/ but NOT compiled by default.  Add the source file to
     * CMakeLists.txt, uncomment the matching block in prj.conf, and
     * uncomment the spawn here.  See docs/feature_selection_guide.md.
     *
     * Recommended spawn order (dependencies first):
     *   config_store -> w5500_net -> time_service -> event_log -> command_uart
     *   -> rest_api -> ota
     * ========================================================================
     */

    /* k_thread_create(&config_store_thread_data, config_store_stack,
     *                 CONFIG_STORE_STACK_SIZE, config_store_thread_entry,
     *                 NULL, NULL, NULL, CONFIG_STORE_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&net_thread_data, net_stack, NET_STACK_SIZE,
     *                 net_thread_entry, NULL, NULL, NULL,
     *                 NET_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&time_service_thread_data, time_service_stack,
     *                 TIME_SERVICE_STACK_SIZE, time_service_thread_entry,
     *                 NULL, NULL, NULL, TIME_SERVICE_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&event_log_thread_data, event_log_stack,
     *                 EVENT_LOG_STACK_SIZE, event_log_thread_entry,
     *                 NULL, NULL, NULL, EVENT_LOG_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&command_uart_thread_data, command_uart_stack,
     *                 COMMAND_UART_STACK_SIZE, command_uart_thread_entry,
     *                 NULL, NULL, NULL, COMMAND_UART_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&rest_api_thread_data, rest_api_stack,
     *                 REST_API_STACK_SIZE, rest_api_thread_entry,
     *                 NULL, NULL, NULL, REST_API_PRIORITY, 0, K_NO_WAIT); */

    /* k_thread_create(&ota_thread_data, ota_stack, OTA_STACK_SIZE,
     *                 ota_thread_entry, NULL, NULL, NULL,
     *                 OTA_PRIORITY, 0, K_NO_WAIT); */

    printk("System started: 3 threads (main + button + blink)\n");

    while (1) {
        /* Main thread can perform other tasks or just sleep */
        k_sleep(K_FOREVER);
    }
    return 0;
}
