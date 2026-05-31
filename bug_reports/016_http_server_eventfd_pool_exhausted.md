# Bug Report #016: HTTP/2 Server Loops On `eventfd failed (-12)` — ZVFS Eventfd Pool Too Small

**Date:** 2026-04-24
**Severity:** Major
**Status:** Resolved
**Component:** Zephyr HTTP server / ZVFS eventfd pool
**Board:** nucleo_h753zi (STM32H753ZI)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** (redacted)


---

## Summary

On boot, the HTTP/2 server worker repeatedly failed to allocate its
control eventfd and spammed the log with `net_http_server: eventfd
failed (-12)` forever. Root cause was `CONFIG_ZVFS_EVENTFD_MAX=1`
(the Kconfig default), which is consumed by another subsystem (SMP
UDP transport / mcumgr) during early boot. The HTTP server then had
zero eventfd slots left and `zvfs_eventfd(0, 0)` returned `-ENOMEM`.

---

## Symptoms

```
[00:00:02.500,000] <err> net_http_server: eventfd failed (-12)
[00:00:02.510,000] <err> net_http_server: eventfd failed (-12)
[00:00:02.520,000] <err> net_http_server: eventfd failed (-12)
... (indefinite, ~100/s)
```

Side effects:

- REST API unreachable — no socket was ever created.
- Console / shell / UART responsive, so the device looked "half alive".
- CPU pegged by the retry loop.
- `CONFIG_ZVFS_EVENTFD_MAX=1` in the generated `.config` (visible via
  `Get-Content build/zephyr/.config | Select-String EVENTFD`).

---

## Root Cause

`zephyr/subsys/net/lib/http/http_server_core.c`:

```c
/* Create an eventfd that can be used to trigger events during poll(). */
fd = zvfs_eventfd(0, 0);
if (fd < 0) {
    LOG_ERR("eventfd failed (%d)", fd);
    ...  /* retry on next accept iteration */
}
```

The ZVFS eventfd pool is a fixed-size table whose depth is set by
`CONFIG_ZVFS_EVENTFD_MAX`. Its Kconfig default is **1**. With both
mcumgr (UDP SMP transport) and HTTP/2 server enabled, two eventfd
slots are required just to bring both subsystems online, plus headroom
for anything else that uses `zvfs_eventfd()`.

With only one slot available, whichever subsystem initialised first
got it, and the other logged a fatal-looking error on every retry.

---

## Impact

- REST API (and by extension OTA control, health checks, config
  writes) silently unavailable after boot.
- Log flood made it hard to see any other output on the console.
- This was a latent configuration bug — it would fire any time the
  boot order happened to allocate the one eventfd to mcumgr first.

---

## Fix

Added to `prj.conf` in the HTTP server block:

```ini
# HTTP server needs its own eventfd; SMP UDP transport grabs the default one.
# Bump the pool so both subsystems can allocate one without contention.
CONFIG_ZVFS_EVENTFD_MAX=4
```

`.config` now shows:

```
CONFIG_ZVFS_EVENTFD_MAX=4
CONFIG_ZVFS_OPEN_ADD_SIZE_EVENTFD=4
```

Chose `4` (not `2`) to leave headroom for future subsystems (e.g.
additional HTTP services, websocket upgrade path, DHCP client retry
channels) without having to revisit this file.

---

## Files Touched

- `prj.conf` — added `CONFIG_ZVFS_EVENTFD_MAX=4` in the HTTP server
  block with a comment explaining why.

---

## Alternatives Considered

1. **Patch the HTTP server to back off on `-ENOMEM`.** Rejected — the
   upstream retry behaviour is correct in principle; the real bug is
   our undersized pool. Don't paper over Kconfig sizing issues in
   application code.
2. **Disable HTTP/2 (`CONFIG_HTTP_SERVER_VERSION_2=n`).** Rejected —
   HTTP/2 is a product requirement for the REST API.
3. **Use `CONFIG_ZVFS_EVENTFD_MAX=2` (exact fit).** Rejected — too
   fragile. Any future feature that takes an eventfd would reproduce
   this exact bug.

---

## Lessons Learned

- `-ENOMEM` from a "create a small kernel object" call usually means a
  fixed-size Kconfig pool, not a heap exhaustion — always search the
  Kconfig for `*_MAX` on the relevant subsystem before suspecting
  memory pressure.
- Default Kconfig values are tuned for minimal examples, not for
  realistic applications that stack several subsystems. Audit any
  `*_MAX` / `*_NUM_*` that defaults to `1` whenever enabling a new
  subsystem that might share the resource.
- An HTTP server that silently cannot bind is worse than one that
  fails loudly at startup. Consider a startup health check that
  asserts the listening socket exists after a timeout.
