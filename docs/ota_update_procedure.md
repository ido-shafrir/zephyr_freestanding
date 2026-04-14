# OTA Update Procedure

Step-by-step instructions for performing a firmware update on the ICB device
using [`smpmgr`](https://github.com/intercreate/smpmgr) over UDP.

For architecture details, configuration, and troubleshooting, see the
[OTA Guide](zephyr_ota_guide.md).

---

## Prerequisites

### 1. Install `smpmgr`

```bash
pip install smpmgr
```

Verify installation:

```bash
smpmgr --version
```

### 2. Know the Device IP

The device must have a valid IP address (via DHCP or static config). Check with:

- **UART command:** `ip_get`
- **HTTP:** `GET http://<device-ip>/api/ping`

---

## Procedure

### Step 1 — Build After a Code Change

```bash
west build -b nucleo_h753zi --sysbuild
```

> Use `--pristine` (or `-p auto`) if you changed Kconfig / DTS. For
> source-only changes, an incremental build is fine.

The signed image is produced at:
```
build/ICB-FW/zephyr/zephyr.signed.bin
```

### Step 2 — Verify Connectivity

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
- W5500 is up and connected
- UDP port 1337 is not blocked by a firewall
- The device was built with MCUmgr enabled (`--sysbuild`)
- `CONFIG_ZCBOR=y` is in `prj.conf` (see [Bug #007](../bug_reports/007_mcumgr_silently_disabled.md))

### Step 3 — Check Current Image State

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

### Step 4 — Upload the New Image

```bash
smpmgr --ip <DEVICE_IP> image upload build/ICB-FW/zephyr/zephyr.signed.bin
```

Example output:
```
⠋ Connecting to 10.100.110.91... OK
build\ICB-FW\zephyr\zephyr.signed.bin ━━━━━━━━━━━━━━━━━━━━━━ 100.0% • 163.3/163.3 kB • 86.4 kB/s • 0:00:00
```

### Step 5 — Verify Upload and Note the Slot 1 Hash

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

### Step 6 — Mark for Test and Reset

```bash
smpmgr --ip <DEVICE_IP> image state-write <slot1-hash>
```

Replace `<slot1-hash>` with the full hash from Step 5 (e.g.,
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

### Step 7 — Verify the New Image

After the device boots (allow ~15 seconds for swap + DHCP):

```bash
smpmgr --ip <DEVICE_IP> image state-read
```

The new version should show `confirmed` in slot 0. The old image sits in
slot 1 as a known-good rollback.

---

## Rollback (Manual Revert)

If the new firmware has issues but the health check passed (e.g., a logic
bug that doesn't affect module init):

### Revert to the Previous Image

```bash
smpmgr --ip <DEVICE_IP> image state-write <old-image-hash>
smpmgr --ip <DEVICE_IP> os reset
```

MCUboot will swap back to the old image.

### Automatic Revert (Health Check Failed)

If the health check times out (a module failed to init), the image is
**not** confirmed. Simply reset the device:

- Power cycle, or
- Press the reset button

MCUboot will swap back to the previous confirmed image automatically.

---

## Erasing Slot 1

To clear a staged (but not yet swapped) image from slot 1:

```bash
smpmgr --ip <DEVICE_IP> image erase 1
```

---

## Quick Reference

| Action | Command |
|--------|---------|
| Echo test | `smpmgr --ip <IP> os echo hello` |
| List images | `smpmgr --ip <IP> image state-read` |
| Upload image | `smpmgr --ip <IP> image upload <path-to-signed-bin>` |
| Mark for test | `smpmgr --ip <IP> image state-write <hash>` |
| Confirm running image | `smpmgr --ip <IP> image state-write --confirm` |
| Reset device | `smpmgr --ip <IP> os reset` |
| Upload + test + reset | `smpmgr --ip <IP> upgrade <path-to-signed-bin>` |
| Erase slot 1 | `smpmgr --ip <IP> image erase 1` |

---

## Example: Full OTA Session

```bash
# 1. Build after a code change
west build -b nucleo_h753zi --sysbuild

# 2. Check current state
smpmgr --ip 192.168.0.84 image state-read

# 3. Upload new firmware
smpmgr --ip 192.168.0.84 image upload build/ICB-FW/zephyr/zephyr.signed.bin

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

Or, as a single command (steps 3–6 combined):

```bash
west build -b nucleo_h753zi --sysbuild
smpmgr --ip 192.168.0.84 upgrade build/ICB-FW/zephyr/zephyr.signed.bin
```

---

## Serial Monitor Output After OTA Swap

After `os reset`, the device reboots through MCUboot and runs the health
check. Key OTA lines are marked with `►`:

```
*** Booting Zephyr OS build v4.4.0-rc3 ***
[00:00:00.005,000] <inf> main: Starting ICB Firmware...
[00:00:00.005,000] <inf> main: All threads created. Entering main loop.
[00:00:00.005,000] <inf> smp_udp: Started (IPv4)                              ► MCUmgr UDP transport listening on port 1337
[00:00:00.005,000] <inf> command_uart: USB/UART command interface ready
[00:00:00.008,000] <inf> ota: Module ready: command_uart (1/3)                ► Health check: UART module OK
[00:00:00.008,000] <inf> w5500_net: ========================================
[00:00:00.008,000] <inf> w5500_net:   W5500 Network Bring-Up Test
[00:00:00.008,000] <inf> w5500_net: ========================================
[00:00:00.008,000] <inf> ota: Module ready: rest_api (2/3)                    ► Health check: REST API module OK
[00:00:00.008,000] <inf> rest_api: HTTP server started on port 80
[00:00:00.008,000] <inf> ota: OTA thread started                              ► Health check thread begins
[00:00:00.008,000] <inf> ota: Image is NOT confirmed — running health check (timeout: 30s)
[00:00:02.008,000] <inf> w5500_net: Network interface found: 0x240012e0
[00:00:02.008,000] <inf> w5500_net: MAC: 02:ab:cd:ef:01:23
[00:00:02.008,000] <inf> w5500_net: Interface is UP
[00:00:02.008,000] <inf> w5500_net:   Starting DHCP client...
[00:00:02.008,000] <inf> w5500_net: DHCP client started
[00:00:02.008,000] <inf> ota: Module ready: w5500_net (3/3)                   ► Health check: NET module OK (all 3/3)
[00:00:02.008,000] <inf> w5500_net: Net thread completed initialization. Exiting thread.
[00:00:02.009,000] <inf> ota: ========================================
[00:00:02.009,000] <inf> ota:   All modules healthy — image CONFIRMED        ► Image permanently confirmed
[00:00:02.009,000] <inf> ota:   Elapsed: 2.0s
[00:00:02.009,000] <inf> ota: ========================================
[00:00:11.278,000] <inf> net_dhcpv4: Received: 10.100.110.91
[00:00:11.278,000] <inf> w5500_net: ========================================
[00:00:11.278,000] <inf> w5500_net:   DHCP lease acquired
[00:00:11.278,000] <inf> w5500_net:   Address: 10.100.110.91
[00:00:11.278,000] <inf> w5500_net:   Netmask: 255.255.255.0
[00:00:11.278,000] <inf> w5500_net:   Gateway: 10.100.110.1
[00:00:11.278,000] <inf> w5500_net: ========================================
```

The health check completed in **2.0 seconds** — all 3 modules (UART, REST API,
NET) reported ready before the 30-second timeout. The image was permanently
confirmed, so a future reset will **not** revert to the old firmware.
