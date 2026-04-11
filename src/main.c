/** \file main.c */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "net.h"
#include "uart.h"

/* 
Threads : 

* net_thread: Initializes the W5500 Ethernet controller and manages network connectivity. 
  - Waits for the W5500 to be ready, checks the network interface status, and logs the IP configuration.
  priority: 5 (medium priority for network tasks)

* uart_thread: Handles UART serial command interface.
  - Interrupt-driven RX on the console UART (usart3 via ST-Link).
  - Dispatches commands (e.g. 'ping' → 'pong').
  priority: 4 (higher than net, responsive to user input)

*/


LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/**
 * @brief Application entry point and thread orchestrator.
 *
 * Creates the net and UART threads, then sleeps forever.
 * All real work is done in the spawned threads.
 *
 * @return 0 (never reached).
 */
int main(void)
{
    LOG_INF("Starting ICB Firmware...");

    k_thread_create(&net_thread_data, net_stack, NET_STACK_SIZE,
                    net_thread_entry, NULL, NULL, NULL,
                    NET_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&uart_thread_data, uart_stack, UART_STACK_SIZE,
                    uart_thread_entry, NULL, NULL, NULL,
                    UART_PRIORITY, 0, K_NO_WAIT);

    LOG_INF("All threads created. Entering main loop.");

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
