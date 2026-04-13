/** \file main.c */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "w5500_net.h"
#include "command_uart.h"
#include "rest_api.h"
#include "ota.h"

/* 
Threads : 

* net_thread: Initializes the W5500 Ethernet controller and manages network connectivity. 
  - Waits for the W5500 to be ready, checks the network interface status, and logs the IP configuration.
  priority: 5 (medium priority for network tasks)

* uart_thread: Handles UART serial command interface.
  - Interrupt-driven RX on the console UART (usart3 via ST-Link).
  - Dispatches commands (e.g. 'ping' → 'pong').
  priority: 4 (higher than net, responsive to user input)

* rest_api_thread: Starts the HTTP REST API server on port 80.
  - Serves JSON endpoints (/api/ping, /api/help) and an HTML index.
  - Uses Zephyr's built-in HTTP server subsystem.
  priority: 7 (lower than net/uart, background service)

* ota_thread: MCUmgr OTA update and health-based image confirmation.
  - Waits for all modules (net, uart, rest_api) to report ready.
  - Confirms the running image with MCUboot once all healthy.
  - MCUmgr SMP UDP server runs in its own Zephyr-managed thread.
  priority: 8 (lowest, background OTA concern)

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
                    command_uart_thread_entry, NULL, NULL, NULL,
                    UART_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&rest_api_thread_data, rest_api_stack, REST_API_STACK_SIZE,
                    rest_api_thread_entry, NULL, NULL, NULL,
                    REST_API_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&ota_thread_data, ota_stack, OTA_STACK_SIZE,
                    ota_thread_entry, NULL, NULL, NULL,
                    OTA_PRIORITY, 0, K_NO_WAIT);

    LOG_INF("All threads created. Entering main loop.");

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
