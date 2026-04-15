# Bug Report #007: MCUmgr Silently Disabled — Missing ZCBOR Dependency

**Date:** 2026-04-14  
**Severity:** Minor  
**Status:** Resolved  
**Component:** Kconfig / MCUmgr  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

After building and flashing the OTA-enabled firmware, `smpmgr` could not
communicate with the device over UDP port 1337. Every SMP request timed out,
including the basic `os echo` command.

The root cause was that `CONFIG_MCUMGR` depends on `CONFIG_ZCBOR`, and
without it the entire MCUmgr menuconfig was silently dropped by Kconfig.
No build warning or error was emitted — the firmware compiled and ran fine,
but with zero MCUmgr functionality.

---

## Symptoms

```
smpmgr --ip 10.100.110.91 os echo hello

ERROR    Timeout (2.5s) waiting for request ...
         group_id=<GroupId.OS_MANAGEMENT: 0>, command_id=<OSManagement.MCUMGR_PARAMETERS: 6>
WARNING  Timeout waiting for MCUMgr parameters
⠙ Connecting to 10.100.110.91... OK
ERROR    Timeout (2.0s) waiting for request ...
         group_id=<GroupId.OS_MANAGEMENT: 0>, command_id=<OSManagement.ECHO: 0>
ERROR    Timeout waiting for response
⠴ Waiting for response to EchoWrite... timeout
```

The device was reachable (HTTP REST API worked, ping worked), but nothing
listened on UDP port 1337.

---

## Root Cause

MCUmgr's top-level Kconfig entry has two dependencies:

```kconfig
menuconfig MCUMGR
    bool "MCUmgr Support"
    depends on NET_BUF
    depends on ZCBOR
```

`NET_BUF` was already enabled (pulled in by the networking stack), but
`ZCBOR` was not. Without `ZCBOR`, the entire `MCUMGR` menu — and every
option under it — was silently excluded from the build.

### How it was missed

Zephyr's Kconfig does not warn when a `CONFIG_*=y` line in `prj.conf`
is dropped due to unmet dependencies. The build succeeded, the firmware
ran, and all non-MCUmgr features worked normally. The only clue was in
the resolved `.config` file:

```bash
# Check: is MCUMGR in the final config?
Select-String -Path "build/ICB-FW/zephyr/.config" -Pattern "MCUMGR"
# Result: only "# CONFIG_UART_CONSOLE_MCUMGR is not set" — no CONFIG_MCUMGR=y
```

---

## Fix

Add `CONFIG_ZCBOR=y` before `CONFIG_MCUMGR=y` in `prj.conf`:

```ini
# ─── MCUmgr core ───
CONFIG_ZCBOR=y
CONFIG_MCUMGR=y
```

After rebuilding, the resolved `.config` includes all MCUmgr options and
the SMP UDP transport starts correctly.

---

## Verification

```
smpmgr --ip 10.100.110.91 os echo hello
⠙ Connecting to 10.100.110.91... OK
hello
```

---

## Lessons Learned

1. **Always verify the resolved `.config` after adding Kconfig options.**
   Zephyr silently drops options with unmet dependencies. Check
   `build/ICB-FW/zephyr/.config` to confirm your settings took effect.

2. **MCUmgr requires ZCBOR.** SMP uses CBOR encoding for all messages.
   This dependency is declared in Kconfig but easy to miss since ZCBOR
   is not a common option to enable manually.
