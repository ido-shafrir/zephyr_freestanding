# Bug Report #018: OTA Health-Check Prints `MISSING: error` — `module_names[]` Missing `OTA_MODULE_TIME_SERVICE`

**Date:** 2026-04-24
**Severity:** Minor
**Status:** Resolved
**Component:** `src/ota.c` (health-check log formatter)
**Board:** nucleo_h753zi (STM32H753ZI)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** (redacted)

---

## Summary

When the OTA health-check timed out with `OTA_MODULE_TIME_SERVICE`
not yet ready, the log printed `MISSING: error` instead of
`MISSING: time_service`. Root cause: the `module_names[]` lookup
table in `ota.c` had six entries but the enum in `ota.h` had seven.
The slot for `OTA_MODULE_TIME_SERVICE` was an implicit `NULL`, which
`LOG_ERR("MISSING: %s", NULL)` rendered as the literal string
`"error"` (or whatever the formatter substitutes for a null `%s`).

---

## Symptoms

```
[00:00:31.626,000] <err> ota:   Ready modules: 6/7
[00:00:31.626,000] <err> ota:   MISSING: error          <-- should read "time_service"
[00:00:31.626,000] <err> ota:   MCUboot will REVERT to previous image on next reset.
```

The operator sees `MISSING: error`, searches the codebase for a
module named "error", finds nothing, and wastes time. The actual
missing module (`time_service`) is hidden.

---

## Root Cause

`include/ota.h`:

```c
enum ota_module {
    OTA_MODULE_NET,
    OTA_MODULE_UART,
    OTA_MODULE_REST_API,
    OTA_MODULE_CONFIG_STORE,
    OTA_MODULE_KEEPALIVE,
    OTA_MODULE_EVENT_LOG,
    OTA_MODULE_TIME_SERVICE,   // <-- added earlier this session
    OTA_MODULE_COUNT
};
```

`src/ota.c` (before fix):

```c
static const char *const module_names[] = {
    [OTA_MODULE_NET]          = "w5500_net",
    [OTA_MODULE_UART]         = "command_uart",
    [OTA_MODULE_REST_API]     = "rest_api",
    [OTA_MODULE_CONFIG_STORE] = "config_store",
    [OTA_MODULE_KEEPALIVE]    = "keepalive",
    [OTA_MODULE_EVENT_LOG]    = "event_log",
    /* <-- no entry for OTA_MODULE_TIME_SERVICE */
};
```

When `OTA_MODULE_TIME_SERVICE` (index 6) is looked up, the designated
initialiser leaves the slot as `NULL` because `ARRAY_SIZE` is driven
by the highest-indexed entry. The log formatter called
`LOG_ERR("  MISSING: %s", NULL)`, and Zephyr's log backend
substituted a fallback string — on this build, `"error"`.

There was no compile-time check tying the enum size to the array
size, so the omission was invisible at build time.

---

## Impact

- Log output actively misleads operators by showing `"error"`
  instead of a module name. Worse than a plain array-index dump,
  which at least would be a number they could cross-reference.
- No runtime crash — the log backend gracefully substitutes — so
  the bug silently shipped with the last firmware build.
- This same class of bug will reappear every time a new module is
  added to the enum without updating `module_names[]`.

---

## Fix

Two changes in `src/ota.c`:

1. Add the missing entry:
   ```c
   [OTA_MODULE_TIME_SERVICE] = "time_service",
   ```
2. Add a compile-time guard so future additions to the enum fail
   the build if they don't also update the table:
   ```c
   BUILD_ASSERT(ARRAY_SIZE(module_names) == OTA_MODULE_COUNT,
                "module_names[] must have one entry per ota_module enum value");
   ```

Note: `BUILD_ASSERT` + `ARRAY_SIZE` catches *short* tables (missing
entries at the end). A *hole* in the middle — e.g. a deleted enum
value that stays referenced — would not be caught, but is caught by
the upstream enum having no gaps. Good enough.

---

## Files Touched

- `src/ota.c` — added missing `module_names[]` entry and `BUILD_ASSERT`.

---

## Alternatives Considered

1. **Use a runtime NULL check** in the log loop (`const char *name
   = module_names[i]; if (!name) name = "<unknown>"`). Rejected —
   moves the error from the log line to a placeholder string, but
   doesn't prevent the underlying inconsistency. Compile-time check
   is strictly better.
2. **Generate the table from a single source** (X-macro over the
   enum). Considered, rejected for now — the enum is 7 entries, not
   70, and the `BUILD_ASSERT` is sufficient. Revisit if the list
   grows large.
3. **Print the module index as a fallback** (`MISSING: module #%d`).
   Rejected — works, but loses the human-readable name on the very
   day an operator most needs it.

---

## Lessons Learned

- Designated-initialiser arrays paired with an enum need a
  `BUILD_ASSERT(ARRAY_SIZE(arr) == ENUM_COUNT, ...)` every time, or
  the array will silently diverge from the enum.
- Printing `NULL` via `%s` is undefined behaviour strictly speaking,
  but glibc / Zephyr tend to substitute a string (`(null)`, `error`,
  `(nil)` — varies by backend) that looks alarmingly like a real log
  value. Any variable-format log line printing a lookup-table string
  is a potential trap.
- This bug was latent for the entire `OTA_MODULE_TIME_SERVICE`
  feature's lifetime; it only surfaced because #017's DHCP race
  caused the time_service module to miss the health-check window.
  Unrelated bugs compound.
