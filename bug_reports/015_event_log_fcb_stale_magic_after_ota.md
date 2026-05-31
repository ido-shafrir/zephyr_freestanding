# Bug Report #015: Event Log Offline After OTA — FCB Magic Mismatch On Data Partition

**Date:** 2026-04-24
**Severity:** Major
**Status:** Resolved
**Component:** `src/event_log.c` (FCB init path)
**Board:** nucleo_h753zi (STM32H753ZI)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** (redacted)


---

## Summary

After performing an OTA update to a firmware image that had bumped the
event-log FCB magic (from `0x45564C47` / "EVLG" to `0x45564C48` / "EVLH"
when a `wall_clock` field was added to `event_entry_t`), `event_log_init()`
failed with `fcb_init failed: -35` (`-ENOMSG`) and the entire event log
subsystem stayed offline for the rest of the boot.

---

## Symptoms

Boot log on the target after flashing the new firmware via `smpmgr`:

```
[00:00:00.009,000] <err> event_log: fcb_init failed: -35
...
<normal boot continues, but every event_log_write() early-returns>
<no boot event recorded, no runtime events recorded>
```

Subsequent reboots produced the same error every time — it was not a
transient failure.

---

## Root Cause

MCUboot's OTA swap only touches the two application slots (`slot0` and
`slot1`). All other flash partitions — including `event-log` and
`storage` — are preserved across upgrades by design.

The previous firmware wrote FCB headers with magic `0x45564C47` to the
`event-log` partition. The new firmware constructs its FCB with magic
`0x45564C48`. On first boot after the OTA:

1. `fcb_init()` reads the first sector.
2. The sector is **not** blank (previous FW's data is still there).
3. The magic in sector 0 does not equal `event_fcb.f_magic`.
4. FCB returns `-ENOMSG` (`-35`) to signal "no valid FCB here".

Our `event_log_init()` treated that as a fatal error:

```c
rc = fcb_init(PARTITION_ID(event_log_partition), &event_fcb);
if (rc) {
    LOG_ERR("fcb_init failed: %d", rc);
    return rc;                        // <-- no recovery
}
```

This is a data-migration hazard inherent to any on-flash format change.
It will happen any time the FCB magic, version, sector count, or layout
is altered between firmware versions and the upgrade path is OTA (as
opposed to a full erase + reflash via debugger).

---

## Impact
- OTA update failure, image is not confirmed 
- Event log completely unavailable after the OTA upgrade until the
  partition is manually erased.
- No boot event, no runtime events, no severity filter changes
  persisted — silent loss of observability on exactly the boot where
  observability matters most (post-upgrade).
- The `/api/mcu/events` endpoint returned empty bodies or errors.
- The failure was silent from the user's perspective — a single `ERR`
  log line buried in the normal boot output.

---

## Fix

`event_log_init()` now treats any non-zero return from `fcb_init()` as
a possible stale-format condition and performs an in-place reformat:

1. Log a `WRN` ("fcb_init failed: <rc> — wiping event-log partition").
2. `flash_area_open()` the partition.
3. `flash_area_erase(fa, 0, fa->fa_size)` — wipes all sectors.
4. `flash_area_close()`.
5. Re-zero the `struct fcb` (fcb_init mutates internal fields on the
   first attempt) and restore the configured magic/version/sectors.
6. Retry `fcb_init()` exactly once.
7. On second failure, return the original rc.
8. On success, log another `WRN` ("event-log partition reformatted
   after stale magic") so the reformat is always visible in the boot
   log.

The retry is capped at one attempt — a genuine hardware-level flash
fault will still surface as an error rather than spinning forever.

All 34 event_log unit tests still pass (they simulate a clean partition
so the recovery path is not exercised there; it's exercised only on
real hardware when the on-flash magic doesn't match).

---

## Files Touched

- `src/event_log.c` — `event_log_init()` recovery path.

---

## Alternatives Considered

1. **Refuse to boot on magic mismatch.** Rejected — the event log is a
   diagnostic subsystem, not a safety-critical one. Losing history is
   acceptable; bricking the device is not.
2. **Store magic in `settings` and migrate entries.** Rejected —
   entries are tiny, ring-buffered, and ephemeral by nature. Migrating
   them has no value.
3. **Erase the partition unconditionally at boot when the FW version
   changes.** Rejected — this would wipe the log on every legitimate
   OTA upgrade, even ones that don't change the FCB format.

The lazy "erase only when fcb_init rejects the existing content"
approach matches what MCUboot itself does for its scratch area.

---

## Lessons Learned

- OTA data migration must be considered **every time** an on-flash
  struct changes — even log formats.
- FCB returning `-ENOMSG` is not a hardware fault; it's the designed
  signal for "no valid FCB formatted here, please initialise one".
- A single `LOG_ERR` without recovery is a footgun when a subsystem is
  non-critical: prefer "warn + recover" over "err + give up" for
  observability subsystems.
- Bug reports #006 (MCUboot no bootable image) and this one share a
  root theme: data partitions survive OTA, so anything written there
  by firmware N+1 must be prepared to find firmware N's layout.
