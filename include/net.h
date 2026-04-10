#ifndef NET_H
#define NET_H

#include <zephyr/kernel.h>

/* ---------- Thread configuration ---------- */
#define NET_STACK_SIZE 2048
#define NET_PRIORITY   5

/* ---------- Thread resources (defined in net.c) ---------- */
extern struct k_thread net_thread_data;
extern k_thread_stack_t net_stack[];

/* ---------- Module API ---------- */
void net_thread_entry(void *p1, void *p2, void *p3);

#endif /* NET_H */
