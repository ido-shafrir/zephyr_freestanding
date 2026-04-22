# Bug Report #008: Settings NVS Backend Fails on STM32H753 — UINT16 Sector Size Limit

**Date:** 2026-04-15  
**Severity:** Minor  
**Status:** Resolved (workaround)  
**Component:** Zephyr settings subsystem / NVS backend  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

`settings_subsys_init()` returned `-33` (`EDOM`) when using the NVS backend
(`CONFIG_SETTINGS_NVS=y`) on the STM32H753ZI. The NVS settings backend has
a hard-coded `UINT16_MAX` check on the sector size, but the STM32H753 has
128 KB flash sectors (131072 bytes), which exceeds `UINT16_MAX` (65535).

---

## Symptoms

```
[00:00:00.008,000] <inf> config_store: Config store initializing...
[00:00:00.008,000] <err> config_store: settings_subsys_init failed: -33
```

The config store thread failed to initialize. No settings were loaded,
and the OTA health check was missing one module.

---

## Root Cause

In `zephyr/subsys/settings/src/settings_nvs.c` (line ~409):

```c
nvs_sector_size = CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT *
                  hw_flash_sector.fs_size;

if (nvs_sector_size > UINT16_MAX) {
    return -EDOM;
}
```

On STM32H753ZI:
- `hw_flash_sector.fs_size = 131072` (128 KB)
- `CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT = 1` (default)
- `nvs_sector_size = 131072 > 65535` → returns `-EDOM`

This check is a legacy artifact. The underlying NVS struct (`struct nvs_fs`
in `zephyr/kvss/nvs.h`) now uses `uint32_t sector_size`, so the hardware
can handle it. But the settings NVS backend still has the old `UINT16_MAX`
guard.

The `CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT` Kconfig only multiplies — there
is no way to divide the sector size to fit under the limit.

---

## Fix: Switch to ZMS Backend

ZMS (Zephyr Memory Storage) is the newer replacement for NVS. Its settings
backend (`settings_zms.c`) checks `UINT32_MAX` instead of `UINT16_MAX`:

```c
if (zms_sector_size > UINT32_MAX) {
    return -EDOM;
}
```

128 KB sectors work fine with ZMS.

### Kconfig Change

```diff
-CONFIG_NVS=y
-CONFIG_SETTINGS_NVS=y
+CONFIG_ZMS=y
+CONFIG_SETTINGS_ZMS=y
```

`CONFIG_SETTINGS=y` stays the same. The settings API (`settings_load`,
`settings_save_one`, etc.) is backend-agnostic — no application code changes
are needed.

---

## Affected Boards

Any board with flash sectors > 64 KB will hit this when using
`CONFIG_SETTINGS_NVS=y`. Common affected MCUs:

- STM32H7 series (128 KB sectors)
- STM32F4 series with large sectors (up to 128 KB)
- Any MCU with flash pages > 64 KB

---

## Lessons Learned

1. **NVS has a 64 KB sector size limit** due to a legacy `UINT16_MAX`
   check in the settings backend. Use ZMS instead on MCUs with large
   flash sectors.

2. **ZMS is the recommended replacement for NVS** in Zephyr 4.x. The
   settings API is backend-agnostic, so switching only requires a
   Kconfig change.

3. **`-EDOM` (error 33) from settings init means "sector size too large".**
   Not an obvious error message — check the flash sector geometry.
