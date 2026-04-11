# Bug Report #001: W5500 DHCP Failure — No Lease Acquired

**Date:** 2026-04-11  
**Severity:** High  
**Status:** Resolved  
**Component:** Network (W5500 / DHCPv4)  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

DHCP client sent DISCOVER packets but never received an OFFER from the
router. The same cable and network worked immediately when connected to a
laptop. Static IP configuration on the W5500 worked correctly, confirming
SPI and Ethernet hardware were functional.

---

## Symptoms

1. Boot log showed "Interface is UP" and "starting DHCP..."
2. DHCP debug logs showed repeated `dhcpv4_send_discover` with exponential
   backoff (3 s → 8 s → 15 s → 32 s), but no OFFER was ever received.
3. Eventually the log showed "Link down" (when user physically moved the
   cable to a laptop for testing).
4. Static IP (`ip_set 192.168.0.50 255.255.255.0 192.168.0.1`) worked
   perfectly — pings, ARP, full connectivity.

---

## Root Causes Found (Two Independent Issues)

### Root Cause 1 — Random MAC Address Rejected by DHCP Server

**Problem:** The W5500 devicetree node had no `local-mac-address` property.
Zephyr's `net_eth_mac_load()` generated a random MAC at each boot. Some
DHCP servers / routers ignore or rate-limit requests from frequently-
changing MACs, or the randomly generated MAC happened to have an invalid
OUI / multicast bit set.

**Evidence:** After assigning a fixed locally-administered MAC
(`02:AB:CD:EF:01:23`) in the DTS overlay, DHCP immediately succeeded
on the first DISCOVER → OFFER → REQUEST → ACK cycle.

**Fix (boards/nucleo_h753zi.overlay):**
```dts
w5500: w5500@0 {
    compatible = "wiznet,w5500";
    ...
    local-mac-address = [02 AB CD EF 01 23];
};
```

> **Note:** The `02` prefix sets the "locally administered" bit (bit 1 of
> the first octet), which is correct for a manually assigned MAC that is
> not an IEEE-registered OUI.

### Root Cause 2 — DHCP Never Started When Link Was Down at Boot

**Problem:** `init_net()` had an early `return 0` when
`iface->config.ip.ipv4 == NULL` (no IPv4 config assigned yet). This code
path was hit whenever the Ethernet cable was not connected at boot time.
The function returned *before* reaching `net_dhcpv4_start()`, so DHCP was
never started. Even after the cable was later plugged in and link came up,
no DHCP client was running.

**Evidence:** Boot log showed:
```
[00:00:12.010,000] <wrn> net: No IPv4 configuration assigned yet
[00:00:12.010,000] <inf> net: Net thread completed initialization. Exiting thread.
...
[00:01:38.024,000] <inf> eth_w5500: w5500@0: Link up
[00:01:38.024,000] <inf> eth_w5500: w5500@0: Link speed 100 Mb, full duplex
```
No DHCP activity after link-up.

**Fix (src/net.c):** Removed the early return. DHCP is now always started
regardless of link state. Zephyr's DHCP client internally handles IF_UP /
IF_DOWN events and will send DISCOVER once the link is established.

```c
/* Before (broken): */
if (iface->config.ip.ipv4 == NULL) {
    LOG_WRN("No IPv4 configuration assigned yet");
    return 0;  // ← skipped net_dhcpv4_start()
}

/* After (fixed): */
if (iface->config.ip.ipv4 != NULL) {
    /* print existing addresses */
} else {
    LOG_WRN("No IPv4 configuration assigned yet");
}
/* Always start DHCP — it handles late link-up internally */
net_dhcpv4_start(iface);
```

---

## Additional Issue — Zephyr net_config Auto-Init Race Condition

**Problem:** The original `prj.conf` had:
```
CONFIG_NET_CONFIG_SETTINGS=y
CONFIG_NET_CONFIG_NEED_IPV4=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_CONFIG_MY_IPV4_ADDR=""
```

Zephyr's `net_config` subsystem attempted to auto-start DHCP very early
in boot, before the W5500 SPI driver had fully initialized. This silent
failure left no DHCP client running.

**Fix:** Removed `CONFIG_NET_CONFIG_SETTINGS` and related settings.
DHCP is now started explicitly in `init_net()` after confirming the W5500
interface exists.

---

## Diagnostic Steps Taken

| Step | Action | Result |
|------|--------|--------|
| 1 | Enabled `CONFIG_NET_DHCPV4_LOG_LEVEL_DBG` | Confirmed DISCOVER sent, no OFFER received |
| 2 | Tested static IP | Worked — ruled out SPI/hardware issues |
| 3 | Added MAC address logging | Confirmed MAC was being set |
| 4 | Set fixed `local-mac-address` in DTS | DHCP immediately worked |
| 5 | Tested boot without cable, late plug-in | Found DHCP never started (Root Cause 2) |
| 6 | Moved `net_dhcpv4_start()` to always execute | Late plug-in now gets DHCP lease |

---

## DHCP Success Log (After Fix)

```
[00:00:02.008,000] <inf> net: MAC: 02:ab:cd:ef:01:23
[00:00:02.008,000] <inf> net: Interface is UP
[00:00:02.008,000] <inf> net: DHCP client started
[00:00:04.009,000] <dbg> net_dhcpv4: dhcpv4_send_discover: send discover xid=0xb145324e
[00:00:04.739,000] <dbg> net_dhcpv4: net_dhcpv4_input: yiaddr=192.168.0.84
[00:00:04.739,000] <dbg> net_dhcpv4: dhcpv4_handle_reply: state=requesting msg=ack
[00:00:04.739,000] <inf> net_dhcpv4: Received: 192.168.0.84
[00:00:04.739,000] <inf> net:   DHCP lease acquired
[00:00:04.739,000] <inf> net:   Address: 192.168.0.84
[00:00:04.739,000] <inf> net:   Netmask: 255.0.0.0
[00:00:04.739,000] <inf> net:   Gateway: 192.168.0.1
```

---

## Files Changed

| File | Change |
|------|--------|
| `boards/nucleo_h753zi.overlay` | Added `local-mac-address = [02 AB CD EF 01 23]` |
| `prj.conf` | Removed `CONFIG_NET_CONFIG_SETTINGS`, added `CONFIG_NET_MGMT_EVENT`, `CONFIG_NET_DHCPV4_LOG_LEVEL_DBG` |
| `src/net.c` | Added DHCP event callback, MAC logging, always-start-DHCP logic |

---

## Lessons Learned

1. **Always set a fixed MAC on W5500.** Random MACs change every boot and
   can cause DHCP servers to reject or delay offers.
2. **Never gate DHCP start on link state.** Zephyr's DHCP client handles
   IF_UP/IF_DOWN events internally. Start it unconditionally.
3. **Avoid `CONFIG_NET_CONFIG_SETTINGS` when managing IP manually.** It
   races with driver initialization on SPI-based Ethernet controllers.
4. **Enable `CONFIG_NET_DHCPV4_LOG_LEVEL_DBG` early.** The DHCP state
   machine logs are the fastest way to diagnose lease failures.
