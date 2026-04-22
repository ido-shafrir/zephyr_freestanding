# Persistent Configuration Store Guide

The config store provides persistent key-value storage that survives reboots
and firmware updates. It is built on Zephyr's **settings** subsystem with a
**ZMS** (Zephyr Memory Storage) flash backend.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Application code                                            │
│    config_store_get_*() / config_store_set_*()               │
├─────────────────────────────────────────────────────────────┤
│  config_store.c        (mutex-protected vars)                │
│    SETTINGS_STATIC_HANDLER_DEFINE per subtree                │
├─────────────────────────────────────────────────────────────┤
│  Zephyr settings API   (settings_load/save_one)              │
├─────────────────────────────────────────────────────────────┤
│  ZMS backend           (CONFIG_SETTINGS_ZMS)                 │
├─────────────────────────────────────────────────────────────┤
│  Flash driver          (storage_partition)                    │
│  0x1C0000 – 0x1FFFFF   256 KB, 2 × 128 KB                   │
└─────────────────────────────────────────────────────────────┘
```

### Why settings survive firmware updates

The storage partition (`0x1C0000`–`0x1FFFFF`) sits after the scratch partition
at the end of flash. MCUboot only swaps data within slot0 and slot1 (using
the scratch area). The storage partition is never touched during an OTA
swap, so all persisted settings are preserved.

See [memory_layout.md](memory_layout.md) for the complete flash map.

---

## Boot Sequence

The config store runs in its own thread (`CONFIG_STORE_PRIORITY = 6`):

```
config_store_thread_entry()
  │
  ├─ settings_subsys_init()       Mount ZMS backend
  ├─ settings_load()              Read all keys from flash → h_set callbacks
  ├─ config_store_dump()          Log all current values
  └─ ota_report_module_ready()    Signal OTA health check
```

On first boot (empty storage), all configs use their compiled-in defaults.

---

## Current Configuration Keys

### `heartbeat/` subtree

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `heartbeat/enabled` | `bool` | `false` | Enable/disable the heartbeat webhook |
| `heartbeat/url` | `char[256]` | `""` (empty) | Webhook URL to POST heartbeats to |
| `heartbeat/interval` | `uint32_t` | `60` | Interval between heartbeats in seconds |

### `system/` subtree

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system/name` | `char[64]` | `"My-Device"` | Human-readable device name |

---

## API Reference

### Getters

```c
bool     config_store_get_heartbeat_enabled(void);
int      config_store_get_heartbeat_url(char *buf, size_t buf_len);
uint32_t config_store_get_heartbeat_interval(void);
int      config_store_get_system_name(char *buf, size_t buf_len);
```

String getters return the string length on success or a negative errno.

### Setters

```c
int config_store_set_heartbeat_enabled(bool enabled);
int config_store_set_heartbeat_url(const char *url);
int config_store_set_heartbeat_interval(uint32_t interval_s);
int config_store_set_system_name(const char *name);
```

All setters persist the value to flash immediately via `settings_save_one()`.
Returns 0 on success, negative errno on failure.

### Dump

```c
void config_store_dump(void);
```

Logs all current config values at `LOG_INF` level. Called automatically
after init; can also be called from any thread at any time.

---

## Adding a New Configuration Key

### 1. Add the variable with a default in `config_store.c`

```c
static uint32_t mymodule_timeout = 30;
```

### 2. Create a settings handler (or add to an existing subtree)

For a new subtree:

```c
static int mymodule_set(const char *name, size_t len,
                        settings_read_cb read_cb, void *cb_arg)
{
    const char *next;
    if (settings_name_steq(name, "timeout", &next) && !next) {
        if (len != sizeof(mymodule_timeout)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &mymodule_timeout, sizeof(mymodule_timeout));
    }
    return -ENOENT;
}

static int mymodule_export(int (*cb)(const char *name, const void *val,
                                     size_t val_len))
{
    cb("mymodule/timeout", &mymodule_timeout, sizeof(mymodule_timeout));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mymodule, "mymodule", NULL, mymodule_set,
                               NULL, mymodule_export);
```

### 3. Add getter/setter functions

```c
uint32_t config_store_get_mymodule_timeout(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    uint32_t val = mymodule_timeout;
    k_mutex_unlock(&cfg_lock);
    return val;
}

int config_store_set_mymodule_timeout(uint32_t timeout_s)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    mymodule_timeout = timeout_s;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("mymodule/timeout", &timeout_s, sizeof(timeout_s));
}
```

### 4. Add a line to `config_store_dump()`

```c
LOG_INF("  mymodule/timeout   = %u s", mymodule_timeout);
```

### 5. Add prototypes to `config_store.h`

```c
uint32_t config_store_get_mymodule_timeout(void);
int      config_store_set_mymodule_timeout(uint32_t timeout_s);
```

That's it. The new key is automatically loaded from flash on boot and
persisted when the setter is called.

---

## Kconfig Dependencies

These must be in `prj.conf`:

```ini
CONFIG_FLASH=y           # Flash driver (already present for MCUboot)
CONFIG_FLASH_MAP=y       # Named flash partitions (already present)
CONFIG_ZMS=y             # Zephyr Memory Storage driver
CONFIG_SETTINGS=y        # Settings subsystem
CONFIG_SETTINGS_ZMS=y    # ZMS as the settings backend
```

---

## DTS Requirements

The app's board overlay must define a `storage_partition` and point the
settings subsystem to it:

```dts
&flash0 {
    partitions {
        storage_partition: partition@1c0000 {
            label = "storage";
            reg = <0x001C0000 DT_SIZE_K(256)>;
        };
    };
};

/ {
    chosen {
        zephyr,settings-partition = &storage_partition;
    };
};
```

---

## Thread Safety

All config variables are protected by a single mutex (`cfg_lock`). The
Zephyr settings API is also internally mutex-protected. This means:

- Getters and setters are safe to call from any thread.
- `settings_save_one()` (called by setters) acquires the settings lock.
- Do not call getters/setters from ISR context (they use mutexes).

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `settings_subsys_init failed: -33` | Flash sector size > 64 KB with NVS backend | Use ZMS instead of NVS (`CONFIG_ZMS + CONFIG_SETTINGS_ZMS`). See [bug #008](../bug_reports/008_settings_nvs_uint16_limit.md) |
| `settings_subsys_init failed: -2` | No storage partition in DTS | Add `storage_partition` to the board overlay |
| Values reset after reboot | Storage partition overlaps with MCUboot slots | Check `memory_layout.md` — storage must be outside slot0/slot1 |
| Values reset after OTA | Storage partition inside swap area | Move storage to address range not covered by slot0, slot1, or scratch |
| `settings_load failed` | ZMS storage corrupted | Erase the storage partition: `west flash --erase` |
| Config store never reports ready to OTA | Thread crash or settings init failure | Check serial log for `config_store` errors |

---

## File Reference

| File | Purpose |
|------|---------|
| [`include/config_store.h`](../include/config_store.h) | Public API: thread config, getters/setters, dump |
| [`src/config_store.c`](../src/config_store.c) | Implementation: settings handlers, thread, mutex |
| [`boards/nucleo_h753zi.overlay`](../boards/nucleo_h753zi.overlay) | Storage partition DTS + `chosen` node |
| [`prj.conf`](../prj.conf) | ZMS/settings Kconfig options |
| [`docs/memory_layout.md`](memory_layout.md) | Complete flash memory map |
