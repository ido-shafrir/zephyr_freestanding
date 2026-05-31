# Bug Report #020: Build Fails With `undefined reference to z_impl_sys_rand_get` Until RNG Is Enabled

**Date:** 2026-05-08
**Severity:** Major (build break — blocks bring-up of any new out-of-tree board)
**Status:** Resolved
**Component:** Board DTS + defconfig
**Board:** Any new out-of-tree STM32 board (F4/L4/etc.) with networking enabled
**Zephyr Version:** 4.4.0+

---

## Summary

The first pristine sysbuild of a new out-of-tree board fails at the link
step with:

```
ld.bfd.exe: zephyr/subsys/net/ip/libsubsys__net__ip.a(net_context.c.obj):
  in function `sys_rand_get`:
  random.h:37: undefined reference to `z_impl_sys_rand_get'
ld.bfd.exe: zephyr/subsys/net/ip/libsubsys__net__ip.a(tcp.c.obj): ...
ld.bfd.exe: zephyr/subsys/net/lib/dhcpv4/libsubsys__net__lib__dhcpv4.a(dhcpv4.c.obj): ...
collect2.exe: error: ld returned 1 exit status
```

CMake configure passes, all .c objects compile, only the final ELF link
fails. Existing stock boards (e.g. `nucleo_h753zi`) work fine because
their upstream defconfig already enables entropy.

---

## Root Cause

The networking subsystem (`net_context`, `tcp`, `dhcpv4`, etc.) **always**
calls `sys_rand_get()` for source-port selection, TCP ISN, DHCP
transaction-ID jitter, etc. With `CONFIG_NET_NATIVE=y` the symbol is
referenced unconditionally; its implementation comes from one of:

- `drivers/entropy/*` (hardware RNG) when `CONFIG_ENTROPY_GENERATOR=y`
  AND a `chosen { zephyr,entropy }` device is present and `okay`, **or**
- the software fallback in `subsys/random/rand32_*.c` enabled by
  `CONFIG_TEST_RANDOM_GENERATOR=y`.

On a new board:

1. The board DTS does **not** mark the `&rng` node `status = "okay"`.
   The SoC dtsi defines the node and even adds
   `chosen { zephyr,entropy = &rng; }` — but leaves the peripheral
   disabled by default, as upstream does for every peripheral.
2. The board defconfig does not select `CONFIG_ENTROPY_GENERATOR=y`.

With both knobs off, no entropy driver is compiled in, no fallback is
enabled, and the `z_impl_sys_rand_get` symbol never exists — hence the
link error.

The error message is unhelpful: it tells you a C symbol is missing, not
that you forgot to enable a peripheral.

---

## Fix

In the board DTS overlay:

```dts
&rng {
    status = "okay";
};
```

In the board defconfig:

```kconfig
# Hardware entropy (STM32 RNG) — required for the network stack's
# sys_rand_get (DHCP, TCP ISN, mbedTLS).
CONFIG_ENTROPY_GENERATOR=y
```

---

## Lessons Learned

- **For every new out-of-tree Zephyr board that runs a TCP/IP stack,
  enable RNG up front.** Two-line change, saves a confusing link failure.
- **`CONFIG_ENTROPY_GENERATOR` is not auto-selected by `CONFIG_NET_NATIVE`.**
  The net stack references `sys_rand_get` without ensuring an
  implementation exists.
- **Vendor SoC dtsi files declare peripherals as `disabled`** — even
  ones that look "always-on" like the RNG. Always check `status` for
  any node your code or Kconfig references.

---

## References

- `zephyr/subsys/net/ip/net_context.c` — calls `sys_rand_get()` for
  ephemeral port selection.
- `zephyr/subsys/net/ip/tcp.c` — RTO derivation and ISN.
- `zephyr/subsys/net/lib/dhcpv4/dhcpv4.c` — message-timeout jitter.
- `zephyr/drivers/entropy/Kconfig` — `CONFIG_ENTROPY_GENERATOR`.
