#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>
#include "w5500_net.h"
#include "config_store.h"
#include "time_service.h"

LOG_MODULE_REGISTER(w5500_net, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(net_stack, NET_STACK_SIZE);
struct k_thread net_thread_data;

/* ---------- DHCP event callback ---------- */
static struct net_mgmt_event_callback dhcp_cb;

/**
 * @brief Common post-bind handling for both DHCP and static-IP paths.
 *
 * Logs the bound address/mask/gateway and kicks the time-service sync.
 * Called from dhcp_handler() after a DHCP lease and from init_net()
 * immediately after a successful static-IP apply.
 *
 * @param iface  The network interface that just got an IPv4 address.
 * @param source "DHCP" or "static" — used in log messages.
 */
static void on_ip_bound(struct net_if *iface, const char *source)
{
    char addr_str[NET_IPV4_ADDR_LEN] = "(none)";
    char mask_str[NET_IPV4_ADDR_LEN] = "(none)";
    char gw_str[NET_IPV4_ADDR_LEN]   = "(none)";

    if (iface->config.ip.ipv4 != NULL) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

            if (a->is_used) {
                net_addr_ntop(AF_INET, &a->address.in_addr,
                              addr_str, sizeof(addr_str));
                net_addr_ntop(AF_INET,
                              &iface->config.ip.ipv4->unicast[i].netmask,
                              mask_str, sizeof(mask_str));
                break;
            }
        }
        net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw,
                      gw_str, sizeof(gw_str));
    }

    LOG_INF("========================================");
    LOG_INF("  IPv4 bound (%s)", source);
    LOG_INF("  Address: %s", addr_str);
    LOG_INF("  Netmask: %s", mask_str);
    LOG_INF("  Gateway: %s", gw_str);
    LOG_INF("========================================");

    /* Trigger time sync now that IP is up */
    time_service_sync();
}

/**
 * @brief Net management callback for DHCP bound events.
 *
 * Called by Zephyr's net_mgmt when a DHCP lease is acquired.
 * Delegates to on_ip_bound() for the common post-bind work.
 */
static void dhcp_handler(struct net_mgmt_event_callback *cb,
                         uint64_t mgmt_event,
                         struct net_if *iface)
{
    if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
        return;
    }

    char dhcp_srv_str[NET_IPV4_ADDR_LEN];
    net_addr_ntop(AF_INET, &iface->config.dhcpv4.server_id,
                  dhcp_srv_str, sizeof(dhcp_srv_str));
    LOG_INF("DHCP server: %s", dhcp_srv_str);

    on_ip_bound(iface, "DHCP");
}

/**
 * @brief Initialize the W5500 network interface.
 *
 * Registers the DHCP event callback, waits for the W5500 Ethernet
 * controller to be ready, checks link status, prints the assigned
 * IPv4 configuration, and always starts the DHCP client regardless
 * of link state (the DHCP client handles late link-up internally).
 *
 * @return 0 on success, -1 if no interface found.
 */
static int init_net(void)
{
    LOG_INF("========================================");
    LOG_INF("  W5500 Network Bring-Up");
    LOG_INF("========================================");

    /* Register DHCP event callback before anything else */
    net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&dhcp_cb);

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

    /* ── Log the MAC address ── */
    struct net_linkaddr *ll = net_if_get_link_addr(iface);

    if (ll && ll->len == 6) {
        LOG_INF("MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                ll->addr[0], ll->addr[1], ll->addr[2],
                ll->addr[3], ll->addr[4], ll->addr[5]);
    } else {
        LOG_WRN("No valid MAC address!");
    }

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

    /* ── Print our IP configuration (if any) ── */
    if (iface->config.ip.ipv4 != NULL) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

            if (addr->is_used) {
                char addr_str[NET_IPV4_ADDR_LEN];

                net_addr_ntop(AF_INET, &addr->address.in_addr,
                              addr_str, sizeof(addr_str));
                LOG_INF("IPv4 Address: %s", addr_str);
            }
        }
    } else {
        LOG_WRN("No IPv4 configuration assigned yet");
    }

    /*
     * ── Apply network configuration from config_store ──
     *
     * If a static IP is persisted, apply it directly.
     * Otherwise fall through to DHCP.
     */
    char ip_str[CONFIG_STORE_IP_ADDR_MAX_LEN]   = "";
    char mask_str2[CONFIG_STORE_IP_ADDR_MAX_LEN] = "";
    char gw_str2[CONFIG_STORE_IP_ADDR_MAX_LEN]   = "";

    (void)config_store_get_ip_address(ip_str, sizeof(ip_str));
    (void)config_store_get_ip_mask(mask_str2, sizeof(mask_str2));
    (void)config_store_get_ip_gateway(gw_str2, sizeof(gw_str2));

    if (ip_str[0] != '\0') {
        struct net_ipv4_config cfg = {0};

        if (net_addr_pton(AF_INET, ip_str, &cfg.addr) < 0) {
            LOG_ERR("Stored ipAddress \"%s\" invalid — falling back to DHCP",
                    ip_str);
            goto start_dhcp;
        }
        if (mask_str2[0] == '\0' ||
            net_addr_pton(AF_INET, mask_str2, &cfg.netmask) < 0) {
            LOG_WRN("Stored ipMask invalid/empty — defaulting to 255.255.255.0");
            (void)net_addr_pton(AF_INET, "255.255.255.0", &cfg.netmask);
        }
        if (gw_str2[0] != '\0') {
            if (net_addr_pton(AF_INET, gw_str2, &cfg.gw) < 0) {
                LOG_WRN("Stored ipDefaultGateway invalid — using 0.0.0.0");
            }
        }

        LOG_INF("Applying static IPv4: %s/%s gw=%s",
                ip_str, mask_str2, gw_str2[0] ? gw_str2 : "(none)");

        int rc = net_set_ip(&cfg);
        if (rc < 0) {
            LOG_ERR("net_set_ip() failed: %d — falling back to DHCP", rc);
            goto start_dhcp;
        }
        on_ip_bound(iface, "static");
        return 0;
    }

start_dhcp:
    /*
     * Start DHCP regardless of current link state.
     * Zephyr's DHCP client handles IF_UP/IF_DOWN events internally
     * and will send DISCOVER once the link is established.
     */
    LOG_INF("Starting DHCP client...");
    net_dhcpv4_start(iface);
    LOG_INF("DHCP client started");

    return 0;
}

/**
 * @brief Net module thread entry point.
 *
 * Calls init_net() to bring up the W5500, then exits.
 * Created by main() via k_thread_create().
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void net_thread_entry(void *p1, void *p2, void *p3)
{
    if (init_net() < 0) {
        LOG_ERR("Network initialization failed.");
        return;
    }
    LOG_INF("Net thread completed initialization. Exiting thread.");
}

/**
 * @brief Set the IPv4 address, netmask, and gateway on the default interface.
 *
 * Stops DHCP if running, removes any existing IPv4 addresses,
 * then applies the new static configuration.
 *
 * @param cfg Pointer to the IPv4 configuration (address, netmask, gateway).
 * @return 0 on success, -ENODEV if no interface, -ENOMEM if address add failed.
 */
int net_set_ip(const struct net_ipv4_config *cfg)
{
    struct net_if *iface = net_if_get_default();

    if (iface == NULL) {
        LOG_ERR("No network interface found");
        return -ENODEV;
    }

    if (!net_if_is_up(iface)) {
        LOG_ERR("Network interface is not up");
        return -ENETDOWN;
    }

    /* Stop DHCP if it is running */
    net_dhcpv4_stop(iface);

    /* Collect old addresses before making any changes */
    struct in_addr old_addrs[NET_IF_MAX_IPV4_ADDR];
    int old_count = 0;

    if (iface->config.ip.ipv4 != NULL) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

            if (addr->is_used) {
                old_addrs[old_count++] = addr->address.in_addr;
            }
        }
    }

    /* Remove old addresses first to free a slot */
    for (int i = 0; i < old_count; i++) {
        net_if_ipv4_addr_rm(iface, &old_addrs[i]);
    }

    /* Add new address — triggers a gratuitous ARP if interface is UP */
    struct net_if_addr *ifaddr = net_if_ipv4_addr_add(
        iface, &cfg->addr, NET_ADDR_MANUAL, 0);

    if (ifaddr == NULL) {
        LOG_ERR("Failed to add IPv4 address");
        return -ENOMEM;
    }

    /* Set netmask for the newly added address */
    if (!net_if_ipv4_set_netmask_by_addr(iface, &cfg->addr, &cfg->netmask)) {
        LOG_WRN("Failed to set netmask");
    }

    /* Set gateway */
    net_if_ipv4_set_gw(iface, &cfg->gw);

    char addr_str[NET_IPV4_ADDR_LEN];
    char mask_str[NET_IPV4_ADDR_LEN];
    char gw_str[NET_IPV4_ADDR_LEN];

    net_addr_ntop(AF_INET, &cfg->addr, addr_str, sizeof(addr_str));
    net_addr_ntop(AF_INET, &cfg->netmask, mask_str, sizeof(mask_str));
    net_addr_ntop(AF_INET, &cfg->gw, gw_str, sizeof(gw_str));

    LOG_INF("IP set: %s mask %s gw %s", addr_str, mask_str, gw_str);

    return 0;
}

/**
 * @brief Start DHCPv4 on the default network interface.
 *
 * Removes any existing static IPv4 addresses and starts the DHCP client.
 *
 * @return 0 on success, negative errno on failure.
 */
int net_set_dhcp(void)
{
    struct net_if *iface = net_if_get_default();

    if (iface == NULL) {
        LOG_ERR("No network interface found");
        return -ENODEV;
    }

    if (!net_if_is_up(iface)) {
        LOG_ERR("Network interface is not up");
        return -ENETDOWN;
    }

    /* Remove existing static addresses */
    if (iface->config.ip.ipv4 != NULL) {
        struct in_addr old_addrs[NET_IF_MAX_IPV4_ADDR];
        int old_count = 0;

        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

            if (addr->is_used && addr->addr_type == NET_ADDR_MANUAL) {
                old_addrs[old_count++] = addr->address.in_addr;
            }
        }

        for (int i = 0; i < old_count; i++) {
            net_if_ipv4_addr_rm(iface, &old_addrs[i]);
        }
    }

    net_dhcpv4_start(iface);
    LOG_INF("DHCP client started");

    return 0;
}

/**
 * @brief Get the current IPv4 configuration from the default interface.
 *
 * Reads the first active unicast address, its per-address netmask, and the
 * interface gateway. Works for both static and DHCP-assigned addresses.
 *
 * @param cfg Output structure filled with current address, netmask, and gateway.
 * @return 0 on success, -ENODEV if no interface, -ENOENT if no IPv4 address assigned.
 */
int net_get_ip(struct net_ipv4_config *cfg)
{
    struct net_if *iface = net_if_get_default();

    if (iface == NULL) {
        return -ENODEV;
    }

    if (iface->config.ip.ipv4 == NULL) {
        return -ENOENT;
    }

    /* Find the first active unicast address */
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

        if (addr->is_used) {
            cfg->addr = addr->address.in_addr;
            cfg->netmask = iface->config.ip.ipv4->unicast[i].netmask;
            cfg->gw = iface->config.ip.ipv4->gw;
            return 0;
        }
    }

    return -ENOENT;
}

bool net_is_dhcp(void)
{
    struct net_if *iface = net_if_get_default();

    if (iface == NULL || iface->config.ip.ipv4 == NULL) {
        return false;
    }

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;

        if (addr->is_used) {
            return addr->addr_type != NET_ADDR_MANUAL;
        }
    }

    return false;
}
