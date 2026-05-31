# Bug Report #013: Time Service Test Hangs on qemu_x86 Due to Full Networking Stack

**Date:** 2026-04-24
**Severity:** Major (test suite cannot run)
**Status:** Resolved (worked around with local SNTP shim header)
**Component:** tests/test_time_service
**Board:** qemu_x86
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** AI-Agent

---

## Summary

The initial `tests/test_time_service/prj.conf` enabled the full
networking stack (`CONFIG_NETWORKING=y`, `CONFIG_NET_IPV4=y`,
`CONFIG_NET_SOCKETS=y`, `CONFIG_DNS_RESOLVER=y`,
`CONFIG_NET_CONFIG_SETTINGS=y`, `CONFIG_SNTP=y`) so the test binary could
link `sntp_simple()`.  On `qemu_x86` — which has no real network — the
DHCP client and DNS resolver blocked on boot, and twister reported
`"unexpected eof"` after the 120-second timeout with **zero** tests
executed.

---

## Symptoms

```
ERROR   - qemu_x86/atom  app.time_service   FAILED: Timeout
... handler.log:  (no ztest output)  unexpected eof
```

15 tests failed to run, even though all are pure logic tests
(ISO 8601 helpers, clock set/get) that do not require networking.

---

## Root Cause

`time_service.c` unconditionally calls `sntp_simple()`, which is
declared in `<zephyr/net/sntp.h>` and links against the networking
library.  Enabling that library pulls in `net_config_init_app()` which
waits synchronously for a DHCPv4 lease.  With no network available, the
call never returns; the test binary never reaches `main()` and never
runs the ztest suite.

Removing `CONFIG_NETWORKING` caused a linker error:
`undefined reference to 'sntp_simple'`.

---

## Fix Applied

Add a **local SNTP shim header** inside the test's include search path
that shadows the real `<zephyr/net/sntp.h>`, combined with a stub that
returns `-ETIMEDOUT`:

1. `tests/test_time_service/CMakeLists.txt` adds `src/` to the include
   path **before** the Zephyr include tree.
2. `tests/test_time_service/src/zephyr/net/sntp.h` provides a minimal
   `struct sntp_time` and `sntp_simple()` prototype — no networking
   dependency.
3. `tests/test_time_service/src/stubs.c` implements `sntp_simple()` as
   a stub that always returns `-ETIMEDOUT`.
4. `prj.conf` drops ALL networking options — only `CONFIG_ZTEST=y`,
   `CONFIG_POSIX_API=y`, `CONFIG_POSIX_TIMERS=y`, `CONFIG_LOG=y`.

```cmake
# tests/test_time_service/CMakeLists.txt
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src    # local SNTP shim first
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
```

---

## Verification

- `west twister -T tests/test_time_service -p qemu_x86`
  → 15 / 15 tests pass in ~100 seconds.
- No networking warnings or DHCP timeouts in boot log.

---

## Lessons Learned

1. For pure logic tests of modules that link against the networking
   stack, a **local shim header + stub** is far more reliable than
   enabling networking on emulated targets.
2. `qemu_x86` is fine for POSIX / filesystem / FCB tests, but its
   SLIP-less default network config never completes DHCP and will
   deadlock any test that initialises `net_config`.
3. Always check `handler.log` when a test reports
   `"unexpected eof"` — an empty handler log almost always means the
   binary hung *before* ztest started.
