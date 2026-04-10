# ICB-FW — Ionizer Control Box Firmware

Firmware for the **Ionizer Control Box (ICB)** project, built on the [Zephyr RTOS](https://zephyrproject.org/).

This is a **freestanding** Zephyr application — it lives in its own repository and pulls in Zephyr as an external dependency via a west manifest (`west.yml`), making the build fully self-contained and reproducible.

## Prerequisites

- [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) installed
- Python 3.12+
- CMake 3.20+
- [west](https://docs.zephyrproject.org/latest/develop/west/index.html) meta-tool (`pip install west`)

## Getting Started

### 1. Initialize the west workspace  

From the directory **containing** this project folder, run:
*Note:* this can take a while the first time.

```bash
west init -l ICB-FW
```

This tells west to use the `west.yml` manifest in this project as the workspace manifest.

### 2. Update (fetch Zephyr and its dependencies)

```bash
west update
```

This clones Zephyr (pinned to **v4.0.0**) and all required modules into the workspace.

### 3. Export Zephyr CMake package

```bash
west zephyr-export
```

This registers the Zephyr CMake package so `find_package(Zephyr)` in `CMakeLists.txt` can locate it.


## Environment Configuration

The `.env` file contains key environment variables used by west, CMake, and the Zephyr build system. Edit it to match your local setup before building.

| Variable | Description | Default |
|---|---|---|
| `ZEPHYR_BASE` | Path to the Zephyr kernel source tree | `C:\STM32h753\zephyrproject` |
| `BOARD` | Default target board | `nucleo_h753zi` |
| `ZEPHYR_TOOLCHAIN_VARIANT` | Toolchain to use (`zephyr`, `gnuarmemb`, `llvm`, etc.) | `zephyr` |
| `ZEPHYR_SDK_INSTALL_DIR` | Path to the Zephyr SDK installation | *(update to your path)* |
| `BUILD_DIR` | Build output directory | `build` |
| `CMAKE_GENERATOR` | CMake generator | `Ninja` |
| `VIRTUAL_ENV` | Path to the Python virtual environment (ensures west and CMake use Python 3.12+) | `../.venv` |


```

## Building

Build the application using west, specifying your target board:

```bash
cd ICB-FW
west build -b <board> 
```

For example, to build for the Nucleo H753ZI:

```bash
cd ICB-FW
west build -b nucleo_h753zi 
```

To do a pristine rebuild:

```bash
west build -b nucleo_h753zi ICB-FW --pristine
```

## Flashing

```bash
west flash
```

## Project Structure

```
ICB-FW/
├── CMakeLists.txt   # Build system entry point
├── prj.conf         # Kconfig project configuration
├── west.yml         # West manifest (pins Zephyr version)
├── README.md
├── Drivers/         # Hardware drivers
├── include/         # Header files
├── docs/            # Project documentation & SOW
└── src/
    └── main.c       # Application entry point
```

### Switching Between Projects

If you have multiple freestanding projects in the same workspace, use `west init -l` to switch between them:

```bash
west init -l ICB-FW
west update
```
