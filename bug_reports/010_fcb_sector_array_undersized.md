# Bug Report #010: FCB flash_area_get_sectors Returns -ENOMEM With Undersized Array

**Date:** 2026-04-24  
**Severity:** Major  
**Status:** Resolved  
**Component:** event_log module / Zephyr FCB  
**Board:** qemu_x86 (flash simulator), nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  
**Reporter / Developer:** AI-Agent  

---

## Summary

`event_log_init()` failed with error `-12` (`ENOMEM`) on platforms where
the flash erase-block size differs from the production hardware.  The root
cause was a statically-sized `flash_sector` array that matched the expected
production geometry (2 × 128 KB sectors) but was too small for platforms
with finer-grained erase blocks (e.g. the QEMU flash simulator with
1024-byte erase blocks).

---

## Symptoms

```
E: Failed to get sector info: -12
```

`event_log_init()` returned `-12` on every call.  All subsequent write,
read, and clear operations failed because FCB was never initialised.
This caused 16 of 22 unit tests to fail on `qemu_x86`.

On the real `nucleo_h753zi` target the bug was latent — the STM32H753
has 128 KB erase blocks, so a 256 KB partition has exactly 2 sectors,
which fit the original array size.

---

## Root Cause

The original code:

```c
#define EVENT_LOG_SECTOR_COUNT  2

static struct flash_sector sectors[EVENT_LOG_SECTOR_COUNT];

int event_log_init(void)
{
    uint32_t sector_count = EVENT_LOG_SECTOR_COUNT;  /* max = 2 */
    rc = flash_area_get_sectors(PARTITION_ID(event_log_partition),
                                &sector_count, sectors);
    /* rc == -ENOMEM when actual sectors > 2 */
}
```

`flash_area_get_sectors()` fills the caller-provided array and returns
`-ENOMEM` if the array is smaller than the actual number of flash sectors
in the partition.

| Platform | Erase block | Partition size | Actual sectors | Array size | Result |
|----------|------------|----------------|----------------|------------|--------|
| STM32H753 | 128 KB | 256 KB | 2 | 2 | OK |
| qemu_x86 flash_sim | 1024 B | 64 KB | 64 | 2 | **-ENOMEM** |
| qemu_x86 flash_sim | 1024 B | 3 KB | 3 | 2 | **-ENOMEM** |

---

## Fix Applied

Changed the static array to a generous maximum and pass that size to the
query:

```diff
-#define EVENT_LOG_SECTOR_COUNT  2
-static struct flash_sector sectors[EVENT_LOG_SECTOR_COUNT];
+#define EVENT_LOG_SECTOR_COUNT  2
+#define EVENT_LOG_MAX_SECTORS  128
+static struct flash_sector sectors[EVENT_LOG_MAX_SECTORS];
```

```diff
-    uint32_t sector_count = EVENT_LOG_SECTOR_COUNT;
+    uint32_t sector_count = EVENT_LOG_MAX_SECTORS;
```

`flash_area_get_sectors()` now has room for up to 128 sectors. The actual
count returned by the driver is then passed to `fcb_init()`, so FCB uses
whatever geometry the hardware provides.

The cost is ~1.5 KB of static RAM (`128 × 12` bytes for `struct flash_sector`)
which is acceptable for this module.

---

## Affected Code

- `src/event_log.c` — `sectors[]` array declaration and
  `event_log_init()` sector query

---

## Lessons Learned

1. **Never size a sector array to the *expected* count.**  
   Different platforms have different erase-block sizes.  Always allocate
   generously or query the count in two passes (first pass with
   `count = 0` to learn the size, then allocate).

2. **`flash_area_get_sectors()` returns `-ENOMEM`, not a partial result.**  
   Unlike some APIs that fill what they can, this one fails entirely if
   the array is too small.

3. **Test on platforms with different flash geometry.**  
   The bug was invisible on the production target (STM32H753) and only
   surfaced on the QEMU flash simulator.  Cross-platform testing caught it.

4. **Error `-12` from flash_area APIs almost always means "buffer too small".**  
   It does not indicate an out-of-memory heap condition.
