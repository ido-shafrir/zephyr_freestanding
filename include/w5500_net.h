#ifndef W5500_NET_H
#define W5500_NET_H

/**
 * @file w5500_net.h
 * @brief W5500 network module interface.
 *
 * Provides network initialization, static IP configuration,
 * and DHCPv4 management for the W5500 Ethernet controller.
 */

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

/**
 * @brief Net module thread entry point.
 *
 * Initializes the W5500 interface, registers DHCP callback,
 * and starts the DHCP client. Created by main() via k_thread_create().
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void net_thread_entry(void *p1, void *p2, void *p3);

/**
 * Set the IPv4 address, netmask, and gateway on the default network interface.
 * Stops DHCP if running before applying static config.
 * Returns 0 on success, negative errno on failure.
 */
int net_set_ip(const struct net_ipv4_config *cfg);

/**
 * Start DHCPv4 on the default network interface.
 * Removes any existing static IPv4 addresses first.
 * Returns 0 on success, negative errno on failure.
 */
int net_set_dhcp(void);

/**
 * Get the current IPv4 configuration (address, netmask, gateway).
 * Works for both static and DHCP-assigned addresses.
 * Returns 0 on success, -ENODEV if no interface, -ENOENT if no address.
 */
int net_get_ip(struct net_ipv4_config *cfg);

#endif /* W5500_NET_H */
