#ifndef NET_H
#define NET_H

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>

/* ---------- Thread configuration ---------- */
#define NET_STACK_SIZE 2048
#define NET_PRIORITY   5

/* ---------- IPv4 configuration ---------- */
struct net_ipv4_config {
    struct in_addr addr;
    struct in_addr netmask;
    struct in_addr gw;
};

/* ---------- Thread resources (defined in net.c) ---------- */
extern struct k_thread net_thread_data;
extern k_thread_stack_t net_stack[];

/* ---------- Module API ---------- */
void net_thread_entry(void *p1, void *p2, void *p3);

/**
 * Set the IPv4 address, netmask, and gateway on the default network interface.
 * Returns 0 on success, negative errno on failure.
 */
int net_set_ip(const struct net_ipv4_config *cfg);

#endif /* NET_H */
