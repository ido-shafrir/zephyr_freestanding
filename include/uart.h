#ifndef UART_H
#define UART_H

#include <zephyr/kernel.h>

/* ---------- Thread configuration ---------- */
#define UART_STACK_SIZE 1024
#define UART_PRIORITY   4

/* ---------- Thread resources (defined in uart.c) ---------- */
extern struct k_thread uart_thread_data;
extern k_thread_stack_t uart_stack[];

/* ---------- Module API ---------- */
void uart_thread_entry(void *p1, void *p2, void *p3);

#endif /* UART_H */
