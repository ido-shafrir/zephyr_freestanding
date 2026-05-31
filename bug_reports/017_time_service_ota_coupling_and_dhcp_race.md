# Bug Report #017: SNTP First Sync Races DHCP Lease — UDP Dropped "src addr is unspecified"

**Date:** 2026-04-24
**Severity:** Minor
**Status:** Resolved (semantic fix; retry path was already correct)
**Component:** `src/time_service.c` (thread entry), `src/w5500_net.c` (DHCP/link-up handlers), OTA health-check coupling
**Board:** nucleo_h753zi (STM32H753ZI)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** (redacted)


## Summary

On a cold boot, the time service thread attempted its first SNTP sync
before the W5500 driver had received a DHCP lease. With no source IP
assigned, Zephyr's network stack dropped the outgoing UDP packet
("src addr is unspecified") and `sntp_simple()` returned `-1`. The
time_service thread then blocked OTA health-check because ready-
reporting was gated on the *first successful sync*, which couldn't
complete until the next interval — by which point OTA had already
timed out and flagged the image for revert.

---

## Symptoms

```
[00:00:06.620,000] <wrn> net_ctx: DROP: src addr is unspecified
[00:00:06.620,000] <err> net_sntp: Failed to send over UDP socket -1
[00:00:06.620,000] <wrn> time_service: SNTP sync failed: -1 (server=216.239.35.0)
[00:00:08.690,000] <inf> net_dhcpv4: Received: 10.100.110.91    <-- DHCP lease arrives 2 s later
...
[00:00:31.626,000] <err> ota:   HEALTH CHECK TIMEOUT — image NOT confirmed!
[00:00:31.626,000] <err> ota:   Ready modules: 6/7
[00:00:31.626,000] <err> ota:   MCUboot will REVERT to previous image on next reset.
```

The NTP server itself (`216.239.35.0` — Google Public NTP,
`time.google.com`) is correct. The race is purely local: the sync
thread woke earlier than the DHCPv4 client could finish its DORA
handshake.

---

## Root Cause

Two compounding issues:

### (a) Hard-coded 5-second sleep before first sync

`time_service_thread_entry()` did:

```c
k_sleep(K_SECONDS(5));
time_service_try_sync_now();   // <-- at t=~5 s
```

This is a best-effort delay, not a readiness signal. On this board,
DHCP typically resolves at `t≈8–9 s` on a cold boot (link autoneg +
DORA round-trip over a W5500). The first sync therefore always fired
~3 s before there was a usable IP. The retry loop would pick up on
the next interval — but by then OTA health had already timed out.

### (b) OTA readiness gated on first successful sync

`ota_report_module_ready(OTA_MODULE_TIME_SERVICE)` was called only
*inside* the `rc == 0` branch of `time_service_try_sync_now()`. This
meant a transient network condition — DHCP delay, firewalled NTP, or
an empty NTP config that returned 0 quickly — determined whether a
freshly-booted image was considered healthy enough to confirm.

OTA confirmation is a *local* image-integrity signal. An external
time-sync outage must never cause MCUboot to revert a working image.
Conflating "clock is synced" with "OTA is healthy" is a layering
violation.

---

## Impact

- Every cold boot where DHCP completed after the 5 s sleep (the
  common case on this board) risked an OTA revert despite the firmware
  being functional.
- Log noise suggested a real NTP failure to operators, who then
  wasted time investigating the NTP server.
- The failure mode was timing-dependent and non-deterministic —
  sometimes DHCP finished at t=4 s and the sync succeeded, sometimes
  at t=9 s and it didn't. Flaky reverts with no obvious trigger are
  the worst kind of OTA bug.

---

## Fix

Two targeted changes in `time_service_thread_entry()`:

1. **Report OTA-ready immediately after `time_service_init()` success**,
   before the network-wait sleep. The wall clock is already seeded
   with the fallback epoch so other modules see a plausible time.
2. **First and periodic sync attempts are now best-effort** — no
   longer re-report OTA readiness, just try to sync. The periodic
   loop in the thread will keep retrying until it succeeds.

```c
int rc = time_service_init();
if (rc == 0) {
    ota_report_module_ready(OTA_MODULE_TIME_SERVICE);  // <-- now unconditional on sync
}

k_sleep(K_SECONDS(5));                 /* still a best-effort delay */
(void)time_service_try_sync_now();     /* first attempt — may fail, retry loop handles it */

while (1) {
    uint32_t interval = config_store_get_ntp_sync_interval();
    (void)k_sem_take(&sync_sem, K_SECONDS(interval));
    (void)time_service_try_sync_now(); /* periodic retry */
}
```

3. **Event-driven first sync.** `src/w5500_net.c` already owns the
   `net_mgmt` callbacks for `NET_EVENT_IPV4_DHCP_BOUND` and
   `NET_EVENT_IF_UP`. Both handlers now call `time_service_sync()`
   to wake the time_service thread the instant a source IP becomes
   available. `time_service_sync()` just gives the `sync_sem` (max
   count 1), so it's safe to call from the net_mgmt context and
   idempotent if the sync thread is already busy. This converts the
   "retry on interval" fallback into an event-driven first attempt
   — typical cold-boot SNTP sync latency drops from `ntp_interval` +
   DHCP delay to ~0.

- `src/w5500_net.c` — `dhcp_handler()` and `carrier_handler()` now
  call `time_service_sync()` on lease acquisition and link-up so
  the first SNTP sync fires as soon as the interface is usable,
  not on the 5 s fixed sleep.

## Test Coverage

- `tests/test_time_service/src/main.c::test_sync_trigger_does_not_crash`
  already covers `time_service_sync()` end-to-end. The w5500
  callers exercise the same public API — no new logic to test.
- The w5500_net layer has no unit tests (`net_mgmt` callbacks are
  not practical to drive in a host-qemu harness without mocking the
  entire networking stack). Left as-is.
---
*inside*
   `time_service.c`.** Rejected — pulls `net_mgmt` into a module
   that otherwise has no networking dependency. Instead, the net
   layer (`w5500_net.c`, which already subscribes to these events
   for logging) pokes time_service through its existing public APIe.c` — `time_service_thread_entry()` reorders
  OTA-ready reporting and drops the `rc == 0` gate around it.

---

## Alternatives Considered

1. **Increase the pre-sync sleep to 10 s.** Rejected — fragile, still
   racy on slower networks, and delays the first sync for no good
   reason on fast networks.
2. **Block on a `net_mgmt` IPv4_ADDR_ADD event before syncing.**
   Deferred — cleaner but pulls `net_mgmt` into `time_service.c` for
   marginal benefit, and requires handling IP-change events during
   runtime as well.
3. **Move OTA-ready reporting into `time_service_init()`.** Rejected
   — init runs before the thread even starts; keeping the report at
   the thread entry preserves symmetry with other modules and lets
   init stay pure (no side-effects outside the module).

---

## Lessons Learned

- "Ready for OTA health" must mean "the module's local state is
  healthy", not "all external dependencies are reachable". Anything
  that depends on the network, DNS, NTP, or another server is
  external by definition.
- `k_sleep(K_SECONDS(5))` is not a synchronisation primitive; it's a
  guess. Event-driven readiness (e.g., `net_mgmt` notifications)
  should be used when reliability matters.
- Correlate OTA-health logs with network-stack logs when diagnosing
  revert-on-boot issues — the root cause is often a module that's
  waiting for something external.
- The `"src addr is unspecified"` warning from `net_ctx` is almost
  always a pre-DHCP race, not a real socket misconfiguration.

---

## Related

- Bug #013 (time_service test hang on networking stack init) — same
  module, different lifecycle phase.
- Bug #018 (OTA `module_names[]` missing entry, misleading "error"
  print) — discovered in the same debugging session.
