# Zephyr OTA Update Guide — MCUmgr over UDP with MCUboot

A comprehensive guide to the OTA (Over-The-Air) firmware update system in the ICB firmware, using MCUboot as the bootloader and MCUmgr's SMP protocol over UDP for image transfer.

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────────┐
│  Host PC                                                          │
│  mcumgr CLI ──── UDP :1337 ──── W5500 Ethernet                    │
└───────────────────────────────────────────────────────────────────┘             
                                                      
┌───────────────────── STM32H753ZI Flash (2 MB) ────────────────────┐
│                                                                   │
│  ┌──────────────┐  ┌───────────────────┐  ┌───────────────────┐   │
│  │  MCUboot     │  │  Slot 0 (primary) │  │  Slot 1 (upgrade) │   │
│  │  Bootloader  │  │  Running firmware │  │  ← uploaded image │   │
│  │  128 KB      │  │  896 KB           │  │  896 KB           │   │
│  │  0x08000000  │  │  0x08020000       │  │  0x08100000       │   │
│  └──────┬───────┘  └────────┬──────────┘  └───────────────────┘   │
│         │                   │                                     │
│         │   On reset:       │                                     │
│         │   1. Validate     │  ┌───────────────────┐              │
│         │      signatures   │  │  Scratch area     │              │
│         │   2. Swap if      │  │  128 KB           │              │
│         │      upgrade      │  │  0x081E0000       │              │
│         │      pending      │  │  (used during     │              │
│         └───────────────────┘  │   swap)           │              │
│                                └───────────────────┘              │
└───────────────────────────────────────────────────────────────────┘
```

### How an OTA Update Works (Step by Step)

1. **Upload**: The `mcumgr` CLI sends a signed firmware image to slot 1 via SMP over UDP port 1337.
2. **Mark pending**: The `mcumgr` CLI (or a test command) marks the image in slot 1 as "pending test".
3. **Reset**: The device resets (via `mcumgr reset` or physical button).
4. **MCUboot swap**: MCUboot detects the pending image, validates its signature, and swaps slot 0 ↔ slot 1 using the scratch area.
5. **Boot new image**: The new firmware boots from slot 0 in **test mode** (unconfirmed).
6. **Health check**: The OTA thread waits for all modules (network, UART, REST API) to report ready.
7. **Confirm**: If all modules are healthy within 30 seconds, `boot_write_img_confirmed()` is called. The image is now permanent.
8. **Or revert**: If the health check fails (timeout, crash, hang), the image stays unconfirmed. On the next reset, MCUboot swaps back to the previous known-good image.

---

## Flash Partition Layout

The STM32H753ZI has 2 MB of internal flash organized as dual-bank (1 MB each), with 8 sectors of 128 KB per bank.

| Partition | Label | Flash Address | Size | Sectors | Purpose |
|-----------|-------|---------------|------|---------|---------|
| `boot_partition` | `mcuboot` | `0x08000000` | 128 KB | Bank 1, sector 0 | MCUboot bootloader |
| `slot0_partition` | `image-0` | `0x08020000` | 896 KB | Bank 1, sectors 1–7 | Primary application image |
| `slot1_partition` | `image-1` | `0x08100000` | 896 KB | Bank 2, sectors 0–6 | Upgrade (staging) image |
| `scratch_partition` | `image-scratch` | `0x081E0000` | 128 KB | Bank 2, sector 7 | Swap scratch area |

This is defined in [`boards/nucleo_h753zi.overlay`](../boards/nucleo_h753zi.overlay):

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        boot_partition: partition@0 {
            label = "mcuboot";
            reg = <0x00000000 DT_SIZE_K(128)>;
            read-only;
        };

        slot0_partition: partition@20000 {
            label = "image-0";
            reg = <0x00020000 DT_SIZE_K(896)>;
        };

        slot1_partition: partition@100000 {
            label = "image-1";
            reg = <0x00100000 DT_SIZE_K(896)>;
        };

        scratch_partition: partition@1e0000 {
            label = "image-scratch";
            reg = <0x001E0000 DT_SIZE_K(128)>;
        };
    };
};
```

The `chosen` node tells Zephyr to link the application into slot 0:

```dts
/ {
    chosen {
        zephyr,code-partition = &slot0_partition;
    };
};
```

---

## Configuration Files

### `prj.conf` — Application Kconfig

These options are added to the application's `prj.conf`:

```ini
# ─── MCUboot (OTA) ───
CONFIG_BOOTLOADER_MCUBOOT=y          # App knows it runs under MCUboot

# ─── Flash subsystem ───
CONFIG_FLASH=y                       # Flash driver
CONFIG_FLASH_MAP=y                   # Named flash partitions
CONFIG_FLASH_PAGE_LAYOUT=y           # Page/sector geometry info
CONFIG_STREAM_FLASH=y                # Streaming flash writes (for large images)
CONFIG_IMG_MANAGER=y                 # Image manager (boot_write_img_confirmed, etc.)
CONFIG_IMG_ERASE_PROGRESSIVELY=y     # Erase slot1 sectors on-the-fly during upload

# ─── MCUmgr core ───
CONFIG_MCUMGR=y                      # MCUmgr framework
CONFIG_MCUMGR_GRP_IMG=y              # Image management command group (upload/list/test/confirm)
CONFIG_MCUMGR_GRP_OS=y               # OS command group (reset, echo)

# ─── MCUmgr SMP transport: UDP ───
CONFIG_MCUMGR_TRANSPORT_UDP=y        # Enable SMP over UDP
CONFIG_MCUMGR_TRANSPORT_UDP_IPV4=y   # IPv4 only
CONFIG_MCUMGR_TRANSPORT_UDP_PORT=1337 # Listen port
CONFIG_MCUMGR_TRANSPORT_UDP_MTU=1500  # MTU size
```

| Option | Purpose |
|--------|---------|
| `CONFIG_BOOTLOADER_MCUBOOT` | Enables MCUboot-aware image trailer handling. Without this, `boot_write_img_confirmed()` doesn't exist. |
| `CONFIG_IMG_ERASE_PROGRESSIVELY` | Erases slot 1 sectors as the image is written, instead of erasing all 896 KB upfront. Reduces upload start latency. |
| `CONFIG_MCUMGR_GRP_IMG` | Registers the SMP image management commands: upload, list, test, confirm, erase. |
| `CONFIG_MCUMGR_GRP_OS` | Registers `os reset` (used to trigger the swap after upload) and `os echo`. |
| `CONFIG_MCUMGR_TRANSPORT_UDP_PORT` | The UDP port the SMP server listens on. Default is 1337. Change if it conflicts with your network. |

### `sysbuild.conf` — Sysbuild Top-Level Config

```ini
SB_CONFIG_BOOTLOADER_MCUBOOT=y
```

This single line tells the Zephyr sysbuild system to co-build MCUboot alongside the application. Sysbuild handles:
- Building MCUboot from source with the correct flash partition layout
- Signing the application image with the MCUboot key
- Producing the merged hex file for initial flashing

### `sysbuild/mcuboot.conf` — MCUboot Bootloader Config

```ini
CONFIG_BOOT_SWAP_USING_SCRATCH=y     # Swap algorithm: slot0 ↔ slot1 via scratch
CONFIG_BOOT_MAX_IMG_SECTORS=16       # Max sectors per slot (7 actual, 16 for safety)
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_GPIO=y
CONFIG_LOG=y
CONFIG_MCUBOOT_LOG_LEVEL_INF=y
CONFIG_MCUBOOT_SERIAL=n              # No serial recovery — we use UDP
```

| Option | Purpose |
|--------|---------|
| `CONFIG_BOOT_SWAP_USING_SCRATCH` | Uses the scratch partition to safely swap images. Power-safe — interrupted swaps resume on next boot. |
| `CONFIG_BOOT_MAX_IMG_SECTORS` | Must be ≥ the number of sectors in the largest slot. Set to 16 for headroom. |
| `CONFIG_MCUBOOT_SERIAL` | Disabled. We use MCUmgr over UDP, not MCUboot's built-in serial recovery. |

---

## Source Files

### `include/ota.h` — Module Interface

Defines the health registry API:

```c
enum ota_module {
    OTA_MODULE_NET,       /* W5500 network */
    OTA_MODULE_UART,      /* Command UART */
    OTA_MODULE_REST_API,  /* REST API server */
    OTA_MODULE_COUNT      /* Sentinel — always last */
};

void ota_report_module_ready(enum ota_module mod);
void ota_thread_entry(void *p1, void *p2, void *p3);
```

### `src/ota.c` — Implementation

- **Health registry**: An `atomic_t` bitmask where each bit represents a module. Lock-free and ISR-safe via `atomic_or()`.
- **OTA thread**: Polls the bitmask every 500 ms for up to `OTA_CONFIRM_TIMEOUT_S` (30s). When all bits are set, calls `boot_write_img_confirmed()`.
- **MCUmgr SMP server**: Runs in its own Zephyr-managed thread automatically when `CONFIG_MCUMGR_TRANSPORT_UDP=y` is set. No explicit start needed in `ota.c`.

### Health Reporting Integration

Each module calls `ota_report_module_ready()` after successful initialization:

| Module | File | When reported |
|--------|------|---------------|
| `OTA_MODULE_NET` | `src/w5500_net.c` | After `init_net()` succeeds (W5500 up, DHCP started) |
| `OTA_MODULE_UART` | `src/command_uart.c` | After UART IRQ enabled and "ready" prompt sent |
| `OTA_MODULE_REST_API` | `src/rest_api.c` | After `http_server_start()` returns success |

### Thread in `main.c`

The OTA thread is created alongside the other threads:

```c
k_thread_create(&ota_thread_data, ota_stack, OTA_STACK_SIZE,
                ota_thread_entry, NULL, NULL, NULL,
                OTA_PRIORITY, 0, K_NO_WAIT);
```

Priority 8 — lowest in the system, since OTA confirmation is a background concern.

---

## Adding a New Module to the Health Check

To add a new module (e.g., a sensor driver) to the OTA health check:

### 1. Add an enum entry in `include/ota.h`

```c
enum ota_module {
    OTA_MODULE_NET,
    OTA_MODULE_UART,
    OTA_MODULE_REST_API,
    OTA_MODULE_SENSOR,    /* ← new */
    OTA_MODULE_COUNT
};
```

### 2. Add the name string in `src/ota.c`

```c
static const char *const module_names[] = {
    [OTA_MODULE_NET]      = "w5500_net",
    [OTA_MODULE_UART]     = "command_uart",
    [OTA_MODULE_REST_API] = "rest_api",
    [OTA_MODULE_SENSOR]   = "sensor",    /* ← new */
};
```

### 3. Call `ota_report_module_ready()` from your module

```c
#include "ota.h"

void sensor_thread_entry(void *p1, void *p2, void *p3)
{
    if (sensor_init() < 0) {
        return;  /* OTA will NOT confirm — triggers revert */
    }

    ota_report_module_ready(OTA_MODULE_SENSOR);

    while (1) { /* ... */ }
}
```

That's it. The OTA thread automatically checks all `OTA_MODULE_COUNT` bits.

---

## Building

### With MCUboot (OTA-enabled) — Sysbuild

```bash
west build -b nucleo_h753zi --sysbuild --pristine
```

This produces:
- `build/mcuboot/zephyr/zephyr.bin` — MCUboot bootloader
- `build/ICB-FW/zephyr/zephyr.signed.bin` — Signed application image
- `build/merged.hex` — Combined bootloader + app for initial flash

### Without MCUboot (development / direct flash)

```bash
west build -b nucleo_h753zi --pristine
```

This builds the application without MCUboot. The OTA thread detects `boot_is_img_confirmed() == true` (no bootloader trailer) and skips the health check. MCUmgr is still compiled in, but without MCUboot there's nothing to swap.

### Flashing (Initial — includes MCUboot)

```bash
west flash
```

When built with `--sysbuild`, this flashes the merged hex (bootloader + app). Subsequent updates use OTA.

---

## Signing Keys

### Development (Default)

Sysbuild uses MCUboot's built-in test key (`root-rsa-2048.pem`) by default. This is fine for development but **must not** be used in production — the private key is public.

### Production

1. Generate a key pair:
   ```bash
   imgtool keygen -k my-signing-key.pem -t rsa-2048
   ```

2. Set in `sysbuild/mcuboot.conf`:
   ```ini
   CONFIG_BOOT_SIGNATURE_KEY_FILE="path/to/my-signing-key.pem"
   ```

3. Rebuild with `--pristine` to embed the new public key in MCUboot.

> **Warning:** If you change the signing key, MCUboot will reject images signed with the old key. Always keep your production key backed up and secure.

---

## OTA Confirmation Flow

```
                        ┌──────────────────────────┐
                        │  MCUboot boots new image │
                        │  (test mode, unconfirmed)│
                        └─────────────┬────────────┘
                                      │
                        ┌─────────────▼────────────┐
                        │  OTA thread starts       │
                        │  boot_is_img_confirmed() │
                        │  → false (new image)     │
                        └─────────────┬────────────┘
                                      │
              ┌───────────────────────────────────────────┐
              │  Poll every 500ms (up to 30s timeout):    │
              │                                           │
              │   w5500_net reports ready?     ☐ → ☑     │
              │   command_uart reports ready?  ☐ → ☑     │
              │   rest_api reports ready?      ☐ → ☑     │
              └───────────────────┬───────────────────────┘
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
              All ready?                   Timeout?
                    │                           │
                    ▼                           ▼
     ┌──────────────────────┐    ┌──────────────────────────┐
     │ boot_write_img_      │    │ Image stays UNCONFIRMED  │
     │ confirmed()          │    │ Log: "HEALTH CHECK       │
     │ Image is PERMANENT   │    │       TIMEOUT"           │
     └──────────────────────┘    │ Next reset → MCUboot     │
                                 │ reverts to slot 1        │
                                 └──────────────────────────┘
```

---

## Performing an OTA Update

See [OTA Update Procedure](ota_update_procedure.md) for the step-by-step process using the `mcumgr` CLI.

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|--------------|-----|
| `mcumgr` can't connect on port 1337 | W5500 not up, wrong IP, firewall | Check device IP with `ip_get` UART command. Ensure UDP 1337 is not blocked. |
| Upload succeeds but device doesn't swap | Image not marked as "test" | Run `mcumgr image test <hash>` after upload, then `mcumgr reset`. |
| Device reverts after every OTA | Health check timing out | Check logs for which module is missing. Increase `OTA_CONFIRM_TIMEOUT_S` if needed. |
| `boot_write_img_confirmed()` fails | Not built with `CONFIG_BOOTLOADER_MCUBOOT=y` | Ensure `prj.conf` has the option. Rebuild with `--pristine`. |
| MCUboot fails to validate image | Wrong signing key | Ensure the image was signed with the key MCUboot was built with. |
| Flash partition overlap errors | DTS addresses wrong | Verify partition addresses don't overlap (see flash layout table above). |

---

## File Reference

| File | Purpose |
|------|---------|
| [`boards/nucleo_h753zi.overlay`](../boards/nucleo_h753zi.overlay) | Flash partition layout (DTS) |
| [`prj.conf`](../prj.conf) | Application Kconfig (MCUmgr, flash, MCUboot awareness) |
| [`sysbuild.conf`](../sysbuild.conf) | Sysbuild config (enables MCUboot co-build) |
| [`sysbuild/mcuboot.conf`](../sysbuild/mcuboot.conf) | MCUboot bootloader Kconfig |
| [`include/ota.h`](../include/ota.h) | OTA module interface + health registry API |
| [`src/ota.c`](../src/ota.c) | OTA thread + health-based confirmation logic |
| [`src/main.c`](../src/main.c) | Thread creation (OTA thread added here) |
| [`src/w5500_net.c`](../src/w5500_net.c) | Reports `OTA_MODULE_NET` ready |
| [`src/command_uart.c`](../src/command_uart.c) | Reports `OTA_MODULE_UART` ready |
| [`src/rest_api.c`](../src/rest_api.c) | Reports `OTA_MODULE_REST_API` ready |
| [`CMakeLists.txt`](../CMakeLists.txt) | Build system (includes `src/ota.c`) |
| [`docs/ota_update_procedure.md`](ota_update_procedure.md) | Step-by-step OTA upload procedure |
