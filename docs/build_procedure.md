# Build Procedure

## 1. Activate the virtual environment

```powershell
& c:\Users\idosh\Documents\zephyr\.venv\Scripts\Activate.ps1
```

Run this in every new terminal session before building.

## 2. Build

```powershell
west build -b nucleo_h753zi -p always --sysbuild
```

## 3. When to use `-p always`

Use `-p always` (pristine / full clean rebuild) when:

- You added or removed source files in `CMakeLists.txt`
- You changed `prj.conf`, `sysbuild.conf`, or any Kconfig file
- You modified the `VERSION` file
- You changed a devicetree overlay (`.overlay` / `.dts`)
- The build directory seems stale (old artifacts despite new source)

For routine source-only edits (`.c` / `.h` changes), you can skip `-p` for faster incremental builds:

```powershell
west build -b nucleo_h753zi --sysbuild
```

## 4. Flash

```powershell
west flash
```

## 5. Version bumping

Edit `VERSION` in the project root before building a new OTA image:

```
VERSION_MAJOR = 0
VERSION_MINOR = 1
VERSION_PATCHLEVEL = 0
VERSION_TWEAK = 0
```

The version is embedded in the MCUboot image header and reported by mcumgr image state.
