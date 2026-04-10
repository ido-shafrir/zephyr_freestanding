#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/logging/log.h>
#include "net.h"

LOG_MODULE_REGISTER(net, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(net_stack, NET_STACK_SIZE);
struct k_thread net_thread_data;

/*
 * Initialize the network interface and bring it up.
 * This function waits for the W5500 Ethernet controller
 * to be ready and prints the IP configuration.
 */

static int init_net(void)
{
 LOG_INF("========================================");
    LOG_INF("  W5500 Network Bring-Up Test");
    LOG_INF("========================================");

    /* 
     * Give the W5500 a moment to initialize.
     * The driver runs in the background — by the time
     * main() executes, it may already be ready, but
     * let's be safe.
     */
    k_msleep(2000);

    /* ── Get the network interface ── */
    struct net_if *iface = net_if_get_default();

    if (iface == NULL) {
        LOG_ERR("No network interface found!");
        LOG_ERR("Check: devicetree overlay loaded? W5500 Kconfig enabled?");
        return -1;
    }

    LOG_INF("Network interface found: %p", iface);

    /* ── Check if the interface is up ── */
    if (net_if_is_up(iface)) {
        LOG_INF("Interface is UP");
    } else {
        LOG_WRN("Interface is DOWN — check SPI wiring and W5500 power");
        LOG_WRN("Waiting for interface...");

        /* Wait up to 10 seconds for the link */
        for (int i = 0; i < 20; i++) {
            k_msleep(500);
            if (net_if_is_up(iface)) {
                LOG_INF("Interface came UP after %d ms", (i + 1) * 500);
                break;
            }
        }

        if (!net_if_is_up(iface)) {
            LOG_WRN("Interface never came up. Debug checklist:");
            LOG_WRN("  1. W5500 powered? (3.3V on the lite board)");
            LOG_WRN("  2. SPI wires correct? (SCK/MOSI/MISO/CS)");
            LOG_WRN("  3. Ethernet cable plugged into W5500 board?");
            LOG_WRN("  4. Check Zephyr boot log for SPI/W5500 errors");
        }
    }

    /* ── Print our IP configuration ── */

    if (iface->config.ip.ipv4 == NULL) {
        LOG_WRN("No IPv4 configuration assigned yet");
        return 0;
    }

    /* Walk the unicast address list to find our IPv4 address */
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

        if (addr->is_used) {
            char addr_str[NET_IPV4_ADDR_LEN];

            net_addr_ntop(AF_INET, &addr->address.in_addr,
                          addr_str, sizeof(addr_str));
            LOG_INF("IPv4 Address: %s", addr_str);
        }
    }

    LOG_INF("========================================");
    LOG_INF("  Network is UP!                       ");
    LOG_INF("  Try: ping %s from your PC",
            CONFIG_NET_CONFIG_MY_IPV4_ADDR);
    LOG_INF("========================================");

    return 0;
}

/* ---------- Thread entry point ---------- */
void net_thread_entry(void *p1, void *p2, void *p3)
{
    if (init_net() < 0) {
        LOG_ERR("Network initialization failed.");
        return;
    }
    LOG_INF("Net thread completed initialization. Exiting thread.");
}

