# Flash Memory Layout — STM32H753ZI (Nucleo-H753ZI)

## Hardware

- **MCU:** STM32H753ZI
- **Internal flash:** 2 MB, dual-bank
  - Bank 1: `0x08000000`–`0x080FFFFF` (1 MB, 8 × 128 KB sectors)
  - Bank 2: `0x08100000`–`0x081FFFFF` (1 MB, 8 × 128 KB sectors)
- **Sector size:** 128 KB (all sectors are uniform on H753)

## Partition Map

DTS offsets are relative to the flash base (`0x08000000`).

```
 Flash Base: 0x08000000
 ┌─────────────────────────────────────────────────────────────┐
 │                        Bank 1 (1 MB)                        │
 ├──────────────┬──────────────────────────────┬───────────────┤
 │ boot (128K)  │        slot0 (768K)          │ slot1 start   │
 │ MCUboot      │        Primary App           │ (128K here)   │
 │ 0x00000      │        0x20000               │ 0xE0000       │
 │  – 0x1FFFF   │         – 0xDFFFF            │  – 0xFFFFF    │
 │ [1 sector]   │        [6 sectors]           │ [1 sector]    │
 ├──────────────┴──────────────────────────────┴───────────────┤
 │                        Bank 2 (1 MB)                        │
 ├────────────────────────────┬──────────┬─────────────────────┤
 │  slot1 cont. (640K)        │scratch   │  storage (256K)     │
 │  Upgrade Image             │(128K)    │  ZMS Settings       │
 │  0x100000                  │0x1A0000  │  0x1C0000           │
 │   – 0x19FFFF               │–0x1BFFFF │   – 0x1FFFFF        │
 │  [5 sectors]               │[1 sector]│  [2 sectors]        │
 └────────────────────────────┴──────────┴─────────────────────┘
```

## Partition Table

| Partition | DTS Label | Offset | End | Size | Sectors | Purpose |
|-----------|-----------|--------|-----|------|---------|---------|
| `boot_partition` | `mcuboot` | `0x00000` | `0x1FFFF` | 128 KB | 1 | MCUboot bootloader |
| `slot0_partition` | `image-0` | `0x20000` | `0xDFFFF` | 768 KB | 6 | Primary application image |
| `slot1_partition` | `image-1` | `0xE0000` | `0x19FFFF` | 768 KB | 6 | Upgrade image (OTA target) |
| `scratch_partition` | `image-scratch` | `0x1A0000` | `0x1BFFFF` | 128 KB | 1 | MCUboot swap scratch |
| `storage_partition` | `storage` | `0x1C0000` | `0x1FFFFF` | 256 KB | 2 | ZMS persistent settings |

**Total used:** 2048 KB (100% of 2 MB — zero waste)

## Design Rationale

### Slot sizing (768 KB)

The application image is currently small (~20–160 KB signed). 768 KB provides
ample headroom for firmware growth. Both slots must be the same size for
MCUboot's swap algorithm.

### Storage placement (end of flash)

The storage partition is placed at `0x1C0000`–`0x1FFFFF` — the last 2 sectors
of flash (bank 2). This position is critical:

- **Outside slot0/slot1:** MCUboot only swaps data within `slot0_partition`
  and `slot1_partition` (using `scratch_partition`). The storage partition
  is never read, erased, or written during an OTA swap.
- **Survives OTA updates:** All ZMS settings persist across firmware updates.
- **Survives MCUboot revert:** If a new firmware fails the health check and
  MCUboot reverts to the old image, settings are still intact.

### Slot1 crosses the bank boundary

slot1 starts at `0xE0000` (last sector of bank 1) and continues into bank 2
up to `0x19FFFF`. The STM32H753 flash driver handles cross-bank access
transparently — MCUboot sees a flat 2 MB address space.

### Scratch sizing (128 KB)

Scratch must be ≥ 1 sector (128 KB on STM32H753). MCUboot swaps one sector
at a time: copies sector from slot0 to scratch, copies sector from slot1 to
slot0, copies scratch to slot1. One sector is sufficient.

### MCUboot sector count

`CONFIG_BOOT_MAX_IMG_SECTORS=16` in `sysbuild/mcuboot.conf`. Each slot has
6 sectors; 16 provides headroom if the layout is adjusted later.

## DTS Definition

The partition layout is defined in two overlays that **must match**:

- [`boards/nucleo_h753zi.overlay`](../boards/nucleo_h753zi.overlay) — App overlay (includes storage + `chosen` node)
- [`sysbuild/mcuboot/boards/nucleo_h753zi.overlay`](../sysbuild/mcuboot/boards/nucleo_h753zi.overlay) — MCUboot overlay (matching slots + scratch)

See the [OTA Guide](zephyr_ota_guide.md) for details on why two overlays
are needed.

## Relationship to OTA

```
MCUboot reads:   boot_partition, slot0_partition, slot1_partition, scratch_partition
MCUboot ignores: storage_partition

OTA swap area:   slot0 ↔ slot1 (via scratch)
Settings area:   storage_partition (at end of flash, untouched by swap)
```

After an OTA update:
1. MCUboot swaps slot0 ↔ slot1
2. New firmware boots and loads settings from `storage_partition` — unchanged
3. If OTA health check fails, MCUboot reverts the swap — settings still unchanged
