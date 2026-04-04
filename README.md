# Zephyr Freestanding Application Skeleton

This project is a **freestanding** Zephyr application skeleton, intended as a starting point for building Zephyr-based firmware outside of the Zephyr repository tree.

## Zephyr Application Types

Zephyr supports three application types:

| Type | Description |
|---|---|
| **Repository** | The application lives inside the Zephyr repository tree (e.g. under `zephyr/samples/`). Tightly coupled to a specific Zephyr version. |
| **Workspace** | The application sits alongside the Zephyr source in a shared west workspace, managed by a top-level manifest (e.g. `zephyrproject/my-app/`). |
| **Freestanding** | The application lives in its own independent directory and pulls in Zephyr as an external dependency via its own `west.yaml` manifest. This is the recommended layout for production projects. |

This project uses the **freestanding** layout. It contains its own west manifest (`west.yaml`) that pins a specific Zephyr release, making the build fully self-contained and reproducible.

## Prerequisites

- [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) installed
- Python 3.10+
- CMake 3.20+
- [west](https://docs.zephyrproject.org/latest/develop/west/index.html) meta-tool (`pip install west`)

## Getting Started

### 1. Initialize the west workspace

From the directory **containing** this project folder, run:

```bash
west init -l zephyr_freestanding
```

This tells west to use the `west.yaml` manifest in this project as the workspace manifest.

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

Load the environment before building:

**Linux / macOS:**
```bash
set -a && source .env && set +a
```

**PowerShell:**
```powershell
Get-Content .env | ForEach-Object {
    if ($_ -match '^\s*([^#][^=]+)=(.*)') {
        [System.Environment]::SetEnvironmentVariable($Matches[1].Trim(), $Matches[2].Trim(), 'Process')
    }
}
```

## Building

Build the application using west, specifying your target board:

```bash
west build -b <board> zephyr_freestanding
```

For example, to build for the Nucleo H753ZI:

```bash
west build -b nucleo_h753zi zephyr_freestanding
```

To do a pristine rebuild:

```bash
west build -b nucleo_h753zi zephyr_freestanding --pristine
```

## Flashing

```bash
west flash
```

## Project Structure

```
zephyr_freestanding/
├── .env             # Environment variables (toolchain, board, paths)
├── CMakeLists.txt   # Build system entry point
├── prj.conf         # Kconfig project configuration
├── west.yaml        # West manifest (pins Zephyr v4.0.0)
├── README.md
└── src/
    └── main.c       # Application entry point
```
