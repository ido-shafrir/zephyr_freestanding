# Bug Report #025: MCUboot Swap Mode Silently Overridden by Sysbuild Kconfig + Mismatched Slot Sectors

**Date:** 2026-04-28
**Severity:** High
**Status:** Resolved
**Component:** `sysbuild.conf`, `sysbuild/mcuboot.conf`, MCUboot, flash partition layout
**Board:** Any board with non-uniform flash sector geometry across slots
**Zephyr Version:** 4.4.0+, MCUboot from bootloader/mcuboot west manifest

---

## Summary

After uploading a signed image via SMP, marking slot 1 pending, and
resetting, the device boots back into the **old** image. The swap
silently did not happen — no MCUboot error messages on the console.

Two independent problems combine to produce the silent no-op:

1. The MCUboot swap mode was being chosen at the **sysbuild** scope,
   not at the MCUboot child-image scope. Setting
   `CONFIG_BOOT_UPGRADE_ONLY=y` in `sysbuild/mcuboot.conf` was
   silently overridden by sysbuild's generated `.config.sysbuild`.
2. The flash layout has **mismatched sector geometry between slot 0
   and slot 1**, which all swap-with-rollback algorithms (SCRATCH,
   MOVE, OFFSET) refuse to operate on.

---

## Symptoms

1. `image upload` succeeds; `image state-read` shows the new image
   in slot 1 with the expected version and hash.
2. `image state-write <slot1-hash>` returns OK; slot 1 shows
   `pending=True`.
3. After reset, slot 0 is **unchanged** (previous version,
   `confirmed=True`). Slot 1 eventually loses `pending`.
4. Build warnings that signal the geometry problem:
   ```
   WARNING: Unable to determine erase size of slot0 partition
   WARNING: Unable to determine erase size of slot1 partition
   WARNING: CONFIG_BOOT_MAX_IMG_SECTORS is not defined, falling back to 128
   ```
5. Inspecting `build/mcuboot/zephyr/.config`:
   ```
   CONFIG_BOOT_SWAP_USING_OFFSET=y
   # CONFIG_BOOT_UPGRADE_ONLY is not set
   ```
   despite `sysbuild/mcuboot.conf` attempting to set overwrite-only.

---

## Root Cause

### 1. Sysbuild overrides the swap-mode `choice`

In Zephyr's sysbuild Kconfig
(`zephyr/share/sysbuild/images/bootloader/Kconfig`), the swap mode is:

```kconfig
choice MCUBOOT_MODE
    default MCUBOOT_MODE_SWAP_USING_OFFSET
    ...
endchoice
```

Sysbuild propagates the resolved choice into the MCUboot child image
via `build/mcuboot/zephyr/.config.sysbuild`, merged **after**
`sysbuild/mcuboot.conf`. Any `CONFIG_BOOT_*` setting in
`sysbuild/mcuboot.conf` is ineffective — the `SB_CONFIG_MCUBOOT_MODE_*`
symbol at sysbuild scope is the only thing that matters.

### 2. Swap-with-rollback requires matching slot geometry

`BOOT_SWAP_USING_SCRATCH`, `BOOT_SWAP_USING_MOVE`, and
`BOOT_SWAP_USING_OFFSET` all need to compare per-sector swap state
between slot 0 and slot 1, assuming identical sector layouts. With
mismatched geometry, MCUboot's calculation fails and the swap is a
no-op.

---

## Fix

Set the swap mode at the **sysbuild scope** in `sysbuild.conf`:

```kconfig
SB_CONFIG_BOOTLOADER_MCUBOOT=y

# Swap method — must be selected at sysbuild scope.
# OVERWRITE_ONLY is required if slot0 and slot1 have different
# sector geometry.
SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y
```

Remove the now-redundant (no-op) `CONFIG_BOOT_UPGRADE_ONLY=y` from
`sysbuild/mcuboot.conf`.

### Trade-off

Overwrite-only has **no automatic rollback**. Recovery from a bad image
requires OTA re-flash or JTAG. To regain rollback, repartition slots
so their sector layouts match.

---

## Verification

1. Confirmed `build/mcuboot/zephyr/.config` contains
   `CONFIG_BOOT_UPGRADE_ONLY=y`.
2. OTA upload + state-write + reset → device boots on new image,
   runtime health check auto-confirms.

---

## Lessons Learned

- **Always set MCUboot swap mode at the sysbuild scope**
  (`SB_CONFIG_MCUBOOT_MODE_*`), not in `sysbuild/mcuboot.conf`.
- **The "Unable to determine erase size" warnings are not cosmetic** —
  they mean swap algorithms will silently no-op.
- **For swap-with-rollback on STM32 dual-bank flash**, ensure both
  slots reside in regions with uniform sector size (e.g. both in the
  128 KB region).

---

## References

- `zephyr/share/sysbuild/images/bootloader/Kconfig` — swap mode choice.
- Bug #005 — sysbuild OTA build failures (same class of issue).
- Bug #006 — MCUboot no bootable image (different cause, same symptom).
