# Zephyr Time Service Guide

## Overview

The time service provides wall-clock (real-world) time for the firmware
by periodically syncing with an NTP server using SNTP, then maintaining the
clock via the POSIX `CLOCK_REALTIME` interface.

## Architecture

```
┌──────────────┐     SNTP query      ┌───────────────┐
│ time_service │ ──────────────────► │  NTP server   │
│   thread     │ ◄────────────────── │ (configurable)│
│  (pri 10)    │   epoch seconds     └───────────────┘
└──────┬───────┘
       │ clock_settime(CLOCK_REALTIME)
       ▼
┌──────────────┐
│ POSIX clock  │ ◄── clock_gettime() ── other modules
│ (kernel)     │
└──────────────┘
```

## Configuration

Three persistent settings in the config store:

| Setting             | Key (ZMS)            | Type     | Default        | Range        |
|---------------------|----------------------|----------|----------------|--------------|
| NTP Server          | `mcu/ntpServer`      | string   | `216.239.35.0` | max 63 chars |
| NTP Sync Interval   | `mcu/ntpSyncIntervalSec` | uint32 | `600` (10 min) | 60–86400     |
| DNS Server          | `mcu/dnsServer`      | string   | `8.8.8.8`      | max 63 chars |

### Kconfig Dependencies

```
CONFIG_SNTP=y
CONFIG_POSIX_TIMERS=y
CONFIG_DNS_RESOLVER=y
CONFIG_DNS_SERVER_IP_ADDRESSES=y
CONFIG_DNS_SERVER1="8.8.8.8"
```

## API Reference

### `int time_service_init(void)`

Resets internal state (sync flag, re-sync semaphore) and writes a
DEBUG event-log entry.  Called automatically from the thread entry,
safe to call directly in tests.

```c
time_service_init();
// time_service_is_synced() == false
```

### `int64_t time_service_get(void)`

Returns the current wall-clock time as UNIX epoch seconds.  Always
returns a plausible value — the fallback clock (seeded at init with
`2025-01-01T00:00:00Z`) is used until the first real SNTP sync or
manual `time_service_set`.  Use `time_service_is_synced()` to tell
real time from fallback.

```c
int64_t now = time_service_get();
```

### `int time_service_try_sync_now(void)`

Runs one SNTP sync attempt synchronously on the caller's thread and
returns the result.  When the NTP server string is empty this
returns `0` immediately and leaves the clock alone.

```c
int rc = time_service_try_sync_now();
// rc == 0  → synced (or skipped because server is empty)
// rc == -ETIMEDOUT → SNTP request timed out
```

### `int time_service_set(int64_t epoch_sec)`

Manually sets the wall clock and marks the service as synced.

```c
time_service_set(1718049600);  // 2024-06-10T20:00:00Z
```

### `void time_service_sync(void)`

Triggers an immediate SNTP re-sync (non-blocking).

```c
time_service_sync();
```

### `bool time_service_is_synced(void)`

Returns `true` if the wall clock has been set at least once.

### `int time_service_format_iso8601(int64_t epoch_sec, char *buf, size_t buf_len)`

Formats a UNIX timestamp as `YYYY-MM-DDTHH:MM:SSZ`.  
Buffer must be >= 21 bytes.

```c
char iso[21];
time_service_format_iso8601(1718049600, iso, sizeof(iso));
// iso == "2024-06-10T20:00:00Z"
```

### `int time_service_parse_iso8601(const char *str, int64_t *epoch_sec)`

Parses a `YYYY-MM-DDTHH:MM:SSZ` string into a UNIX timestamp.

```c
int64_t ts;
time_service_parse_iso8601("2024-06-10T20:00:00Z", &ts);
// ts == 1718049600
```

## UART Commands

| Command                        | Description                          |
|--------------------------------|--------------------------------------|
| `time_get`                     | Show current wall-clock time         |
| `time_set <YYYY-MM-DDTHH:MM:SSZ>` | Manually set the wall clock      |
| `time_sync`                    | Trigger immediate SNTP sync          |

## REST API

The `/api/mcu` endpoint exposes the time-related config fields:

### GET /api/mcu (response includes)

```json
{
  "ntpServer": "216.239.35.0",
  "ntpSyncIntervalSec": 600,
  "dnsServer": "8.8.8.8"
}
```

### PATCH /api/mcu (updatable fields)

```json
{
  "ntpServer": "pool.ntp.org",
  "ntpSyncIntervalSec": 3600,
  "dnsServer": "1.1.1.1"
}
```

## Event Log Integration

Event entries now include a `wall_clock` field (UNIX epoch seconds):
- Set to the real wall-clock time if `time_service_is_synced()` is true
- Set to `0` if the clock has not been synced yet

The UART `event_log` dump shows ISO 8601 timestamps when `wall_clock` is available:

```
[42s 2024-06-10T20:00:00Z] INFO SYSTEM: time synced via SNTP
[15s] INFO BOOT: firmware boot                         (before sync)
```

## DNS Runtime Reconfiguration

The `net_set_ip()` function accepts an optional `dns` parameter. When
provided, it reconfigures the Zephyr DNS resolver at runtime using
`dns_resolve_init()`. This allows the DNS server to change alongside
the IP address without a reboot.

```c
net_set_ip(&cfg, "1.1.1.1");    // set IP + change DNS
net_set_ip(&cfg, NULL);          // set IP, leave DNS unchanged
```

## Sync Triggers

The time service re-syncs:
1. **Periodically** — every `ntpSyncIntervalSec` seconds (default 10 min)
2. **On network link-up** — the carrier callback calls `time_service_sync()`
3. **On user request** — UART `time_sync` command or REST API

## Thread Details

- Stack size: 2048 bytes
- Priority: 10 (lowest, background)
- Initial delay: 5 seconds (waits for network)

## Initialisation

`time_service_init()` is called first thing from the thread entry.
It resets the `synced` flag, re-arms the re-sync semaphore, seeds
`CLOCK_REALTIME` with a placeholder fallback date
(`2025-01-01T00:00:00Z`), and writes a **DEBUG** event-log entry
`"time service initialised"`.  Unit tests that don't spawn the thread
can call it directly to reset module state between test cases.

The fallback clock only affects readable wall-clock stamps — the
`time_service_is_synced()` flag stays `false` until a real SNTP sync
or an explicit `time_service_set()` succeeds.

## Empty NTP Server Configuration

Setting `mcu/ntpServer` to an empty string is a **valid** configuration
meant for deployments where the clock is set manually (UART
`time_set`, REST `PATCH /api/mcu`) or delivered through a keepalive
response.  In that case `do_sntp_sync()`:

- returns **`0`** (success) without contacting any server,
- emits a DEBUG event `sntp skip: no server configured`,
- leaves `time_service_is_synced()` unchanged.

The module still reports ready to the OTA health system so the image
confirmation is not blocked by the lack of an NTP server.

## Event Log Entries Emitted

| Severity | Type   | Message                                 | When emitted                    |
|----------|--------|-----------------------------------------|---------------------------------|
| DEBUG    | SYSTEM | `time service initialised`              | Module init                     |
| DEBUG    | SYSTEM | `sntp skip: no server configured`       | Empty NTP server, sync skipped  |
| DEBUG    | SYSTEM | `sntp sync ok: 2024-06-10T20:00:00Z`    | Every successful SNTP sync      |
| INFO     | SYSTEM | `time synced via SNTP`                  | First successful sync only      |
| WARN     | SYSTEM | `sntp sync failed rc=<errno> server=…`  | SNTP request failed             |
| WARN     | SYSTEM | `clock_settime failed rc=<errno>`       | POSIX `clock_settime` rejected  |

DEBUG entries are only recorded when the runtime log filter is set to
`DEBUG` (via `event_log_set_level()` or `mcu/logVerbosity` = `debug`).
WARN and INFO entries are always recorded under default settings.

## Testing

The test suite `tests/test_time_service/` runs 30+ cases on `qemu_x86`,
covering:
- ISO 8601 formatting and parsing (good paths)
- Bad-path parsing (invalid month/day/hour, wrong length, missing
  separators, year before 1970, …)
- Buffer-too-small formatting
- Manual `time_service_set` + `time_service_get` round-trip
- `time_service_init` behaviour (returns 0, clears sync flag)

The test uses a local `zephyr/net/sntp.h` shim so the binary does not
pull in the Zephyr networking stack — see
[`bug_reports/013_time_service_test_hang_networking_stack.md`](../bug_reports/013_time_service_test_hang_networking_stack.md).

## Future Work: External RTC Chip Backend

### What is an RTC chip?

A real-time clock (RTC) chip is a small dedicated IC (e.g. DS3231,
PCF8563, MCP7940N, RV-3028) that keeps wall-clock time independently
of the MCU. It runs from a tiny coin-cell or supercap on its own
`VBAT` pin, uses a 32.768 kHz crystal (often temperature-compensated),
and exposes time/date registers over I²C or SPI. Many parts also
provide alarms, a square-wave/interrupt output, and a few bytes of
battery-backed SRAM.

### How is it different from the current implementation?

Today the time service keeps wall-clock time entirely inside the MCU:

| Aspect              | Current (kernel + SNTP)            | External RTC chip                       |
|---------------------|------------------------------------|-----------------------------------------|
| Time source         | POSIX `CLOCK_REALTIME` (SysTick)   | Dedicated 32.768 kHz crystal on the IC  |
| Survives reset      | No — re-syncs from NTP on boot     | Yes — RTC keeps running                 |
| Survives power loss | No — falls back to build epoch     | Yes, while VBAT/coin-cell is present    |
| Drift               | MCU oscillator (tens–hundreds ppm) | TCXO RTCs reach ±2 ppm (<1 min/year)    |
| Needs network       | Yes (for first accurate sync)      | No (only to correct long-term drift)    |
| Extra hardware      | None                               | RTC IC + crystal + battery + I²C/SPI    |

The kernel-clock approach is fine while the device has frequent
network access and is rarely power-cycled. The RTC chip becomes
attractive for devices that boot offline, log events before NTP is
reachable, or must produce trustworthy timestamps across power cuts.

### Planned integration

The intent is to keep the public API (`time_service_get`,
`time_service_is_synced`, ISO 8601 helpers) **unchanged** and add the
RTC as an optional backend behind a Kconfig switch:

```
CONFIG_APP_TIME_SERVICE_RTC_BACKEND=y     # opt in
CONFIG_APP_TIME_SERVICE_RTC_DEV="rtc0"    # devicetree node label
```

Sketch of the integration points inside `time_service.c`:

1. **Boot path** — before starting the SNTP loop, read the RTC via
   Zephyr's `<zephyr/drivers/rtc.h>` (`rtc_get_time`). If the RTC
   reports a plausible time (≥ build epoch), call `clock_settime()`
   with it and set the sync flag. Devices that boot offline now have a
   real wall-clock immediately.
2. **After every successful SNTP sync** — push the freshly-synced
   value back to the RTC with `rtc_set_time`. The RTC becomes the
   long-term keeper; SNTP only corrects drift.
3. **Manual set** (`time_service_set`, UART `time_set`, REST
   `PATCH /api/mcu`) — also writes through to the RTC so the next
   cold boot keeps the operator-supplied time.
4. **Periodic re-read** (optional) — if the MCU clock drifts faster
   than the RTC, a low-priority timer can re-load `CLOCK_REALTIME`
   from the RTC between SNTP syncs.

Devicetree on the target board would add an I²C/SPI child node, e.g.:

```dts
&i2c1 {
    rtc0: ds3231@68 {
        compatible = "maxim,ds3231";
        reg = <0x68>;
    };
};
```

### Status: not supported yet

**This backend is not implemented in the current template.** A future
project that needs cold-boot timestamps or offline operation will add
it — likely as a thin `time_service_rtc.c` module compiled in only
when `CONFIG_APP_TIME_SERVICE_RTC_BACKEND=y`. Until then the time
service relies on SNTP plus the build-epoch fallback documented above.

