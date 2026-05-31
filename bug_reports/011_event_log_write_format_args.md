# Bug Report #011: event_log_write Called With printf-Style Format Arguments

**Date:** 2026-04-24  
**Severity:** Minor  
**Status:** Resolved  
**Component:** REST API endpoints / event_log module  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  
**Reporter / Developer:** AI-Agent  

---

## Summary

Ten call sites in `src/rest_api_endpoints.c` passed printf-style format
arguments to `event_log_write()`, which accepts a plain `const char *msg`
parameter — not a variadic format string.  This caused build failures with
"too many arguments to function" errors.

---

## Symptoms

```
src/rest_api_endpoints.c:680:9: error: too many arguments to function 'event_log_write'
src/rest_api_endpoints.c:684:9: error: too many arguments to function 'event_log_write'
... (10 errors total)
```

The firmware failed to build.

---

## Root Cause

`event_log_write()` is declared as:

```c
int event_log_write(enum event_severity sev, enum event_type type,
                    const char *msg);
```

But the call sites used printf-style formatting:

```c
event_log_write(EVENT_SEV_INFO, EVENT_TYPE_CONFIG,
                "systemName changed to %s", upd.system_name);
```

This compiled as 4 arguments to a 3-parameter function.  The API was
intentionally designed as non-variadic to keep the FCB write path simple
and avoid dynamic allocation, but the call sites were written as if it
were `LOG_INF()`-style.

---

## Fix Applied

Pre-format messages with `snprintf()` into a stack buffer before passing
to `event_log_write()`:

```diff
+    char evt_msg[64];
+
     if (upd.has_system_name) {
         config_store_set_system_name(upd.system_name);
-        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_CONFIG,
-                        "systemName changed to %s", upd.system_name);
+        snprintf(evt_msg, sizeof(evt_msg),
+                 "systemName changed to %s", upd.system_name);
+        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_CONFIG, evt_msg);
     }
```

Applied to all 10 affected call sites in the PATCH `/api/mcu` handler.

---

## Affected Code

- `src/rest_api_endpoints.c` — PATCH `/api/mcu` handler, per-field
  config change event writes (10 call sites)

---

## Lessons Learned

1. **Non-variadic string APIs are easy to misuse as printf-style.**  
   When an API takes `const char *msg`, callers instinctively add format
   specifiers.  Consider either making the API variadic or adding a clear
   comment/docstring noting "this is NOT a format string."

2. **Pre-format into a stack buffer when the API takes a plain string.**  
   A 64-byte `char[]` on the stack is cheap and avoids heap allocation.
   `snprintf()` safely truncates if the formatted message exceeds the
   buffer.

3. **The compiler catches this immediately** — unlike variadic functions
   where extra arguments silently compile.  Non-variadic signatures are
   actually safer in this regard.
