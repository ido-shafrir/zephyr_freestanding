# Feature Selection Guide

The `zephyr_freestanding` template ships a curated menu of optional
features. Each feature lives in `src/` and `include/` but is **not**
compiled by default — the base build produces only the blinky demo.

Use this guide to:

1. Pick the features your project needs from the table below.
2. Follow the per-feature recipe to enable it.
3. Check the dependency graph before turning on a feature that depends
   on others.

---

## Mental Model: The Dormant Pattern

The template uses a **dormant pattern**:

- All optional source files live in `src/` and `include/`.
- `CMakeLists.txt` deliberately wires only the blinky demo by default.
- `prj.conf` ships with commented `# === <feature> ===` blocks.
- `boards/<board>.overlay` already provides every flash partition any
  feature might need (`storage_partition`, `event_log_partition`,
  MCUboot slots, scratch).

To enable a feature you typically:

1. Add its source files to `CMakeLists.txt` (`target_sources(app PRIVATE …)`).
2. Uncomment the matching `# === <feature> ===` block in `prj.conf`.
3. Add the matching `k_thread_create(…)` call in `src/main.c`
   (reference snippets are already commented in there).
4. (Where applicable) update or extend the OTA enum so unused modules
   don't trigger a `MISSING:` warning at boot.

Step 4 is rarely needed — see [OTA enum surgery](#ota-enum-surgery)
below.

---

## Feature Matrix

| Feature | Source files | Header(s) | `prj.conf` block | DTS partition | Depends on | Tests |
|---|---|---|---|---|---|---|
| Networking (W5500) | `w5500_net.c` | `w5500_net.h` | Networking | — | — | — |
| Command UART | `command_uart.c`, `command_parse.c`, `utils.c` | `command_uart.h`, `command_parse.h`, `utils.h` | (UART is on by board default) | — | — | `test_command_parse`, `test_utils` |
| REST API | `rest_api.c`, `rest_api_endpoints.c`, `rest_logic.c` | `rest_api.h`, `rest_api_endpoints.h`, `rest_logic.h` | HTTP server | — | Networking | `test_rest_logic` |
| OTA (MCUboot + MCUmgr) | `ota.c` | `ota.h` | MCUboot OTA + MCUmgr | `slot0`, `slot1`, `scratch` (already in overlay) | Networking | — |
| Persistent config | `config_store.c` | `config_store.h` | Persistent config | `storage_partition` (already in overlay) | — | `test_config_store` |
| Event log | `event_log.c` | `event_log.h` | Event log | `event_log_partition` (already in overlay) | Persistent config | `test_event_log` |
| Time service | `time_service.c` | `time_service.h` | Time service | — | Persistent config, Networking | `test_time_service` |
| Blinky / Switch demo | `blink_thread.c`, `switch_thread.c`, `sw_blinky.c` | `blink_thread.h`, `switch_thread.h`, `sw_blinky.h`, `state_machines.h` | (no extra Kconfig) | — | — | — |

### Dependency Graph

```
                             ┌──────────────────┐
                             │  Persistent      │
                             │  config          │
                             │  (config_store)  │
                             └────┬───────┬─────┘
                                  │       │
                             ┌────▼────┐ ┌▼──────────────┐
                             │  Event  │ │  Time         │
                             │  log    │ │  service      │
                             │  (FCB)  │ │  (SNTP)       │
                             └─────────┘ └────┬──────────┘
                                              │
                                              │ needs network
                                              ▼
                                        ┌──────────────┐    ┌────────────────┐    ┌────────┐
                                        │  Networking  │ ◄──┤  REST API      │    │  OTA   │
                                        │  (W5500)     │    │  (HTTP server) │    │ MCUmgr │
                                        └──────────────┘    └────────────────┘    └────────┘
                                            ▲                                       │
                                            └───────────────────────────────────────┘
                                                            needs network
```

`event_log` and `time_service` have a benign cyclic relationship:
`event_log` *may* call `time_service_get()` to stamp wall-clock time on
each entry, and `time_service` *may* emit `event_log` events about
sync state. Both modules tolerate the other being absent — neither
hard-includes the other.

---

## Per-Feature Recipes

Recipes assume you start from a clean checkout where only the blinky
demo is compiled. Apply them in dependency order if you enable
multiple features.

### Networking (W5500)

```cmake
# CMakeLists.txt
target_sources(app PRIVATE src/w5500_net.c)
```

```ini
# prj.conf — uncomment "# === Networking ===" block
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_L2_ETHERNET=y
CONFIG_ETH_W5500=y
CONFIG_SPI=y
```

```c
/* src/main.c */
#include "w5500_net.h"
k_thread_create(&net_thread_data, net_stack, NET_STACK_SIZE,
                net_thread_entry, NULL, NULL, NULL,
                NET_PRIORITY, 0, K_NO_WAIT);
```

See [zephyr_w5500_net_guide.md](zephyr_w5500_net_guide.md) for wiring,
DHCP/static APIs, and the optional carrier/DHCP-event hook pattern.

### Command UART

```cmake
target_sources(app PRIVATE
    src/command_uart.c
    src/command_parse.c
    src/utils.c)
```

```c
/* src/main.c */
#include "command_uart.h"
k_thread_create(&command_uart_thread_data, command_uart_stack,
                COMMAND_UART_STACK_SIZE, command_uart_thread_entry,
                NULL, NULL, NULL, COMMAND_UART_PRIORITY, 0, K_NO_WAIT);
```

UART support is on by default in the Nucleo overlays. See
[zephyr_command_uart.md](zephyr_command_uart.md).

### REST API

```cmake
target_sources(app PRIVATE
    src/rest_api.c
    src/rest_api_endpoints.c
    src/rest_logic.c
    src/utils.c)
```

```ini
# prj.conf — uncomment "# === HTTP server ===" block, plus Networking
CONFIG_HTTP_SERVER=y
CONFIG_HTTP_PARSER=y
CONFIG_HTTP_PARSER_URL=y
CONFIG_JSON_LIBRARY=y
CONFIG_ZVFS=y
CONFIG_ZVFS_EVENTFD=y
CONFIG_ZVFS_EVENTFD_MAX=4   # bug 016
```

```c
/* src/main.c */
#include "rest_api.h"
k_thread_create(&rest_api_thread_data, rest_api_stack,
                REST_API_STACK_SIZE, rest_api_thread_entry,
                NULL, NULL, NULL, REST_API_PRIORITY, 0, K_NO_WAIT);
```

See [zephyr_rest_api_guide.md](zephyr_rest_api_guide.md).

### OTA (MCUboot + MCUmgr)

```cmake
target_sources(app PRIVATE src/ota.c)
```

```ini
# prj.conf — uncomment "# === MCUboot OTA + MCUmgr ===" block
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_IMG_MANAGER=y
CONFIG_STREAM_FLASH=y
CONFIG_MCUMGR=y
CONFIG_MCUMGR_TRANSPORT_UDP=y
CONFIG_MCUMGR_GRP_IMG=y
CONFIG_MCUMGR_GRP_OS=y
```

Build with sysbuild:

```bash
west build -b nucleo_h753zi --sysbuild
```

```c
/* src/main.c */
#include "ota.h"
k_thread_create(&ota_thread_data, ota_stack, OTA_STACK_SIZE,
                ota_thread_entry, NULL, NULL, NULL,
                OTA_PRIORITY, 0, K_NO_WAIT);
```

See [zephyr_ota_guide.md](zephyr_ota_guide.md) and
[ota_update_procedure.md](ota_update_procedure.md).

### Persistent config (config_store)

```cmake
target_sources(app PRIVATE src/config_store.c)
```

```ini
# prj.conf — uncomment "# === Persistent config ===" block
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_ZMS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_ZMS=y
```

```c
/* src/main.c */
#include "config_store.h"
k_thread_create(&config_store_thread_data, config_store_stack,
                CONFIG_STORE_STACK_SIZE, config_store_thread_entry,
                NULL, NULL, NULL, CONFIG_STORE_PRIORITY, 0, K_NO_WAIT);
```

See [zephyr_config_store_guide.md](zephyr_config_store_guide.md).

### Event log

```cmake
target_sources(app PRIVATE
    src/event_log.c
    src/config_store.c)   # event_log calls config_store_get_boot_count()
```

```ini
# prj.conf — uncomment "# === Event log ===" block plus Persistent config
# and Logging blocks above
CONFIG_FCB=y
CONFIG_CRC=y
```

```c
/* src/main.c */
#include "event_log.h"
k_thread_create(&event_log_thread_data, event_log_stack,
                EVENT_LOG_STACK_SIZE, event_log_thread_entry,
                NULL, NULL, NULL, EVENT_LOG_PRIORITY, 0, K_NO_WAIT);
```

The `event_log_partition` is already defined in
[boards/nucleo_h753zi.overlay](../boards/nucleo_h753zi.overlay).
See [zephyr_event_logging_guide.md](zephyr_event_logging_guide.md).

### Time service

```cmake
target_sources(app PRIVATE
    src/time_service.c
    src/config_store.c
    src/w5500_net.c)
```

```ini
# prj.conf — uncomment "# === Time service ===" block plus Networking +
# Persistent config blocks
CONFIG_SNTP=y
CONFIG_POSIX_API=y
CONFIG_POSIX_TIMERS=y
CONFIG_DNS_RESOLVER=y
CONFIG_DNS_SERVER_IP_ADDRESSES=y
CONFIG_DNS_SERVER1="8.8.8.8"
```

```c
/* src/main.c */
#include "time_service.h"
k_thread_create(&time_service_thread_data, time_service_stack,
                TIME_SERVICE_STACK_SIZE, time_service_thread_entry,
                NULL, NULL, NULL, TIME_SERVICE_PRIORITY, 0, K_NO_WAIT);
```

See [zephyr_time_service_guide.md](zephyr_time_service_guide.md).

---

## OTA Enum Surgery

`include/ota.h` declares an enum of every module the OTA health check
tracks. The default enum lists all seven modules:

```c
enum ota_module {
    OTA_MODULE_NET,
    OTA_MODULE_UART,
    OTA_MODULE_REST_API,
    OTA_MODULE_CONFIG_STORE,
    OTA_MODULE_EVENT_LOG,
    OTA_MODULE_TIME_SERVICE,
    OTA_MODULE_COUNT
};
```

`src/ota.c` has a matching `module_names[]` array guarded by
`BUILD_ASSERT(ARRAY_SIZE(module_names) == OTA_MODULE_COUNT, …)`.

If your project does **not** enable a feature, the OTA thread will
log `MISSING: <name>` for that module at the confirm-timeout boundary
and the image will be left unconfirmed. To avoid that:

1. Remove the unused enum entry from `include/ota.h`.
2. Remove the matching row from `module_names[]` in `src/ota.c`.
3. Rebuild — `BUILD_ASSERT` confirms both arrays are still in sync.

You can also bypass OTA entirely by **not** including `ota.c` in the
build at all. In that case the bootloader simply confirms the image at
the `mcuboot.conf` level, with no module-readiness gating.

---

## Tests

Each pure-logic module has a ztest suite under `tests/`. Run the full
matrix with Twister:

```bash
west twister -T tests/ -O c:\tmp\tw_all
```

Suites that build cleanly on `qemu_cortex_m3` (Windows-friendly):

- `test_command_parse`
- `test_config_store`
- `test_rest_logic`
- `test_utils`

Suites that need `qemu_x86` (FCB simulator and POSIX timers):

- `test_event_log`
- `test_time_service`

See [zephyr_unit_test_guide.md](zephyr_unit_test_guide.md) for
test-writing conventions.
