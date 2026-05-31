# Zephyr Event Logging Guide

## Overview

This project's event log is a **flash-backed circular buffer** that records
structured events across module boundaries. It uses Zephyr's
**FCB (Flash Circular Buffer)** subsystem, which provides automatic
oldest-entry rotation when the partition fills up — no manual garbage
collection required.

### Key Properties

- **Persistent**: Events survive reboots (stored in internal flash).
- **Ring buffer**: When full, the oldest sector is erased automatically.
- **Thread-safe**: All public API functions use a mutex internally.
- **Filtered**: A runtime severity level drops low-priority events before
  they reach flash, reducing write wear.
- **80-byte fixed records**: Each entry contains an uptime timestamp,
  a wall-clock timestamp, a persistent boot ID, severity, event type,
  and a 64-char message.

## Flash Partition

The event log occupies a dedicated 256 KB partition with 2 × 128 KB sectors:

| Field | Value |
|-------|-------|
| Partition label | `event-log` |
| DTS node | `event_log_partition` |
| Offset | `0x180000` |
| Size | 256 KB (2 sectors) |
| Scratch count | 1 (FCB uses one sector as scratch) |

This means only **1 active sector (128 KB)** stores data at any time.
With 72-byte entries, that's ~1,820 events before rotation.

See [memory_layout.md](memory_layout.md) for the full flash map.

## Architecture

```
┌──────────────┐     event_log_write()      ┌──────────────┐
│  Any thread  │ ──────────────────────────>│  event_log.c │
│  (w5500,     │                            │              │
│   ota,       │     event_log_read()       │  FCB on      │
│   rest_api,  │ <──────────────────────────│  flash       │
│   uart, …)   │                            │  partition   │
└──────────────┘                            └──────────────┘
       │                                           │
       │  event_log_set_level()                    │
       │  event_log_get_level()                    │
       │  event_log_clear()                        │
       └───────────────────────────────────────────┘
```

## Severity Levels (RFC 5424)

| Value | Name | When to use |
|-------|------|-------------|
| 0 | EMERG | System is unusable |
| 1 | ALERT | Immediate action required |
| 2 | CRIT | Critical conditions |
| 3 | ERR | Error conditions (OTA failures, flash errors) |
| 4 | WARN | Warning conditions (network down, link issues) |
| 5 | NOTICE | Normal but significant |
| 6 | INFO | Informational (DHCP lease, config changes, boot) |
| 7 | DEBUG | Debug messages (module init completion) |

### Mapping from `mcu_log_verbosity`

The runtime filter level is driven by `config_store`'s persisted
`mcuLogVerbosityLevel`:

| `mcu_log_verbosity` | Filter level | Events logged |
|---------------------|-------------|---------------|
| `MCU_LOG_ERROR` (0) | `EVENT_SEV_ERR` (3) | EMERG, ALERT, CRIT, ERR |
| `MCU_LOG_WARN` (1) | `EVENT_SEV_WARN` (4) | + WARN |
| `MCU_LOG_INFO` (2) | `EVENT_SEV_INFO` (6) | + NOTICE, INFO |
| `MCU_LOG_DEBUG` (3) | `EVENT_SEV_DEBUG` (7) | + DEBUG (everything) |

## Event Types

| Value | Name | Typical use |
|-------|------|-------------|
| 0 | BOOT | System boot / reboot |
| 1 | NETWORK | DHCP lease, link up/down |
| 2 | OTA | Image confirmed, upgrade, health timeout |
| 3 | CONFIG | Config field changed via REST PATCH |
| 4 | WEBHOOK | Keepalive / notification POST |
| 5 | SYSTEM | Module init, general system events |
| 6 | USER | User-triggered actions |

### Adding a New Event Type

1. Add an entry to `enum event_type` in `include/event_log.h`
   (before `EVENT_TYPE_COUNT`).
2. Add a string mapping in `event_type_to_str()` in `src/event_log.c`.

No registration or callback setup is needed.

## API Reference

### `event_log_init()`

Initialize the FCB subsystem. Called automatically by the event_log
thread on boot. Safe to call multiple times (subsequent calls are no-ops).

```c
int rc = event_log_init();
// rc == 0  → ready
// rc < 0   → flash partition not found or FCB init failed
```

### `event_log_write(sev, type, msg)`

Write an event. If `sev > current_level`, the event is silently dropped.
Messages longer than 64 bytes are truncated.

```c
event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "system boot");
event_log_write(EVENT_SEV_WARN, EVENT_TYPE_NETWORK, "link down");
event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "heartbeat");
// ↑ dropped if filter is INFO
```

### `event_log_read(since_wall, since_uptime, boot_id, boot_id_strict, callback, user_data)`

Walk the FCB oldest→newest and invoke the callback for each entry that
passes every active filter. Return `true` from the callback to continue,
`false` to stop early.

Each entry stores three independent time-identity fields:

| field        | clock                | cross-reboot? | always populated? |
|--------------|----------------------|--------------|------------------|
| `timestamp`  | `k_uptime_get()/1000` (uptime s) | no  | yes |
| `wall_clock` | UNIX epoch (s)       | yes (once synced) | only after SNTP/manual sync |
| `boot_id`    | `mcu/bootCount` (persistent counter) | yes (unique per boot) | yes |

The `boot_id` is the persistent identity field: it is set once per
boot by `event_log_init()` from config_store's persistent
`mcu/bootCount` counter (incremented and saved on every init). It
lets a reader disambiguate *pre-sync* entries across reboots — their
uptime value aliases, but their `boot_id` does not. See
[bug_reports/019](../bug_reports/019_event_log_cross_boot_time_filter.md).

Filter evaluation is **asymmetric** between pre-sync and sync'd
entries. Sync'd entries carry an authoritative cross-boot timestamp
(`wall_clock`), so the wall-clock window alone is enough to pick the
right ones — even if they were written by a previous boot. Pre-sync
entries have only uptime, which aliases across reboots, so they must
be gated by `boot_id` to avoid leaking prior-boot noise.

```text
for each entry walked:
    # Strict mode: every entry must match the requested boot.
    if boot_id_strict and boot_id != 0 and entry.boot_id != boot_id:
        skip

    if entry.wall_clock != 0:
        # Sync'd entry — wall axis only. boot_id is NOT applied in
        # default mode because the timestamp already uniquely places
        # the entry on the wall timeline, regardless of boot.
        if since_wall != 0 and entry.wall_clock < since_wall: skip
    else:
        # Pre-sync entry — emitted iff its boot is "in scope" for
        # the wall window:
        #   * always emit current-boot pre-sync, OR
        #   * emit if entry.boot_id > min_in_window
        #     (a younger boot that started inside the window —
        #      its banner/DHCP/SNTP-failed lines belong with its
        #      sync'd lines that already passed the wall filter).
        # `min_in_window` is the smallest boot_id whose sync'd
        # entries passed the wall window in pass 1.
        in_scope = (entry.boot_id == boot_id) or
                   (min_valid and entry.boot_id > min_in_window)
        if not in_scope: skip
        if since_uptime != 0 and entry.timestamp < since_uptime: skip

    emit entry
```

Passing `(0, 0, 0, false, cb, ud)` returns every entry in the log
(no anchor + no caller filter ⇒ "dump everything" path).

#### Why this rule?

The two classes of entries have different identity guarantees, so
they need different filter rules:

- **Sync'd entries** (`wall_clock != 0`) carry a cross-boot-comparable
  timestamp. They are gated only by the wall window, regardless of
  which boot wrote them.
- **Pre-sync entries** (`wall_clock == 0`) only have uptime, which
  resets to 0 on every boot. The current boot's pre-sync is always
  emitted (the operator's own boot context). A previous boot's
  pre-sync is only emitted if that boot's `boot_id > min_in_window`
  — meaning it was a *younger* boot that started after the boot
  whose sync'd entries first qualified for the window. Boots
  strictly older than that have their pre-sync suppressed because
  those entries occurred outside the window.

The `boot_id_strict` flag overrides this and applies `boot_id` to
every entry, which is what `event_log_boot <id>` uses to dump a
specific historic boot's full log without bleed from adjacent boots.

| intent                                        | call                                            |
|-----------------------------------------------|-------------------------------------------------|
| "Last 60 s of wall time (any boot, sync'd)"   | `(now_wall-60, 0, bid, false, cb, ud)`          |
| "Dump boot 42's full log, only boot 42"       | `(0, 0, 42, true,  cb, ud)`                     |
| "Everything across every boot"                | `(0, 0,  0, false, cb, ud)`                     |

#### Worked example

Device has gone through five boots (current `boot_id=5`); the FCB
ring still holds entries from boots 3 and 4. Operator types
`event_log 600` after the current boot has just sync'd:

| # | boot_id | `timestamp` | `wall_clock`           | emitted | rule |
|---|---------|-------------|------------------------|---------|------|
| 1 | 3       | 0           | 0                      | ❌ | pre-sync; boot 3 == min_in_window |
| 2 | 3       | 7           | 1_714_019_030 (sync'd) | ✅ | sync'd, in wall window |
| 3 | 4       | 0           | 0                      | ✅ | pre-sync; boot 4 > min_in_window (3) |
| 4 | 4       | 5           | 1_714_019_044 (sync'd) | ✅ | sync'd, in wall window |
| 5 | 5       | 0           | 0                      | ✅ | pre-sync, current boot |
| 6 | 5       | 5           | 0                      | ✅ | pre-sync, current boot |
| 7 | 5       | 5           | 0                      | ✅ | pre-sync, current boot |
| 8 | 5       | 5           | 1_714_019_094 (sync'd) | ✅ | sync'd, in wall window |

`min_in_window = 3` (the smallest boot_id whose sync'd entries fell
in the 600 s wall window). Boot 3's pre-sync banner is suppressed
(boot 3 is the anchor — its pre-sync occurred *before* the window).
Boot 4's banner is included because 4 > 3 — it started inside the
window and sync'd a few seconds later. Boot 5's pre-sync is always
emitted as the current boot.

If the operator instead types `event_log_boot 4` (strict mode), only
entries 3 and 4 are emitted.

#### Special-case inputs

| `since_wall` | `since_uptime` | `boot_id` | `strict` | Meaning                                                              |
|-------------:|---------------:|----------:|:--------:|----------------------------------------------------------------------|
| 0            | 0              | 0         | false    | Dump everything, every boot.                                         |
| 0            | 0              | X         | true     | Full log of boot X only.                                             |
| W > 0        | 0              | X         | false    | Sync'd entries in wall window + current-boot pre-sync + pre-sync of every boot whose `boot_id > min_in_window`. |
| W > 0        | 0              | X         | true     | Boot X's pre-sync + boot X's sync'd entries in window.               |
| W > 0        | 0              | 0         | false    | Sync'd entries in wall window + pre-sync of every boot with `boot_id > min_in_window`. |

#### Snippets

```c
static bool print_event(const event_entry_t *e, void *ctx) {
    printk("[boot %u][%u] %s %s: %.*s\n",
           e->boot_id, e->timestamp,
           severity_to_str(e->severity),
           event_type_to_str(e->event_type),
           e->data_len, e->message);
    return true;
}

/* "Last 60 s" — the common UART case. Pre-sync of this boot + any
 * sync'd entry in the wall window, from any boot. */
uint32_t bid   = event_log_get_boot_id();
uint32_t now_w = time_service_is_synced()
               ? (uint32_t)time_service_get() : 0;
uint32_t since_w = (now_w > 60) ? now_w - 60 : 0;
int n = event_log_read(since_w, 0, bid, /*strict=*/false,
                       print_event, NULL);

/* Replay boot 42 in full (strict). */
event_log_read(0, 0, 42, /*strict=*/true, print_event, NULL);

/* Dump everything. */
event_log_read(0, 0, 0, /*strict=*/false, print_event, NULL);
```

See [bug_reports/019_event_log_cross_boot_time_filter.md](../bug_reports/019_event_log_cross_boot_time_filter.md)
for the full evolution: single `since_epoch` → two-axis `since_wall /
since_uptime` → three-axis with `boot_id` → asymmetric `boot_id` →
`min_in_window` cross-boot pre-sync inclusion (current).

### `event_log_get_boot_id()`

Returns the boot ID stamped on every entry written during this boot.
Value is loaded (and incremented) by `event_log_init()` from the
persistent `mcu/bootCount` setting in config_store. Stable for the
lifetime of the firmware's run.

```c
uint32_t id = event_log_get_boot_id();
// id == 42 (on the 42nd boot since factory reset)
```

### `event_log_clear()`

Erase all events (full FCB clear).

```c
event_log_clear();
```

### `event_log_set_level(level)` / `event_log_get_level()`

Change or query the runtime severity filter. Thread-safe (atomic).

```c
event_log_set_level(EVENT_SEV_WARN);
// Now only EMERG..WARN are logged

enum event_severity lvl = event_log_get_level();
```

## UART Commands

| Command | Description |
|---------|-------------|
| `event_log` | Dump this-boot events from last 300 seconds |
| `event_log <N>` | Dump this-boot events from last N seconds (0 = all of this boot) |
| `event_log_boot <id> [seconds]` | Dump entries stamped with `boot_id == <id>`; optional wall-clock window |
| `log_level_get` | Print current log verbosity level |
| `log_level_set <level>` | Set log level (`error`, `warn`, `info`, `debug`) |

The default `event_log` pins the current boot's `boot_id` so prior-boot
noise (still in the FCB ring after reboot) is filtered out. To inspect
an earlier boot, use `event_log_boot <id>` — the current boot's id is
printed in each line and in the footer of every dump, making it easy
to note historic ids.

Example UART session:

```
> log_level_set debug
log_level set to: debug

> event_log 60
[boot 3][12s] INFO BOOT: system boot fw=v0.1.4
[boot 3][12s] DEBUG SYSTEM: event_log init done
[boot 3][13s] DEBUG NETWORK: w5500_net init done
[boot 3][15s] INFO NETWORK: DHCP lease: 192.168.1.100
[boot 3][22s 2026-04-24T19:20:22Z] INFO SYSTEM: time synced via SNTP
--- 5 event(s) from boot 3 ---

> event_log_boot 2
[boot 2][0s] INFO BOOT: system boot fw=v0.1.3
[boot 2][11s] INFO NETWORK: DHCP lease: 192.168.1.100
[boot 2][31s] ERR OTA: health check timeout — image not confirmed
--- 3 event(s) from boot 2 ---
```

## REST API Integration

The PATCH `/api/mcu` endpoint syncs the event log filter level whenever
`mcuLogVerbosityLevel` is changed. Each config field change also emits
an `EVENT_SEV_INFO` / `EVENT_TYPE_CONFIG` event.

## Built-in Event Emission Points

| Module | Event | Severity | Type |
|--------|-------|----------|------|
| event_log | `system boot` | INFO | BOOT |
| event_log | `event_log init done` | DEBUG | SYSTEM |
| w5500_net | `w5500_net init done` | DEBUG | NETWORK |
| w5500_net | `DHCP lease: <addr>` | INFO | NETWORK |
| w5500_net | `network link up after <ms>ms` | WARN | NETWORK |
| w5500_net | `network interface never came up` | WARN | NETWORK |
| command_uart | `command_uart init done` | DEBUG | SYSTEM |
| rest_api | `rest_api init done` | DEBUG | SYSTEM |
| config_store | `config_store init done` | DEBUG | CONFIG |
| keepalive | `keepalive init done` | DEBUG | SYSTEM |
| ota | `image confirmed after <N>s` | INFO | OTA |
| ota | `image confirmation failed: <rc>` | ERR | OTA |
| ota | `health check timeout` | ERR | OTA |
| rest_api_ep | `<field> changed` (per config field) | INFO | CONFIG |

## Unit Tests

Tests live in `tests/test_event_log/` and run on `native_sim` (which
provides simulated flash). Run with:

```bash
west twister -T tests/test_event_log -p native_sim
```

On Windows hosts use `qemu_x86` instead (also listed in `platform_allow`):

```bash
west twister -T tests/test_event_log -p qemu_x86
```

Test suites:
- **init**: `event_log_init()` succeeds and is idempotent
- **write**: single/multiple writes, truncation, null/empty messages
- **read**: empty log, null callback, stop-early, since-filter
- **clear**: empties log, write-after-clear
- **level**: set/get, filter drops at INFO/WARN/ERR, DEBUG passes all
- **strings**: `severity_to_str()`, `event_type_to_str()`, invalid values
