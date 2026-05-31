# EVB Quick-Start Guide

Hardware wiring, build, and flash instructions for the firmware on a
**NUCLEO-H753ZI** evaluation board with a **W5500 Ethernet module**.

---

## 1. W5500 Wiring

Connect the W5500 module to the Nucleo board using **6 signal wires + power**:

| W5500 Pin | Nucleo Pin | STM32 Pin | Function       |
|-----------|------------|-----------|----------------|
| SCK       | CN7-10     | **PA5**   | SPI1 Clock     |
| MISO      | CN7-12     | **PA6**   | SPI1 MISO      |
| MOSI      | CN7-14     | **PB5**   | SPI1 MOSI      |
| CS (SCS)  | CN7-16     | **PD14**  | Chip Select     |
| INT       | CN7-18     | **PD15**  | Interrupt       |
| RST       | *(not wired — pulled high on module)* | — | Reset (optional) |
| 3.3V      | CN8-7 or CN7-16 (3V3) | —  | Power 3.3 V    |
| GND       | CN8-5 or any GND      | —  | Ground          |

> **Tip:** Use short jumper wires (< 15 cm). The SPI clock is 10 MHz; long
> wires cause signal integrity issues.

### Pin Location on Nucleo CN7 Header

```
        CN7 (left header, top view, USB facing up)
        ┌─────────┐
   1  ──┤ PC6     ├── 2
   3  ──┤ PB8     ├── 4
   ...  │         │  ...
   9  ──┤ PA4     ├── 10  ◄── SCK  (PA5)
  11  ──┤ PA7     ├── 12  ◄── MISO (PA6)
  13  ──┤ PA1     ├── 14  ◄── MOSI (PB5)
  15  ──┤ PA2     ├── 16  ◄── CS   (PD14)
  17  ──┤ PA3     ├── 18  ◄── INT  (PD15)
        └─────────┘
```

> Check the [Nucleo-H753ZI pinout diagram](https://os.mbed.com/platforms/ST-Nucleo-H753ZI/)
> for exact header positions. The pins above follow the Morpho connector layout.

---

## 2. Prerequisites

| Tool | Install |
|------|---------|
| **Zephyr SDK** | [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/) |
| **west** | `pip install west` |
| **STM32CubeProgrammer** | [Download](https://www.st.com/en/development-tools/stm32cubeprog.html) — required by `west flash` for ST-Link |
| **smpmgr** (OTA only) | `pip install smpmgr` |

Make sure the Zephyr workspace is initialised and the virtual-env is activated:

```powershell
cd C:\Users\<you>\Documents\zephyr
.\.venv\Scripts\Activate.ps1
```

---

## 3. Build

```powershell
cd zephyr_freestanding
west build -b nucleo_h753zi -p always --sysbuild
```

This co-builds **MCUboot** (bootloader) + the application.

Output binaries land in:

| File | Purpose |
|------|---------|
| `build/app/zephyr/zephyr.signed.bin` | Signed app image (for OTA) |
| `build/app/zephyr/zephyr.bin` | Raw app binary |
| `build/mcuboot/zephyr/zephyr.bin` | MCUboot bootloader |

---

## 4. Flash (First Time — via ST-Link)

The Nucleo board has a built-in ST-Link debugger. Just plug in USB and run:

```powershell
west flash
```

This flashes both MCUboot and the application to the board.

> **Note:** `west flash` requires **STM32CubeProgrammer** (see prerequisites).
> Make sure it is installed and its CLI (`STM32_Programmer_CLI`) is on your PATH.

---

## 5. Flash (Subsequent Updates — OTA via Ethernet)

Once the first flash is done, future updates can be sent over the network
using `smpmgr` over UDP (port 1337).

See **[ota_update_procedure.md](ota_update_procedure.md)** for the full
step-by-step procedure including image upload, test/confirm flow,
rollback, and troubleshooting.

---

## 6. Verify

After flashing or OTA, confirm the board is running:

```
GET http://<DEVICE_IP>/api/ping
→ {"result":"pong"}

GET http://<DEVICE_IP>/api/help
→ (lists all available endpoints)
```

Serial console (USART3, 115200 8N1) will show boot logs and accepts
commands like `ip_get`, `ip_set`, `bars_get`, etc.

---

## 7. Troubleshooting

| Symptom | Fix |
|---------|-----|
| No Ethernet link | Check wiring, especially CS (PD14) and INT (PD15). Try shorter wires. |
| DHCP timeout | Confirm the network has a DHCP server. Or set a static IP via UART: `ip_set 10.0.0.50 255.255.255.0 10.0.0.1` |
| `west flash` fails | Install STM32CubeProgrammer. Check ST-Link USB connection. |
| OTA upload hangs | Verify device IP is reachable (`ping <IP>`). Check UDP port 1337 isn't blocked. |
| Build error | Ensure venv is activated and `west update` has been run at least once. |
