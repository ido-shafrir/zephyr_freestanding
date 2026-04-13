# OTA Update Procedure

Step-by-step instructions for performing a firmware update on the ICB device using `mcumgr` over UDP.

For architecture details, configuration, and troubleshooting, see the [OTA Guide](zephyr_ota_guide.md).

---

## Prerequisites

### 1. Install the `mcumgr` CLI

**Go-based CLI (recommended):**

```bash
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
```

Or download a pre-built binary from the [MCUmgr releases](https://github.com/apache/mynewt-mcumgr-cli/releases).

**Verify installation:**

```bash
mcumgr version
```

### 2. Know the Device IP

The device must have a valid IP address (via DHCP or static config). Check with:

- **UART command:** `ip_get`
- **HTTP:** `GET http://<device-ip>/api/ping`

### 3. Build a Signed Firmware Image

Build using sysbuild to produce a signed image:

```bash
cd ICB-FW
west build -b nucleo_h753zi --sysbuild --pristine
```

The signed image is at:
```
build/ICB-FW/zephyr/zephyr.signed.bin
```

---

## Procedure

### Step 1 — Verify Connectivity

Confirm `mcumgr` can reach the device on UDP port 1337:

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 echo hello
```

Expected output:
```
hello
```

If this fails, check:
- Device IP is correct
- W5500 is up and connected
- UDP port 1337 is not blocked by a firewall
- The device was built with MCUmgr enabled (`--sysbuild`)

### Step 2 — Check Current Image State

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image list
```

Example output:
```
Images:
 image=0 slot=0
    version: 0.1.0
    bootable: true
    flags: active confirmed
    hash: abcdef1234567890...
 Split status: N/A (0)
```

This shows the currently running (slot 0) image. Slot 1 should be empty or contain an old image.

### Step 3 — Upload the New Image

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image upload build/ICB-FW/zephyr/zephyr.signed.bin
```

This uploads the signed image to **slot 1**. Progress is shown:

```
 35.84 KiB / 198.34 KiB [===========>                         ] 18.07% 12.45 KiB/s
```

> **Note:** Upload speed depends on network conditions. Over Ethernet this typically takes 15–30 seconds for a ~200 KB image.

### Step 4 — Verify the Upload

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image list
```

You should now see two images:

```
Images:
 image=0 slot=0
    version: 0.1.0
    bootable: true
    flags: active confirmed
    hash: abcdef1234567890...
 image=0 slot=1
    version: 0.2.0
    bootable: true
    flags:
    hash: 9876543210fedcba...
 Split status: N/A (0)
```

### Step 5 — Mark the New Image for Testing

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image test <slot1-hash>
```

Replace `<slot1-hash>` with the hash from Step 4 (e.g., `9876543210fedcba...`).

This marks the slot 1 image as "pending test". MCUboot will swap it into slot 0 on the next reset.

### Step 6 — Reset the Device

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 reset
```

The device will:
1. Reset
2. MCUboot swaps slot 0 ↔ slot 1
3. New firmware boots in **test mode** (unconfirmed)
4. OTA health check runs (up to 30s)
5. If all modules report healthy → image auto-confirmed
6. If health check fails → image stays unconfirmed

### Step 7 — Verify the New Image

After the device boots (allow ~15 seconds for swap + DHCP):

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image list
```

Expected output (success):
```
Images:
 image=0 slot=0
    version: 0.2.0
    bootable: true
    flags: active confirmed    ← auto-confirmed by health check
    hash: 9876543210fedcba...
 image=0 slot=1
    version: 0.1.0
    bootable: true
    flags:
    hash: abcdef1234567890...
 Split status: N/A (0)
```

The new image is now running and confirmed. The old image sits in slot 1 as a known-good rollback.

---

## Rollback (Manual Revert)

If the new firmware has issues but the health check passed (e.g., a logic bug that doesn't affect module init):

### Revert to the Previous Image

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image confirm <old-image-hash>
```

Then reset:

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 reset
```

MCUboot will swap back to the old image.

### Automatic Revert (Health Check Failed)

If the health check times out (a module failed to init), the image is **not** confirmed. Simply reset the device:

- Power cycle, or
- Press the reset button

MCUboot will swap back to the previous confirmed image automatically.

---

## Erasing Slot 1

To clear a staged (but not yet swapped) image from slot 1:

```bash
mcumgr --conntype udp --connstring=[<DEVICE_IP>]:1337 image erase
```

---

## Quick Reference

| Action | Command |
|--------|---------|
| Test connectivity | `mcumgr --conntype udp --connstring=[<IP>]:1337 echo hello` |
| List images | `mcumgr --conntype udp --connstring=[<IP>]:1337 image list` |
| Upload image | `mcumgr --conntype udp --connstring=[<IP>]:1337 image upload <path-to-signed-bin>` |
| Mark for test | `mcumgr --conntype udp --connstring=[<IP>]:1337 image test <hash>` |
| Confirm image | `mcumgr --conntype udp --connstring=[<IP>]:1337 image confirm <hash>` |
| Reset device | `mcumgr --conntype udp --connstring=[<IP>]:1337 reset` |
| Erase slot 1 | `mcumgr --conntype udp --connstring=[<IP>]:1337 image erase` |

---

## Example: Full OTA Session

```bash
# 1. Build
west build -b nucleo_h753zi --sysbuild --pristine

# 2. Check current state
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 image list

# 3. Upload new firmware
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 image upload build/ICB-FW/zephyr/zephyr.signed.bin

# 4. Verify upload
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 image list

# 5. Mark for test (use hash from step 4)
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 image test 9876543210fedcba...

# 6. Reset
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 reset

# 7. Wait ~15s, then verify
mcumgr --conntype udp --connstring=[192.168.0.84]:1337 image list
# Should show new version as "active confirmed"
```
