# Zephyr OTA Update Guide — MCUmgr over UDP with MCUboot

A comprehensive guide to implementing OTA (Over-The-Air) firmware updates in Zephyr using MCUboot as the bootloader and MCUmgr's SMP protocol over UDP for image transfer, with a health-based auto-confirmation system.

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────────┐
│  Host PC                                                          │
│  smpmgr CLI ──── UDP :1337 ──── Ethernet                          │
└───────────────────────────────────────────────────────────────────┘

┌───────────────────── MCU Flash (example: 2 MB) ───────────────────┐
│                                                                   │
│  ┌──────────────┐  ┌───────────────────┐  ┌───────────────────┐   │
│  │  MCUboot     │  │  Slot 0 (primary) │  │  Slot 1 (upgrade) │   │
│  │  Bootloader  │  │  Running firmware │  │  ← uploaded image │   │
│  └──────┬───────┘  └────────┬──────────┘  └───────────────────┘   │
│         │                   │                                     │
│         │   On reset:       │  ┌───────────────────┐              │
│         │   1. Validate     │  │  Scratch area     │              │
│         │      signatures   │  │  (used during     │              │
│         │   2. Swap if      │  │   swap)           │              │
│         │      upgrade      │  └───────────────────┘              │
│         │      pending      │                                     │
│         └───────────────────┘                                     │
└───────────────────────────────────────────────────────────────────┘
```

### How an OTA Update Works

1. **Upload**: The `smpmgr` CLI sends a signed firmware image to slot 1 via SMP over UDP.
2. **Mark pending**: The CLI marks the image in slot 1 as "pending test".
3. **Reset**: The device resets.
4. **MCUboot swap**: MCUboot validates the signature and swaps slot 0 ↔ slot 1 via the scratch area.
5. **Boot new image**: The new firmware boots in **test mode** (unconfirmed).
6. **Health check**: The OTA thread waits for all application modules to report ready.
7. **Confirm or revert**: If all modules are healthy within the timeout, the image is confirmed. If the health check fails, MCUboot reverts on the next reset.

---

## Step 1: Kconfig — Application `prj.conf`

Add these options to enable MCUboot awareness and MCUmgr:

```ini
# ─── MCUboot (OTA) ───
CONFIG_BOOTLOADER_MCUBOOT=y

# ─── Flash subsystem ───
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_STREAM_FLASH=y
CONFIG_IMG_MANAGER=y
CONFIG_IMG_ERASE_PROGRESSIVELY=y

# ─── MCUmgr core ───
CONFIG_ZCBOR=y                            # REQUIRED — see bug #007
CONFIG_MCUMGR=y
CONFIG_MCUMGR_GRP_IMG=y                   # Image upload/list/test/confirm
CONFIG_MCUMGR_GRP_OS=y                    # OS reset, echo
CONFIG_MCUMGR_GRP_OS_MCUMGR_PARAMS=y
CONFIG_MCUMGR_SMP_VERBOSE_ERR_RESPONSE=y

# ─── MCUmgr SMP transport: UDP ───
CONFIG_MCUMGR_TRANSPORT_UDP=y
CONFIG_MCUMGR_TRANSPORT_UDP_IPV4=y
CONFIG_MCUMGR_TRANSPORT_UDP_PORT=1337
CONFIG_MCUMGR_TRANSPORT_UDP_MTU=1500
```

### CRITICAL: `CONFIG_ZCBOR=y` must be present

> **Bug lesson (see [bug_reports/007](../bug_reports/007_mcumgr_silently_disabled.md)):**
> `CONFIG_MCUMGR` depends on `CONFIG_ZCBOR`. Without it, Kconfig **silently drops**
> the entire MCUmgr menu — no build error, no warning. The firmware compiles and runs
> but with zero MCUmgr functionality (UDP port 1337 never opens).
>
> Always verify the resolved `.config` after adding Kconfig options:
> ```bash
> Select-String -Path "build/<app>/zephyr/.config" -Pattern "MCUMGR"
> ```

---

## Step 2: Sysbuild Configuration

### `sysbuild.conf` — Top-level

```ini
SB_CONFIG_BOOTLOADER_MCUBOOT=y
```

This tells the Zephyr sysbuild system to co-build MCUboot alongside the application.

### `sysbuild/mcuboot.conf` — MCUboot Bootloader Config

```ini
CONFIG_BOOT_SWAP_USING_SCRATCH=y
CONFIG_BOOT_MAX_IMG_SECTORS=16
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_GPIO=y
CONFIG_LOG=y
CONFIG_MCUBOOT_LOG_LEVEL_INF=y
CONFIG_MCUBOOT_SERIAL=n
CONFIG_MAIN_STACK_SIZE=16384
```

### CRITICAL: MCUboot stack size must be large enough

> **Bug lesson (see [bug_reports/006](../bug_reports/006_mcuboot_no_bootable_image.md), Issue 3):**
> RSA-2048 signature verification uses mbedTLS bignum math which is deeply recursive.
> The default 2–4 KB stack causes a stack overflow (MPU fault) on many MCUs.
> **Set `CONFIG_MAIN_STACK_SIZE=16384`** in `sysbuild/mcuboot.conf`.

---

## Step 3: Flash Partition Layout (Devicetree Overlay)

MCUboot requires three flash partitions: `boot_partition` (bootloader), `slot0_partition` (primary image), `slot1_partition` (upgrade image), and optionally `scratch_partition` (for swap).

Most Zephyr board DTS files already define partitions. You'll typically need to resize them in your board overlay.

### CRITICAL: Use `/delete-node/` before redefining

> **Bug lesson (see [bug_reports/005](../bug_reports/005_sysbuild_ota_build_failures.md)):**
> DTS labels are globally unique. If you add new partition nodes at different addresses
> but reuse labels like `slot0_partition`, the build fails with a label conflict.
> Always delete the base board's partition nodes first:

```dts
&flash0 {
    partitions {
        /* Delete base board's partition nodes that we're replacing */
        /delete-node/ partition@20000;
        /delete-node/ partition@60000;
        /delete-node/ partition@c0000;

        /* Redefine with your layout */
        slot0_partition: partition@20000 {
            label = "image-0";
            reg = <0x00020000 DT_SIZE_K(896)>;   /* adjust for your MCU */
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

---

## Step 4: MCUboot's Own DTS Overlay

### CRITICAL: MCUboot needs its own matching overlay

> **Bug lesson (see [bug_reports/006](../bug_reports/006_mcuboot_no_bootable_image.md), Issues 1 & 2):**
> In sysbuild, MCUboot and the application are **separate CMake projects**. The app's
> board overlay is NOT inherited by MCUboot. If MCUboot uses the base board's original
> (smaller) partition layout, it will look for image headers at the wrong addresses
> and fail with "Unable to find bootable image".

Create a matching overlay at `sysbuild/mcuboot/boards/<board>.overlay`:

```dts
/* Must match the app's partition layout exactly */
&flash0 {
    partitions {
        /delete-node/ partition@20000;
        /delete-node/ partition@60000;
        /delete-node/ partition@c0000;

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

/* MCUboot must be placed at the boot partition, not slot0 */
/ {
    chosen {
        zephyr,code-partition = &boot_partition;
    };
};
```

### Why override `zephyr,code-partition`?

Many board DTS files set `zephyr,code-partition = &slot0_partition` (for the app). MCUboot inherits this, causing it to be linked at slot0's address instead of the boot partition. The override ensures MCUboot is placed at `&boot_partition`.

### Required empty `prj.conf`

Zephyr requires `sysbuild/mcuboot/prj.conf` to exist when the directory exists. It can be empty — the actual MCUboot Kconfig lives in `sysbuild/mcuboot.conf`.

---

## Step 5: The Health-Based Confirmation System

### `include/ota.h` — Module Registry

```c
enum ota_module {
    OTA_MODULE_NET,
    OTA_MODULE_UART,
    OTA_MODULE_REST_API,
    OTA_MODULE_COUNT      /* sentinel — always last */
};

void ota_report_module_ready(enum ota_module mod);
void ota_thread_entry(void *p1, void *p2, void *p3);
```

### How It Works

- An `atomic_t` bitmask tracks which modules have reported ready.
- Each module calls `ota_report_module_ready(OTA_MODULE_XXX)` after successful init.
- The OTA thread polls the bitmask every 500 ms for up to `OTA_CONFIRM_TIMEOUT_S` (30s).
- When all bits are set → `boot_write_img_confirmed()` makes the image permanent.
- If timeout → the image stays unconfirmed → MCUboot reverts on next reset.

### Confirmation Flow

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
              │   Module A ready?  ☐ → ☑                 │
              │   Module B ready?  ☐ → ☑                 │
              │   Module C ready?  ☐ → ☑                 │
              └───────────────────┬───────────────────────┘
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
              All ready?                   Timeout?
                    │                           │
                    ▼                           ▼
     ┌──────────────────────┐    ┌──────────────────────────┐
     │ boot_write_img_      │    │ Image stays UNCONFIRMED  │
     │ confirmed()          │    │ MCUboot REVERTS on       │
     │ Image is PERMANENT   │    │ next reset               │
     └──────────────────────┘    └──────────────────────────┘
```

### Adding a Module to the Health Check

1. Add an enum entry in `ota.h` (before `OTA_MODULE_COUNT`):
   ```c
   OTA_MODULE_SENSOR,
   ```

2. Add the name string in `ota.c`:
   ```c
   [OTA_MODULE_SENSOR] = "sensor",
   ```

3. Call from your module after init succeeds:
   ```c
   #include "ota.h"
   ota_report_module_ready(OTA_MODULE_SENSOR);
   ```

That's it — the OTA thread automatically checks all `OTA_MODULE_COUNT` bits.

---

## Step 6: Building

### With MCUboot (OTA-enabled) — Sysbuild

```bash
west build -b <board> --sysbuild --pristine
```

This produces:
- `build/mcuboot/zephyr/zephyr.hex` — MCUboot bootloader
- `build/<app>/zephyr/zephyr.signed.bin` — Signed application image (for OTA upload)
- `build/<app>/zephyr/zephyr.signed.hex` — Signed application hex (for initial flash)

### Without MCUboot (development)

```bash
west build -b <board> --pristine
```

The OTA thread detects `boot_is_img_confirmed() == true` and skips the health check.

### Initial Flash

```bash
west flash
```

When built with `--sysbuild`, this flashes both MCUboot and the signed app.

### Python Dependencies

> **Bug lesson (see [bug_reports/005](../bug_reports/005_sysbuild_ota_build_failures.md), Issue 2):**
> MCUboot's build uses `imgtool.py` for key extraction, which requires the `click` module.
> Install before building:
> ```bash
> pip install click imgtool
> ```

---

## Step 7: OTA Update Procedure

### Prerequisites

#### 1. Install `smpmgr`

[`smpmgr`](https://github.com/intercreate/smpmgr) is a Python CLI tool for
communicating with MCUmgr's SMP protocol:

```bash
pip install smpmgr
```

Verify installation:

```bash
smpmgr --version
```

#### 2. Know the Device IP

The device must have a valid IP address (via DHCP or static config).

---

### Step 7.1 — Build After a Code Change

```bash
west build -b <board> --sysbuild
```

> Use `--pristine` (or `-p auto`) if you changed Kconfig / DTS. For
> source-only changes, an incremental build is fine.

The signed image is produced at:
```
build/<app>/zephyr/zephyr.signed.bin
```

### Step 7.2 — Verify Connectivity

Confirm `smpmgr` can reach the device on UDP port 1337:

```bash
smpmgr --ip <DEVICE_IP> os echo hello
```

Expected output:
```
⠋ Connecting to 10.100.110.91... OK
⠋ Waiting for response to EchoWrite... OK
EchoWriteResponse(
    ...
    r='hello'
)
```

If this fails, check:
- Device IP is correct
- Network interface is up and connected
- UDP port 1337 is not blocked by a firewall
- The device was built with MCUmgr enabled (`--sysbuild`)
- `CONFIG_ZCBOR=y` is in `prj.conf` (see [Bug #007](../bug_reports/007_mcumgr_silently_disabled.md))

### Step 7.3 — Check Current Image State

```bash
smpmgr --ip <DEVICE_IP> image state-read
```

Example output:
```
⠋ Connecting to 10.100.110.91... OK
⠋ Waiting for image states... OK
ImageState(
    slot=0,
    version='0.0.0',
    hash=HashBytes('DA4D6AE11B80F1BCCA766F7F404AA21069F6BEBD...'),
    bootable=True,
    pending=False,
    confirmed=True,
    active=True,
    permanent=False
)
splitStatus: 0
```

This shows the currently running (slot 0) image. Slot 1 should be empty or
contain an old image.

### Step 7.4 — Upload the New Image

```bash
smpmgr --ip <DEVICE_IP> image upload build/<app>/zephyr/zephyr.signed.bin
```

Example output:
```
⠋ Connecting to 10.100.110.91... OK
build/<app>/zephyr/zephyr.signed.bin ━━━━━━━━━━━━━━━━━━━━━━ 100.0% • 163.3/163.3 kB • 86.4 kB/s • 0:00:00
```

### Step 7.5 — Verify Upload and Note the Slot 1 Hash

```bash
smpmgr --ip <DEVICE_IP> image state-read
```

You should now see two images:
```
ImageState(
    slot=0,
    version='0.0.0',
    hash=HashBytes('DA4D6AE11B80F1BCCA766F7F404AA21069F6BEBD...'),
    bootable=True,
    confirmed=True,
    active=True,
    ...
)
ImageState(
    slot=1,
    version='0.0.0',
    hash=HashBytes('EBF59D5E825CA26DEBEF50C7EC72AFA5929900C4...'),
    bootable=True,
    confirmed=False,
    active=False,
    ...
)
```

### Step 7.6 — Mark for Test and Reset

```bash
smpmgr --ip <DEVICE_IP> image state-write <slot1-hash>
```

Replace `<slot1-hash>` with the full hash from Step 7.5 (e.g.,
`EBF59D5E825CA26DEBEF50C7EC72AFA5929900C42228813C3E1D7995B7EBC1EF`).

Then reset:

```bash
smpmgr --ip <DEVICE_IP> os reset
```

Expected output:
```
⠋ Connecting to 10.100.110.91... OK
⠋ Waiting for response to ResetWrite... OK
```

The device will:
1. Reset
2. MCUboot swaps slot 0 ↔ slot 1
3. New firmware boots in **test mode** (unconfirmed)
4. OTA health check runs (up to 30s)
5. If all modules report healthy → image auto-confirmed
6. If health check fails → image stays unconfirmed

> **One-command alternative:** `smpmgr --ip <DEVICE_IP> upgrade <path>`
> does upload + mark-for-test + reset in a single command.

### Step 7.7 — Verify the New Image

After the device boots (allow ~15 seconds for swap + network init):

```bash
smpmgr --ip <DEVICE_IP> image state-read
```

The new version should show `confirmed` in slot 0. The old image sits in
slot 1 as a known-good rollback.

---

### Rollback

#### Automatic Revert (Health Check Failed)

If the health check times out (a module failed to init), the image is
**not** confirmed. Simply reset the device:

- Power cycle, or
- Press the reset button

MCUboot will swap back to the previous confirmed image automatically.

#### Manual Revert

If the new firmware has issues but the health check passed (e.g., a logic
bug that doesn't affect module init):

```bash
smpmgr --ip <DEVICE_IP> image state-write <old-image-hash>
smpmgr --ip <DEVICE_IP> os reset
```

MCUboot will swap back to the old image.

### Erasing Slot 1

To clear a staged (but not yet swapped) image from slot 1:

```bash
smpmgr --ip <DEVICE_IP> image erase 1
```

### Quick Reference

| Action | Command |
|--------|---------|
| Echo test | `smpmgr --ip <IP> os echo hello` |
| List images | `smpmgr --ip <IP> image state-read` |
| Upload image | `smpmgr --ip <IP> image upload <signed.bin>` |
| Mark for test | `smpmgr --ip <IP> image state-write <hash>` |
| Confirm running image | `smpmgr --ip <IP> image state-write --confirm` |
| Reset device | `smpmgr --ip <IP> os reset` |
| Upload + test + reset | `smpmgr --ip <IP> upgrade <signed.bin>` |
| Erase slot 1 | `smpmgr --ip <IP> image erase 1` |

### Example: Full OTA Session

```bash
# 1. Build after a code change
west build -b <board> --sysbuild

# 2. Check current state
smpmgr --ip 192.168.0.84 image state-read

# 3. Upload new firmware
smpmgr --ip 192.168.0.84 image upload build/<app>/zephyr/zephyr.signed.bin

# 4. Verify upload — note the slot 1 hash
smpmgr --ip 192.168.0.84 image state-read

# 5. Mark for test (use hash from step 4)
smpmgr --ip 192.168.0.84 image state-write <slot1-hash>

# 6. Reset
smpmgr --ip 192.168.0.84 os reset

# 7. Wait ~15s, then verify
smpmgr --ip 192.168.0.84 image state-read
# Should show new version as confirmed in slot 0
```

---

## Signing Keys

### Development (Default)

Sysbuild uses MCUboot's built-in test key (`root-rsa-2048.pem`). Fine for development but **must not** be used in production.

### Production

1. Generate a key pair:
   ```bash
   imgtool keygen -k my-signing-key.pem -t rsa-2048
   ```

2. Set in `sysbuild/mcuboot.conf`:
   ```ini
   CONFIG_BOOT_SIGNATURE_KEY_FILE="path/to/my-signing-key.pem"
   ```

3. Rebuild with `--pristine` to embed the new public key.

---

## File Layout

```
project/
├── include/
│   └── ota.h                              # Health registry enum + API
├── src/
│   ├── main.c                             # Creates OTA thread
│   └── ota.c                              # Health check + confirmation logic
├── sysbuild.conf                          # SB_CONFIG_BOOTLOADER_MCUBOOT=y
├── sysbuild/
│   ├── mcuboot.conf                       # MCUboot Kconfig (swap, stack, flash)
│   └── mcuboot/
│       ├── prj.conf                       # Required (can be empty)
│       └── boards/
│           └── <board>.overlay            # MCUboot DTS: matching partitions
├── boards/
│   └── <board>.overlay                    # App DTS: flash partitions
├── prj.conf                               # App Kconfig (MCUmgr, flash, MCUboot)
└── CMakeLists.txt
```

---

## Known Issues and Bug Fixes

### 1. DTS Label Conflict on Build (Bug #005)

**Symptom:** Build fails with "Label 'slot1_partition' appears on ... and on ...".

**Fix:** Use `/delete-node/` to remove base board partition nodes before redefining them with new addresses.

See [bug_reports/005_sysbuild_ota_build_failures.md](../bug_reports/005_sysbuild_ota_build_failures.md).

### 2. Missing `imgtool` Python Dependencies (Bug #005)

**Symptom:** Build fails with `ModuleNotFoundError: No module named 'click'` during MCUboot key extraction.

**Fix:** `pip install click imgtool`.

See [bug_reports/005_sysbuild_ota_build_failures.md](../bug_reports/005_sysbuild_ota_build_failures.md).

### 3. MCUboot Can't Find Bootable Image (Bug #006)

**Symptom:** MCUboot logs `magic=unset`, `Unable to find bootable image`.

**Fix:** Three issues — (1) add MCUboot DTS overlay with matching partitions, (2) override `zephyr,code-partition` to `&boot_partition`, (3) increase `CONFIG_MAIN_STACK_SIZE=16384`.

See [bug_reports/006_mcuboot_no_bootable_image.md](../bug_reports/006_mcuboot_no_bootable_image.md).

### 4. MCUmgr Silently Disabled (Bug #007)

**Symptom:** `smpmgr` times out — UDP 1337 never opens. No build error.

**Fix:** Add `CONFIG_ZCBOR=y` before `CONFIG_MCUMGR=y` in `prj.conf`.

See [bug_reports/007_mcumgr_silently_disabled.md](../bug_reports/007_mcumgr_silently_disabled.md).

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|--------------|-----|
| `smpmgr` can't connect on port 1337 | MCUmgr silently disabled | Check `CONFIG_ZCBOR=y` in prj.conf (bug #007) |
| Build fails: DTS label conflict | Redefining partition without deleting old | Use `/delete-node/` (bug #005) |
| Build fails: `No module named 'click'` | Missing Python deps | `pip install click imgtool` (bug #005) |
| MCUboot: `Unable to find bootable image` | Partition mismatch | Add MCUboot DTS overlay (bug #006) |
| MCUboot hex overlaps with app | MCUboot linked to slot0 | Override `zephyr,code-partition` (bug #006) |
| MCUboot: MPU fault / stack overflow | RSA-2048 needs large stack | `CONFIG_MAIN_STACK_SIZE=16384` (bug #006) |
| Upload succeeds, no swap after reset | Image not marked as "test" | Run `image state-write <hash>` then `os reset` |
| Device reverts after every OTA | Health check timeout | Check logs for missing module; increase timeout |
| `boot_write_img_confirmed()` fails | Missing `CONFIG_BOOTLOADER_MCUBOOT=y` | Add to prj.conf, rebuild `--pristine` |
| Resolved `.config` missing MCUMGR | ZCBOR not enabled | Check `build/<app>/zephyr/.config` for MCUMGR entries |
