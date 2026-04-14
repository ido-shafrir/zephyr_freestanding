# Bug Report #006: MCUboot Fails to Boot App — Three Independent Issues

**Date:** 2026-04-14  
**Severity:** High  
**Status:** Resolved  
**Component:** Build system / Sysbuild DTS overlays / MCUboot config  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

After building with `west build -b nucleo_h753zi --sysbuild --pristine`
and flashing with `west flash`, MCUboot failed to boot the application.
Three independent issues were discovered and fixed sequentially:

1. **MCUboot built without the app's flash partition overlay** — slot
   addresses didn't match, MCUboot looked for the image at the wrong
   location.
2. **MCUboot linked to slot0 instead of boot_partition** — the base board
   DTS `chosen` node pointed `zephyr,code-partition` at `&slot0_partition`,
   so MCUboot was placed at `0x08020000` instead of `0x08000000`.
3. **MCUboot stack overflow during RSA-2048 signature verification** —
   the default main thread stack was too small for the crypto operations.

---

## Issue 1: MCUboot Missing Board Overlay (Partition Mismatch)

### Symptoms

```
I: Primary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
W: Failed reading image headers; Image=0
E: Unable to find bootable image
```

MCUboot read erased flash (`magic=unset`) at what it thought was slot0.

### Root Cause

In sysbuild, the application's board overlay (`boards/nucleo_h753zi.overlay`)
is only applied to the **application** build. MCUboot is a separate CMake
project and does NOT inherit the app's overlay. It uses the base board DTS
partitions:

| Partition | MCUboot saw | App used |
|-----------|-------------|----------|
| slot0 | `0x08020000`, **256 KB** | `0x08020000`, **896 KB** |
| slot1 | `0x08060000`, **384 KB** | `0x08100000`, **896 KB** |
| scratch | *(not defined)* | `0x081E0000`, **128 KB** |

MCUboot was looking for image headers with a 256 KB slot0 and a slot1
starting at `0x08060000` — completely wrong. The signed app image, while
correctly placed at `0x08020000`, had metadata (size, TLVs) that didn't
match what MCUboot expected for a 256 KB slot.

### Fix

Create a **separate DTS overlay for MCUboot** at
`sysbuild/mcuboot/boards/nucleo_h753zi.overlay` with the same partition
layout as the app overlay. Also requires an empty `sysbuild/mcuboot/prj.conf`
because Zephyr expects a `prj.conf` when the directory exists.

---

## Issue 2: MCUboot Linked to slot0 Instead of boot_partition

### Symptoms

After fixing Issue 1, the merged hex showed MCUboot and the app at the
**same address** (`0x08020000`):

```
MCUboot: 0x08020000 - 0x0802D463
App:     0x08020000 - 0x0804428B
```

Merging with `intelhex` raised `AddressOverlapError`.

### Root Cause

The base board DTS (`nucleo_h753zi.dts`) contains:

```dts
/ {
    chosen {
        zephyr,code-partition = &slot0_partition;
    };
};
```

MCUboot inherited this `chosen` node, causing it to be linked at slot0's
address (`0x08020000`) instead of the boot partition (`0x08000000`).
MCUboot must always be placed in the boot partition.

### Fix

Override `zephyr,code-partition` in MCUboot's board overlay:

```dts
/ {
    chosen {
        zephyr,code-partition = &boot_partition;
    };
};
```

After this fix:
```
MCUboot: 0x08000000 - 0x0800D463 (53 KB)
App:     0x08020000 - 0x0804428B (144 KB)
Gap:     76,700 bytes (no overlap)
```

---

## Issue 3: MCUboot Stack Overflow During RSA-2048 Verification

### Symptoms

After fixing Issues 1 and 2, MCUboot booted and found the image headers
but crashed during signature validation:

```
[00:00:00.013,000] <err> os: ***** MPU FAULT *****
[00:00:00.013,000] <err> os:   Data Access Violation
[00:00:00.013,000] <err> os:   MMFAR Address: 0x24005900
[00:00:00.013,000] <err> os: Faulting instruction address (r15/pc): 0x080071be
[00:00:00.013,000] <err> os: >>> ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0
```

### Root Cause

RSA-2048 signature verification uses mbedTLS internally, which allocates
large buffers on the stack for bignum arithmetic. The default MCUboot main
thread stack size (typically 2–4 KB) is insufficient. The STM32H7 MPU
detected the stack overflow at address `0x24005900` (in SRAM1).

### Fix

Increase MCUboot's main stack size in `sysbuild/mcuboot.conf`:

```ini
CONFIG_MAIN_STACK_SIZE=16384
```

16 KB provides plenty of headroom for RSA-2048 operations.

---

## Files Changed

| File | Change |
|------|--------|
| `sysbuild/mcuboot/boards/nucleo_h753zi.overlay` | **New** — partition layout matching the app + `chosen` override for `boot_partition` |
| `sysbuild/mcuboot/prj.conf` | **New** — empty (required by Zephyr when the directory exists) |
| `sysbuild/mcuboot.conf` | Added `CONFIG_MAIN_STACK_SIZE=16384` |

---

## Lessons Learned

1. **Sysbuild does NOT share DTS overlays between images.** MCUboot and the
   app are separate CMake projects. Each needs its own board overlay with
   matching partition layouts. Mismatched partitions cause MCUboot to look
   for images at the wrong flash addresses.

2. **MCUboot must override `zephyr,code-partition` to `&boot_partition`.**
   Many board DTS files set `zephyr,code-partition = &slot0_partition` for
   the app. MCUboot inherits this unless explicitly overridden, causing it
   to be linked at the wrong address.

3. **RSA-2048 needs a large stack in MCUboot.** The mbedTLS bignum operations
   used for RSA signature verification are deeply recursive and allocate
   large intermediates on the stack. 16 KB is a safe minimum.

4. **Verify MCUboot's compiled DTS before flashing.** Check
   `build/mcuboot/zephyr/zephyr.dts` to confirm partition addresses match
   the app. Check hex address ranges with `intelhex` to verify MCUboot is
   at `0x08000000` and the app is at `0x08020000`.

5. **Check image headers when MCUboot fails.** The MCUboot image header at
   offset 0 of the signed binary should have magic `0x96F3B83D` and a
   non-zero `hdr_size`. If both are correct, the issue is flash placement,
   not image signing.
