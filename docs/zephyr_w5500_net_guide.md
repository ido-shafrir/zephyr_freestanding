# Zephyr W5500 Networking Guide

A step-by-step guide to bringing up the WIZnet W5500 SPI Ethernet controller on Zephyr with DHCP and static IP support.

## Architecture Overview

```
┌──────────────┐   SPI   ┌──────────────┐   net_mgmt    ┌───────────────┐
│  W5500 HW    │ ◄─────► │  Zephyr      │ ─────────────►│  Net Thread   │
│  (Ethernet)  │         │  eth_w5500   │               │               │
│              │         │  driver      │               │  init_net()   │
│              │         │              │   DHCP_BOUND  │  dhcp_handler │
└──────────────┘         └──────────────┘               └───────────────┘
```

The W5500 communicates over SPI. Zephyr's `eth_w5500` driver handles all register-level access. Your application code interacts only with Zephyr's networking APIs (`net_if`, `net_mgmt`, `dhcpv4`).

---

## Step 1: Kconfig — Enable Networking and the W5500 Driver

In `prj.conf`:

```ini
# ─── Networking Core ───
CONFIG_NETWORKING=y
CONFIG_NET_L2_ETHERNET=y

# ─── TCP/IP Stack ───
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_ARP=y

# ─── Sockets (optional, for application use) ───
CONFIG_NET_SOCKETS=y

# ─── DHCP ───
CONFIG_NET_DHCPV4=y

# ─── Net Management Events (for DHCP lease callback) ───
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_MGMT_EVENT_INFO=y

# ─── SPI ───
CONFIG_SPI=y

# ─── W5500 Driver ───
CONFIG_ETH_W5500=y

# ─── GPIO (needed for CS and INT pins) ───
CONFIG_GPIO=y

# ─── Buffer Configuration ───
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16
CONFIG_NET_BUF_RX_COUNT=32
CONFIG_NET_BUF_TX_COUNT=32
CONFIG_NET_TX_STACK_SIZE=2048
CONFIG_NET_RX_STACK_SIZE=2048

# ─── Logging ───
CONFIG_LOG=y
CONFIG_NET_LOG=y
```

Key points:
- `CONFIG_ETH_W5500=y` pulls in the W5500 driver. Zephyr matches it to the devicetree node via `compatible = "wiznet,w5500"`.
- `CONFIG_NET_MGMT_EVENT=y` and `CONFIG_NET_MGMT_EVENT_INFO=y` are required for the DHCP lease callback to work.

---

## Step 2: Devicetree Overlay — Wire the W5500 to SPI

Create a board overlay file (e.g. `boards/nucleo_h753zi.overlay`):

```dts
/* Disable onboard Ethernet MAC if present — avoids two interfaces
 * on the same subnet causing routing confusion. */
&mac {
    status = "disabled";
};

/* Configure SPI pins (example: STM32H7 SPI1 on AF5) */
&pinctrl {
    spi1_sck_pa5: spi1_sck_pa5 {
        pinmux = <STM32_PINMUX('A', 5, AF5)>;
        bias-pull-down;
        slew-rate = "very-high-speed";
    };
    spi1_miso_pa6: spi1_miso_pa6 {
        pinmux = <STM32_PINMUX('A', 6, AF5)>;
    };
    spi1_mosi_pb5: spi1_mosi_pb5 {
        pinmux = <STM32_PINMUX('B', 5, AF5)>;
        slew-rate = "very-high-speed";
    };
};

/* Enable SPI1 and attach the W5500 */
&spi1 {
    status = "okay";
    pinctrl-0 = <&spi1_sck_pa5 &spi1_miso_pa6 &spi1_mosi_pb5>;
    pinctrl-names = "default";
    cs-gpios = <&gpiod 14 GPIO_ACTIVE_LOW>;

    w5500: w5500@0 {
        compatible = "wiznet,w5500";
        reg = <0>;
        spi-max-frequency = <10000000>;  /* 10 MHz — safe for jumper wires */
        int-gpios = <&gpiod 15 GPIO_ACTIVE_LOW>;
        local-mac-address = [02 AB CD EF 01 23];
    };
};
```

### IMPORTANT: Always set `local-mac-address`

> **Bug lesson (see [bug_reports/001](../bug_reports/001_w5500_dhcp_failure.md)):**
> Without `local-mac-address`, Zephyr generates a **random MAC at every boot**.
> Many DHCP servers/routers ignore or rate-limit requests from frequently changing
> MACs, or the random MAC may have the multicast bit set, causing DHCP to silently
> fail (DISCOVER sent, no OFFER received).
>
> **Fix:** Always set a fixed locally-administered MAC in the overlay. The `02`
> prefix sets the "locally administered" bit — this is correct for manually
> assigned MACs that are not IEEE-registered OUIs.

---

## Step 3: DHCP Event Callback

Register a net management callback to be notified when a DHCP lease is acquired:

```c
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>

static struct net_mgmt_event_callback dhcp_cb;

/**
 * @brief Net management callback for DHCP bound events.
 *
 * Called by Zephyr's net_mgmt when a DHCP lease is acquired.
 * Logs the assigned IPv4 address, netmask, and gateway.
 *
 * @param cb         Pointer to the event callback structure.
 * @param mgmt_event The management event that triggered the callback.
 * @param iface      The network interface that received the DHCP lease.
 */
static void dhcp_handler(struct net_mgmt_event_callback *cb,
                         uint64_t mgmt_event,
                         struct net_if *iface)
{
    if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
        return;
    }

    char addr_str[NET_IPV4_ADDR_LEN];
    char mask_str[NET_IPV4_ADDR_LEN];
    char gw_str[NET_IPV4_ADDR_LEN];

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

    LOG_INF("DHCP lease acquired — IP: %s  Mask: %s  GW: %s",
            addr_str, mask_str, gw_str);
}
```

- `dhcp_cb` must be static/global — it is linked into net_mgmt's internal list.
- Register it **before** starting DHCP to avoid missing the first event.

---

## Step 4: Network Initialization

```c
#include <zephyr/net/net_if.h>

/**
 * @brief Initialize the W5500 network interface.
 *
 * Registers the DHCP event callback, waits for the W5500 to be ready,
 * checks link status, and always starts the DHCP client regardless
 * of link state.
 *
 * @return 0 on success, -1 if no interface found.
 */
static int init_net(void)
{
    /* Register DHCP callback before anything else */
    net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&dhcp_cb);

    /* Give the W5500 driver time to initialize */
    k_msleep(2000);

    struct net_if *iface = net_if_get_default();
    if (iface == NULL) {
        LOG_ERR("No network interface found!");
        return -1;
    }

    /* Log MAC address */
    struct net_linkaddr *ll = net_if_get_link_addr(iface);
    if (ll && ll->len == 6) {
        LOG_INF("MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                ll->addr[0], ll->addr[1], ll->addr[2],
                ll->addr[3], ll->addr[4], ll->addr[5]);
    }

    /* Wait for link if not already up */
    if (!net_if_is_up(iface)) {
        LOG_WRN("Interface is DOWN — waiting for link...");
        for (int i = 0; i < 20; i++) {
            k_msleep(500);
            if (net_if_is_up(iface)) {
                LOG_INF("Interface came UP after %d ms", (i + 1) * 500);
                break;
            }
        }
    }

    /* Print existing IP config if any */
    if (iface->config.ip.ipv4 != NULL) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            struct net_if_addr *addr = &iface->config.ip.ipv4->unicast[i].ipv4;
            if (addr->is_used) {
                char s[NET_IPV4_ADDR_LEN];
                net_addr_ntop(AF_INET, &addr->address.in_addr, s, sizeof(s));
                LOG_INF("IPv4 Address: %s", s);
            }
        }
    } else {
        LOG_WRN("No IPv4 configuration assigned yet");
    }

    /*
     * *** ALWAYS start DHCP regardless of link state. ***
     *
     * Zephyr's DHCP client handles IF_UP/IF_DOWN events internally.
     * If the cable is unplugged at boot, DHCP will automatically send
     * DISCOVER once the link comes up later.
     *
     * BUG LESSON (see bug_reports/001): Previously DHCP was only started
     * when the interface already had an IPv4 config. If the cable was
     * unplugged at boot, the code returned before reaching
     * net_dhcpv4_start(), leaving the device permanently unconfigured
     * even after the cable was plugged in.
     */
    net_dhcpv4_start(iface);
    LOG_INF("DHCP client started");

    return 0;
}
```

---

## Step 5: Thread Entry and Creation

```c
#define NET_STACK_SIZE 2048
#define NET_PRIORITY   5

K_THREAD_STACK_DEFINE(net_stack, NET_STACK_SIZE);
struct k_thread net_thread_data;

/**
 * @brief Net module thread entry point.
 *
 * Calls init_net() to bring up the W5500, then exits.
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
    LOG_INF("Net thread completed initialization.");
}
```

Create the thread from `main()`:

```c
#include "w5500_net.h"

void main(void)
{
    k_thread_create(&net_thread_data, net_stack,
                    K_THREAD_STACK_SIZEOF(net_stack),
                    net_thread_entry, NULL, NULL, NULL,
                    NET_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&net_thread_data, "w5500_net");
}
```

---

## Step 6: Static IP and DHCP Switching

The module provides `net_set_ip()` and `net_set_dhcp()` for runtime IP management:

```c
/* Switch to static IP */
struct net_ipv4_config cfg;
net_addr_pton(AF_INET, "192.168.0.50", &cfg.addr);
net_addr_pton(AF_INET, "255.255.255.0", &cfg.netmask);
net_addr_pton(AF_INET, "192.168.0.1", &cfg.gw);
net_set_ip(&cfg);

/* Switch back to DHCP */
net_set_dhcp();

/* Read current config */
struct net_ipv4_config current;
if (net_get_ip(&current) == 0) {
    /* use current.addr, current.netmask, current.gw */
}
```

`net_set_ip()` stops DHCP, removes old addresses, and applies the new config.  
`net_set_dhcp()` removes static addresses and starts the DHCP client.

---

## File Layout

```
project/
├── include/
│   └── w5500_net.h          # IPv4 config struct, thread externs, API prototypes
├── src/
│   ├── main.c               # Creates the net thread
│   └── w5500_net.c           # DHCP handler, init, set_ip/set_dhcp/get_ip
├── boards/
│   └── <board>.overlay       # W5500 SPI + MAC address devicetree overlay
├── prj.conf                  # Networking, SPI, W5500, DHCP Kconfig
└── CMakeLists.txt
```

---

## Known Issues and Bug Fixes

These issues were discovered in downstream projects and are already addressed in this code:

### 1. DHCP Fails — Random MAC Rejected by Router

**Symptom:** DISCOVER packets sent repeatedly, no OFFER received. Static IP works fine.

**Cause:** No `local-mac-address` in the devicetree overlay. Zephyr generates a random MAC each boot, which some DHCP servers reject.

**Fix:** Always set `local-mac-address` in the W5500 devicetree node. Use the `02` prefix for locally-administered addresses.

See [bug_reports/001_w5500_dhcp_failure.md](../bug_reports/001_w5500_dhcp_failure.md) for full details.

### 2. DHCP Never Starts When Cable Unplugged at Boot

**Symptom:** If Ethernet cable is not connected at boot, DHCP never starts even after plugging in later.

**Cause:** `init_net()` returned early when no IPv4 config existed, skipping `net_dhcpv4_start()`.

**Fix:** Always call `net_dhcpv4_start()` regardless of link/IP state. The DHCP client handles late link-up internally.

See [bug_reports/001_w5500_dhcp_failure.md](../bug_reports/001_w5500_dhcp_failure.md) (Root Cause 2) for full details.

---

## Debugging Checklist

| Issue | What to Check |
|---|---|
| No network interface found | Overlay filename matches board? `CONFIG_ETH_W5500=y`? |
| Interface stays DOWN | SPI wiring (SCK/MOSI/MISO/CS)? W5500 powered at 3.3V? Cable connected? |
| DHCP never gets an OFFER | `local-mac-address` set in overlay? Router DHCP pool not exhausted? |
| DHCP never starts | `net_dhcpv4_start()` called unconditionally? Not guarded by early return? |
| No MAC address logged | SPI communication failing — check wiring and `spi-max-frequency` |
| ARP works but no TCP | `CONFIG_NET_TCP=y` in prj.conf? Buffer counts sufficient? |
| Intermittent SPI errors | Lower `spi-max-frequency` (try 1 MHz on long wires) |

## Optional: Link-State and DHCP-Event Hooks

The template ships `w5500_net.c` with a single `net_mgmt` callback for `NET_EVENT_IPV4_DHCP_BOUND` (logs the lease). Two more hook points are commonly useful in real applications and are easy to add in the same style:

- `NET_EVENT_L4_CONNECTED` / `NET_EVENT_L4_DISCONNECTED` \u2014 link-layer carrier up/down (cable plugged/unplugged or peer link failure).
- `NET_EVENT_IPV4_DHCP_BOUND` \u2014 already wired; useful additional payloads include the DHCP server IP, lease time, and DNS list.

Typical use-cases:

- **Event log entries** \u2014 emit a NETWORK event every time the link goes up/down or DHCP rebinds, so the long-term history shows connectivity issues at a glance.
- **Trigger a re-sync** \u2014 give a semaphore the time-service thread is waiting on, so SNTP runs immediately on every fresh DHCP lease (instead of waiting for the next periodic tick).
- **Gate OTA readiness** \u2014 only call `ota_report_module_ready(OTA_MODULE_NET)` after the link is actually carrying traffic.

### Sketch

```c
static struct net_mgmt_event_callback carrier_cb;

static void carrier_handler(struct net_mgmt_event_callback *cb,
                            uint64_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_L4_CONNECTED) {
        LOG_INF("link up");
        /* event_log_write(EVENT_SEV_INFO, EVENT_TYPE_NETWORK, "link up"); */
        /* time_service_sync(); */
    } else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
        LOG_INF("link down");
        /* event_log_write(EVENT_SEV_WARN, EVENT_TYPE_NETWORK, "link down"); */
    }
}

/* Inside init_net(), before net_dhcpv4_start(): */
net_mgmt_init_event_callback(&carrier_cb, carrier_handler,
                             NET_EVENT_L4_CONNECTED |
                             NET_EVENT_L4_DISCONNECTED);
net_mgmt_add_event_callback(&carrier_cb);
```

The template intentionally ships **without** these hooks active so projects that don't need them stay free of dependencies on `event_log` or `time_service`. Paste the snippet above into `src/w5500_net.c` when you do need them.

### Querying the active address type

When you want to know whether the running address came from DHCP or was set statically (e.g. for a REST status endpoint, or to skip DHCP-renewal logic on a static config), use:

```c
bool dhcp = net_is_dhcp();
```

The helper is implemented in `w5500_net.c` and inspects the address-type metadata Zephyr stores on the active unicast address. No flags or shadow state required.
