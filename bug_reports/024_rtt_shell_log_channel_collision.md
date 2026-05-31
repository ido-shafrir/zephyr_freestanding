# Bug Report #024: RTT Shell and Log Backends Both Default to Channel 0 → BUILD_ASSERT

**Date:** 2026-04-28
**Severity:** Medium (build-time failure with confusing message)
**Status:** Resolved (workaround documented)
**Component:** Zephyr RTT subsystem, Kconfig overlay
**Board:** Any board using both shell-RTT and log-RTT backends
**Zephyr Version:** 4.4.0+

---

## Summary

Enabling both `CONFIG_SHELL_BACKEND_RTT=y` and `CONFIG_LOG_BACKEND_RTT=y`
simultaneously causes a `BUILD_ASSERT` from `subsys/shell/backends/shell_rtt.c`:

```
error: static assertion failed: "Conflicting log RTT backend enabled
on the same channel"
```

Both backends default to RTT up-channel **0**, which is illegal — only
one feature can own a given channel. A second pitfall appears at run time:
J-Link's GDB-server RTT plugin only relays one RTT channel per debug
session.

---

## Symptoms

1. Pristine build terminates with:
   ```
   shell_rtt.c:19:1: error: static assertion failed:
   "Conflicting log RTT backend enabled on the same channel"
   BUILD_ASSERT(!(CONFIG_SHELL_BACKEND_RTT_BUFFER ==
                  CONFIG_LOG_BACKEND_RTT_BUFFER), …)
   ```

2. Naively bumping `CONFIG_LOG_BACKEND_RTT_BUFFER=1` fixes the build,
   but J-Link errors at launch:
   ```
   Port/channel 1 selected but another decoder is using port 0.
   ```

---

## Root Cause

The Zephyr RTT subsystem allocates one ring buffer per up-channel.
Both `CONFIG_SHELL_BACKEND_RTT_BUFFER` and `CONFIG_LOG_BACKEND_RTT_BUFFER`
default to `0`. The `shell_rtt` backend has a `BUILD_ASSERT` that
catches the collision.

The probe-side single-channel limit is a **J-Link** vendor restriction,
not a Zephyr issue. OpenOCD can serve multiple RTT channels on separate
TCP ports.

---

## Fix

### Option A: Single-channel (J-Link compatible, recommended)

Drop the dedicated RTT log backend and route all log output through the
shell backend (`CONFIG_SHELL_LOG_BACKEND=y`, default when shell is enabled):

```kconfig
CONFIG_LOG=y
CONFIG_LOG_BACKEND_RTT=n
CONFIG_LOG_BACKEND_UART=n
CONFIG_LOG_MODE_DEFERRED=y
CONFIG_SHELL_LOG_BACKEND=y

CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_RTT=y
CONFIG_SHELL_BACKEND_SERIAL=n
CONFIG_SHELL_BACKEND_RTT_BUFFER=0
```

### Option B: Two-channel (OpenOCD only)

```kconfig
CONFIG_LOG_BACKEND_RTT=y
CONFIG_LOG_BACKEND_RTT_BUFFER=1
CONFIG_LOG_BACKEND_RTT_BUFFER_SIZE=1024
CONFIG_SEGGER_RTT_MAX_NUM_UP_BUFFERS=3
CONFIG_SEGGER_RTT_MAX_NUM_DOWN_BUFFERS=3
CONFIG_SHELL_BACKEND_RTT_BUFFER=0
```

OpenOCD then needs two `rtt server start` invocations:
```
rtt server start 9090 0   # shell terminal
rtt server start 9091 1   # log terminal
```

---

## Lessons Learned

1. **The `BUILD_ASSERT` mentions "channel" but the Kconfig options
   are named `*_BUFFER`.** Grep for `_BUFFER`, not `_CHANNEL`.

2. **`CONFIG_SHELL_LOG_BACKEND=y` is enabled by default whenever the
   shell is enabled.** That makes the dedicated `LOG_BACKEND_RTT`
   redundant for single-channel probes.

3. **Probe vendor matters for RTT topology.** A config that works on
   OpenOCD will fail on J-Link with a misleading error.

4. **Use `CONFIG_LOG_MODE_DEFERRED=y`** with shell-RTT to avoid
   deadlocks when log statements fire from ISR context.

---

## References

- Zephyr source: `subsys/shell/backends/shell_rtt.c` (the asserting
  backend) and `subsys/logging/backends/log_backend_rtt.c`.
- Segger J-Link UM08001 user manual, "RTT" chapter — single-channel
  limitation in the J-Link GDB server's RTT plug-in.
