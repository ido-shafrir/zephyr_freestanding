# Bug Report #026: New Defaults Don't Take Effect on Already-Deployed Devices (ZMS Persists Prior Values)

**Date:** 2026-04-30
**Severity:** Low (expected behavior, but silently surprising)
**Status:** Documented (no code fix required)
**Component:** `src/config_store.c`, ZMS-backed settings subsystem
**Board:** All
**Zephyr Version:** 4.4.0+

---

## Summary

When the firmware ships with a changed compile-time default for a
config_store key (e.g. `DEFAULT_NTP_SERVER` `"216.239.35.0"` → `""`),
**already-deployed devices that previously saved any value for that key
keep using the old value** after the OTA. The new defaults only apply to
keys that have never been written (e.g. fresh-from-factory units, or
brand-new keys this release introduces).

This is **not a bug in the settings subsystem** — it is the documented
ZMS / settings semantic ("loaded value wins over compile-time default").
But it is silently surprising for anyone reading the change as "just
flip the default."

---

## Symptoms

1. Brand-new device after first flash: `config_store_dump()` shows
   the new defaults.
2. Same firmware OTA'd onto a unit running previous version: old values
   persist for previously-written keys. Only new keys adopt the new
   compile-time defaults.
3. Network traces on deployed units show behavior matching old defaults,
   not new ones.

---

## Root Cause

`settings_load()` calls the module's `set` handler for every key found
in ZMS. When the previous firmware called `settings_save_one("key",
value, len)`, that record sits in flash. The new firmware's compile-time
default is the *initial value of the static buffer*, but `settings_load()`
overwrites it from the ZMS record before any consumer reads it.

New keys are unaffected only because no record exists for them yet.

---

## Resolution Options

1. **Field instruction (simplest)**: after OTA, run a REST PATCH or
   UART command to explicitly set the desired values, then power-cycle.
2. **One-shot migration in config init**: after `settings_load`, detect
   "legacy" values and overwrite. Brittle — depends on knowing all
   legacy strings and conflicts with operators who deliberately set
   those values.
3. **Schema-version key**: add a `config/schemaVersion` integer; on
   bump, run a per-version migration function. Worth doing if more
   "default flip" changes are anticipated.

---

## Lessons Learned

- "Change a default" in any settings-backed config is a **two-step**
  change in the field: code update **plus** a one-time data migration
  per unit. Always document both.
- Brand-new keys are safe to introduce — they pick up the new
  compile-time default on every unit at first boot.
- For features that *must* match a new value, prefer adding new keys
  rather than retasking existing ones.
- A factory-reset command is the cleanest recovery path because it
  re-saves every key with the current compile-time defaults.

---

## References

- Settings subsystem semantics:
  https://docs.zephyrproject.org/latest/services/storage/settings/index.html
- Bug #008 — settings NVS uint16 limit (related config_store issue).
- Bug #019 — event_log boot_id (cross-boot persistence assumption).
