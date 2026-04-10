/** \file main.c */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "net.h"

/* 
Threads : 

* net_thread: Initializes the W5500 Ethernet controller and manages network connectivity. 
  - Waits for the W5500 to be ready, checks the network interface status, and logs the IP configuration.
  priority: 5 (medium priority for network tasks)


*/


LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);
int main(void)
{
    LOG_INF("Starting ICB Firmware...");

    k_thread_create(&net_thread_data, net_stack, NET_STACK_SIZE,
                    net_thread_entry, NULL, NULL, NULL,
                    NET_PRIORITY, 0, K_NO_WAIT);

    LOG_INF("Net thread created. Entering main loop.");

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
