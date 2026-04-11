#ifndef COMMAND_UART_H
#define COMMAND_UART_H

/**
 * @file command_uart.h
 * @brief UART command interface module.
 *
 * Provides an interrupt-driven UART RX command interface
 * on the console UART (USART3 via ST-Link). Supports commands
 * for IP configuration, DHCP control, and diagnostics.
 */

#include <zephyr/kernel.h>

/* ---------- Thread configuration ---------- */
#define UART_STACK_SIZE 1024
#define UART_PRIORITY   4

/* ---------- Thread resources (defined in uart.c) ---------- */
extern struct k_thread uart_thread_data;
extern k_thread_stack_t uart_stack[];

/* ---------- Module API ---------- */

/**
 * @brief UART module thread entry point.
 *
 * Initializes interrupt-driven UART RX, then loops waiting
 * for complete command lines and dispatching them.
 * Created by main() via k_thread_create().
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void command_uart_thread_entry(void *p1, void *p2, void *p3);

#endif /* COMMAND_UART_H */
