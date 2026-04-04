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
- Python 3.12+
- CMake 3.20+
- [west](https://docs.zephyrproject.org/latest/develop/west/index.html) meta-tool (`pip install west`)

## Getting Started

### 1. Initialize the west workspace,  

From the directory **containing** this project folder, run:
*Note:* this can take a while first time 

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


```

## Building

Build the application using west, specifying your target board:
build from the zephyr_freestanding project so the .env file auto loads 

```bash
cd zephyr_freestanding
west build -b <board> 
```

For example, to build for the Nucleo H753ZI:

```bash
cd zephyr_freestanding
west build -b nucleo_h753zi 
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

## Recommended Workspace Layout

Since the Zephyr workspace is large (kernel, HALs, modules, tools), you should **avoid duplicating it per project**. Instead, keep a single Zephyr workspace and place all your freestanding applications inside it:

```
zephyr-workspace/                   # One shared workspace root
├── .west/                          # West workspace metadata
├── zephyr/                         # Zephyr kernel (fetched by west)
├── modules/                        # Zephyr modules (HALs, libs, etc.)
│   ├── hal/
│   ├── lib/
│   └── ...
├── tools/                          # Zephyr tools
│
├── app_blinky/                     # Freestanding project A
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── west.yml
│   └── src/
│       └── main.c
│
├── app_sensor/                     # Freestanding project B
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── west.yml
│   └── src/
│       └── main.c
│
└── app_motor_ctrl/                 # Freestanding project C
    ├── CMakeLists.txt
    ├── prj.conf
    ├── west.yml
    └── src/
        └── main.c
```

### Switching Between Projects

Since `west init -l` sets the workspace manifest to one project at a time, you need to re-initialize when switching projects:

```bash
# Start working on app_blinky
west init -l app_blinky
west update

# Later, switch to app_sensor
west init -l app_sensor
west update
```

> **Note:** If all projects pin the same Zephyr version and modules, `west update` after switching will be fast since the sources are already present. If projects use different versions, west will checkout the appropriate revisions.

### When to Use This Layout

| Scenario | Recommendation |
|---|---|
| All projects use the **same Zephyr version** and modules | Single workspace + multiple freestanding apps. Switching is fast. |
| Projects need **different Zephyr versions** or modules | Single workspace still works, but `west update` may re-checkout sources. Consider separate workspaces if switching is frequent. |
| Only **one project** | Single workspace with one freestanding app (this repo's default). |
