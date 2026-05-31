# Bug Report #027: Event Log Unfilterable When Wall Clock Never Syncs (No NTP Deployment)

**Date:** 2026-04-30
**Severity:** Medium (observability / storage management)
**Status:** Resolved (mitigation: `log_drop` UART command, debug-only)
**Component:** `src/event_log.c` (FCB walker filter),
               `src/command_uart.c` (`event_log [seconds]`, `log_drop` command)
**Board:** All
**Zephyr Version:** 4.4.0+

---

## Summary

When deploying with static IP and no NTP server configured, devices
never reach a synchronized wall clock. The event log's time-window
filter — designed around the assumption that a wall clock eventually
arrives — degrades to a boot-strict, uptime-only filter forever.
Operators see only the current boot's entries and have no built-in way
to free space in the FCB ring.

---

## Symptoms

1. **`event_log <seconds>` returns only current-boot entries** even
   after many reboots. Prior boots are present in the FCB ring but
   suppressed because their `boot_id != current_boot_id` and there are
   no synced entries to compute `min_in_window`.

2. **No way to clear the log.** When the FCB ring fills with stale
   entries, the operator has no UART/REST command to wipe it short of
   `config_factory_reset` (which also wipes IP and other config).

---

## Root Cause

The cross-boot time filter (bug #019) uses `min_in_window` computed
from the smallest `boot_id` whose synced entries fell inside the wall
window. With NTP permanently disabled:

- No entry ever has `wall_clock != 0` → `min_in_window` never advances.
- Every prior-boot entry is pre-sync → gated by strict
  `boot_id == current_boot_id`.
- Result: only the current boot is visible via the default command.

---

## Fix

### Part 1 — Uptime fallback in event_log dump

When `time_service_is_synced()` is false, fall back to a
`(since_uptime, strict_boot_id)` window so the last N seconds of the
current boot are returned:

```c
if (seconds != 0) {
    if (time_service_is_synced()) {
        uint32_t now_wall = (uint32_t)time_service_get();
        since_wall = (seconds > now_wall) ? 0 : now_wall - seconds;
    } else {
        uint32_t now_uptime = (uint32_t)(k_uptime_get() / 1000);
        since_uptime = (seconds > now_uptime) ? 0 : now_uptime - seconds;
        strict = true;
    }
}
```

### Part 2 — `log_drop` UART command (debug-verbosity guard)

A new UART command erases all events, gated by debug verbosity to
prevent accidental wipe in production:

```
> log_drop
error: log_drop requires debug verbosity (run 'log_level_set debug' first)
> log_level_set debug
ok: log level set to debug
> log_drop
ok: event log erased
```

### Part 3 — `event_log_drop_all()` API alias

```c
static inline int event_log_drop_all(void) { return event_log_clear(); }
```

Clearer intent name for the new UART entry-point.

---

## Lessons Learned

- Time-based filtering assumes a time source exists. Any system that
  may deploy without NTP needs an uptime-based fallback path.
- Destructive operations (`log_drop`) should be gated by a privilege
  check — reusing the existing verbosity knob avoids adding new
  access-control machinery.
- `config_factory_reset()` is too coarse for log-only cleanup because
  it also wipes networking config.

---

## Future Work

- **Count-based dump** (`event_log_recent <N>`) — walks back from FCB
  tail ignoring time entirely.
- **Synthetic monotonic key** — derive a `(boot_id, uptime)` total
  order to allow cross-boot visibility without wall-clock.

---

## References

- Bug #019 — event_log cross-boot time filter (the design being
  affected).
- Bug #026 — config_store defaults (related: NTP default change
  surprise).
