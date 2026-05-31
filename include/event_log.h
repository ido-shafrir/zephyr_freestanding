#ifndef EVENT_LOG_H
#define EVENT_LOG_H

/**
 * @file event_log.h
 * @brief Flash-backed circular event log using Zephyr FCB.
 *
 * Provides a ring-buffer event log stored on a dedicated flash partition
 * ("event-log", 256 KB / 2 sectors).  Events are 76-byte fixed-size
 * records with a severity level modelled after Linux syslog (RFC 5424).
 *
 * The runtime filter level is driven by config_store's mcu_log_verbosity:
 *   MCU_LOG_ERROR → EVENT_SEV_ERR   (drops WARN/NOTICE/INFO/DEBUG)
 *   MCU_LOG_WARN  → EVENT_SEV_WARN  (drops NOTICE/INFO/DEBUG)
 *   MCU_LOG_INFO  → EVENT_SEV_INFO  (drops only DEBUG)
 *   MCU_LOG_DEBUG → EVENT_SEV_DEBUG (logs everything)
 *
 * Thread-safe — all public functions may be called from any context.
 *
 * To add a new event type:
 *   1. Add an entry to enum event_type (before EVENT_TYPE_COUNT).
 *   2. Add a string in event_type_to_str() in event_log.c.
 *   That's it — no registration required.
 */

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------- Thread configuration ---------- */
#define EVENT_LOG_STACK_SIZE   2048
#define EVENT_LOG_PRIORITY     7
#define EVENT_LOG_MSG_MAX_LEN  64

/* ---------- Severity levels (RFC 5424 / Linux syslog) ---------- */

/**
 * @brief Event severity levels, modelled after Linux kernel log levels.
 *
 * Lower numeric value = higher severity.  The runtime filter drops
 * events whose severity value is strictly greater than the current level.
 *
 * @par Level table
 * | Value | Name   | Description                        |
 * |-------|--------|------------------------------------|
 * | 0     | EMERG  | System is unusable                 |
 * | 1     | ALERT  | Action must be taken immediately   |
 * | 2     | CRIT   | Critical conditions                |
 * | 3     | ERR    | Error conditions                   |
 * | 4     | WARN   | Warning conditions                 |
 * | 5     | NOTICE | Normal but significant             |
 * | 6     | INFO   | Informational messages             |
 * | 7     | DEBUG  | Debug-level messages               |
 */
enum event_severity {
    EVENT_SEV_EMERG  = 0,
    EVENT_SEV_ALERT  = 1,
    EVENT_SEV_CRIT   = 2,
    EVENT_SEV_ERR    = 3,
    EVENT_SEV_WARN   = 4,
    EVENT_SEV_NOTICE = 5,
    EVENT_SEV_INFO   = 6,
    EVENT_SEV_DEBUG  = 7,
};

/* ---------- Event types ---------- */

/**
 * @brief Categories of events recorded in the log.
 *
 * @par Type table
 * | Value | Name    | Typical use                        |
 * |-------|---------|------------------------------------|
 * | 0     | BOOT    | System boot / reboot               |
 * | 1     | NETWORK | DHCP, link up/down, IP changes     |
 * | 2     | OTA     | Image confirmed, upgrade started   |
 * | 3     | CONFIG  | Config store changes via REST API   |
 * | 4     | WEBHOOK | Keepalive / notification POST       |
 * | 5     | SYSTEM  | Module init, general system events  |
 * | 6     | USER    | User-triggered actions              |
 */
enum event_type {
    EVENT_TYPE_BOOT = 0,
    EVENT_TYPE_NETWORK,
    EVENT_TYPE_OTA,
    EVENT_TYPE_CONFIG,
    EVENT_TYPE_WEBHOOK,
    EVENT_TYPE_SYSTEM,
    EVENT_TYPE_USER,
    EVENT_TYPE_COUNT,
};

/* ---------- Event entry structure ---------- */

/**
 * @brief A single event log entry (80 bytes, packed).
 *
 * Stored directly in flash via FCB.
 *
 * Three independent timestamps tag each record:
 *   - `timestamp`  — uptime seconds (`k_uptime_get()/1000`). Resets on
 *                    every reboot, unique within a boot.
 *   - `wall_clock` — UNIX epoch seconds if the time service was
 *                    synced at write time, 0 otherwise. Monotonic
 *                    across reboots (once synced), but 0 for every
 *                    pre-sync entry.
 *   - `boot_id`    — persistent, monotonically-increasing boot
 *                    counter (`mcu/bootCount`). Stamps every record
 *                    with the boot session it was written in, so
 *                    cross-boot noise can be filtered out even when
 *                    `wall_clock` is 0 (see bug report #019).
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * 0       4     timestamp   (uint32_t — uptime seconds)
 * 4       4     wall_clock  (uint32_t — UNIX epoch seconds, 0 if unsynced)
 * 8       4     boot_id     (uint32_t — persistent boot counter)
 * 12      1     event_type  (uint8_t  — enum event_type)
 * 13      1     severity    (uint8_t  — enum event_severity)
 * 14      2     data_len    (uint16_t — actual message length)
 * 16      64    message     (char[]   — NUL-padded description)
 * Total: 80 bytes
 * @endcode
 */
typedef struct {
    uint32_t timestamp;
    uint32_t wall_clock;
    uint32_t boot_id;
    uint8_t  event_type;
    uint8_t  severity;
    uint16_t data_len;
    char     message[EVENT_LOG_MSG_MAX_LEN];
} __packed event_entry_t;

/* ---------- Walk callback ---------- */

/**
 * @brief Callback invoked for each event during event_log_read().
 *
 * @param entry     Pointer to the deserialized event entry.
 * @param user_data Opaque context pointer passed through from the caller.
 * @return true to continue walking, false to stop early.
 *
 * @par Example
 * @code
 * static bool print_event(const event_entry_t *e, void *ctx) {
 *     printk("[%u] %s\n", e->timestamp, e->message);
 *     return true;  // continue
 * }
 * int count = event_log_read(0, print_event, NULL);
 * @endcode
 */
typedef bool (*event_log_walk_cb)(const event_entry_t *entry, void *user_data);

/* ---------- Thread resources (defined in event_log.c) ---------- */
extern struct k_thread event_log_thread_data;
extern k_thread_stack_t event_log_stack[];

/* ---------- Public API ---------- */

/**
 * @brief Initialize the FCB event log subsystem.
 *
 * Opens the "event-log" flash partition, queries sector geometry,
 * and calls fcb_init().  Called automatically by the event_log thread,
 * but exposed for unit tests that don't spawn the thread.
 *
 * @return 0 on success, negative errno on failure.
 *
 * @par Example
 * @code
 * int rc = event_log_init();
 * // rc == 0  → FCB ready
 * // rc == -ENODEV → flash partition not found
 * @endcode
 */
int event_log_init(void);

/**
 * @brief Write an event to the log.
 *
 * If the event's severity value is greater than the current filter
 * level, the event is silently dropped (returns 0).  Messages longer
 * than EVENT_LOG_MSG_MAX_LEN (64) bytes are truncated.
 *
 * @param sev  Severity level (EVENT_SEV_EMERG … EVENT_SEV_DEBUG).
 * @param type Event category (EVENT_TYPE_BOOT … EVENT_TYPE_USER).
 * @param msg  Human-readable description (NUL-terminated, max 64 chars).
 * @return 0 on success (including filtered-out events), negative errno on
 *         flash write failure.
 *
 * @par Examples
 * | sev          | type              | msg                  | filter=INFO | Result     |
 * |--------------|-------------------|----------------------|-------------|------------|
 * | EVENT_SEV_INFO  | EVENT_TYPE_BOOT | "system boot"     | INFO        | Written    |
 * | EVENT_SEV_DEBUG | EVENT_TYPE_SYSTEM | "heartbeat tick" | INFO        | Dropped    |
 * | EVENT_SEV_ERR   | EVENT_TYPE_OTA  | "confirm failed"  | INFO        | Written    |
 *
 * @code
 * event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "system boot");
 * event_log_write(EVENT_SEV_WARN, EVENT_TYPE_NETWORK, "link down");
 * @endcode
 */
int event_log_write(enum event_severity sev, enum event_type type,
                    const char *msg);

/**
 * @brief Read events from the log, filtered by time and/or boot ID.
 *
 * Walks the FCB from oldest to newest and invokes @p cb for each
 * entry that passes all active filters.  If the callback returns
 * false, the walk stops early.
 *
 * The filter is asymmetric by design, because a `boot_id` is only
 * needed to disambiguate entries whose `wall_clock` is 0 (pre-sync
 * entries — where per-boot uptime aliases across reboots). Entries
 * written after SNTP sync already carry a monotonic, cross-reboot
 * `wall_clock`, so they are filtered purely by @p since_wall and
 * are emitted regardless of the boot they came from.
 *
 * Default (@p boot_id_strict == false):
 *   1. `entry.wall_clock != 0` — sync'd entries: pass if
 *      `since_wall == 0 || entry.wall_clock >= since_wall`,
 *      regardless of boot.
 *   2. `entry.wall_clock == 0` — pre-sync entries: emitted iff
 *      the entry's boot is "in scope" for the requested window:
 *        - the entry's boot_id equals @p boot_id (current boot's
 *          pre-sync is always shown), OR
 *        - the entry's boot_id is strictly greater than the
 *          smallest boot_id whose sync'd entries fell inside the
 *          wall window (`min_in_window`). Younger boots that
 *          started inside the window contribute their pre-sync
 *          banner / DHCP / SNTP-failed diagnostics; the boot
 *          that produced `min_in_window` itself is excluded
 *          because its pre-sync occurred *before* the window.
 *      Pre-sync entries also honour @p since_uptime when set.
 *      When the log contains no sync'd entry inside the window
 *      (no anchor), pre-sync emission collapses to "current boot
 *      only" (same as the iteration-4 rule).
 *
 * Strict (@p boot_id_strict == true):
 *   - Every entry (sync'd or not) must also have
 *     `entry.boot_id == boot_id`. Used by `event_log_boot <id>`
 *     to replay one specific boot's full log.
 *
 * @param since_wall       Wall-clock cutoff (UNIX epoch). 0 = no
 *                         wall filter.
 * @param since_uptime     Uptime cutoff (s) for pre-sync entries.
 *                         0 = no uptime filter.
 * @param boot_id          Boot ID (0 = no boot filter).
 * @param boot_id_strict   If true, @p boot_id applies to every
 *                         entry. If false, only to pre-sync entries.
 * @param cb               Callback invoked per matching entry.
 * @param user_data        Opaque pointer forwarded to @p cb.
 * @return Number of entries passed to the callback (>= 0), or
 *         negative errno on FCB read failure.
 *
 * @par Example
 * @code
 * uint32_t bid = event_log_get_boot_id();
 *
 * // "Last 300 s of wall time, plus current boot's pre-sync".
 * // Sync'd entries from previous boots that fall in the window
 * // are included (their wall clocks are authoritative).
 * event_log_read(now_wall - 300, 0, bid, false, cb, ud);
 *
 * // Full replay of boot 42 (strict).
 * event_log_read(0, 0, 42, true,  cb, ud);
 *
 * // Every entry ever written.
 * event_log_read(0, 0, 0, false, cb, ud);
 * @endcode
 */
int event_log_read(uint32_t since_wall, uint32_t since_uptime,
                   uint32_t boot_id, bool boot_id_strict,
                   event_log_walk_cb cb, void *user_data);

/**
 * @brief Return the boot ID stamped on every entry written during
 *        this boot session.
 *
 * The value is loaded and incremented once by `event_log_init()`
 * from the persistent counter `mcu/bootCount` in config_store.
 * It is stable for the lifetime of the firmware's run.
 *
 * @return Current boot ID (>= 1 after a successful init; 0 if
 *         `event_log_init()` has not yet completed).
 *
 * @par Example
 * @code
 * uint32_t id = event_log_get_boot_id();
 * printk("this boot has id %u\n", id);
 * @endcode
 */
uint32_t event_log_get_boot_id(void);

/**
 * @brief Erase all events from the log.
 *
 * Performs a full FCB clear (erases all sectors except scratch).
 *
 * @return 0 on success, negative errno on flash erase failure.
 *
 * @par Example
 * @code
 * event_log_clear();
 * int n = event_log_read(0, my_cb, NULL);
 * // n == 0
 * @endcode
 */
int event_log_clear(void);

/**
 * @brief Erase all events from the log (alias for event_log_clear).
 *
 * Provided for API clarity; identical to event_log_clear().
 */
static inline int event_log_drop_all(void) { return event_log_clear(); }

/**
 * @brief Set the runtime severity filter level.
 *
 * Events with severity > level are silently dropped by event_log_write().
 * Thread-safe (uses atomic store).
 *
 * @param level Maximum severity value to accept (EVENT_SEV_EMERG … EVENT_SEV_DEBUG).
 *
 * @par Example
 * @code
 * event_log_set_level(EVENT_SEV_WARN);
 * // Now only EMERG, ALERT, CRIT, ERR, WARN are logged
 *
 * event_log_set_level(EVENT_SEV_DEBUG);
 * // Everything is logged
 * @endcode
 */
void event_log_set_level(enum event_severity level);

/**
 * @brief Get the current runtime severity filter level.
 *
 * @return Current filter level (EVENT_SEV_EMERG … EVENT_SEV_DEBUG).
 *
 * @par Example
 * @code
 * enum event_severity lvl = event_log_get_level();
 * // lvl == EVENT_SEV_INFO (default after init with MCU_LOG_INFO)
 * @endcode
 */
enum event_severity event_log_get_level(void);

/**
 * @brief Convert a severity enum to a human-readable string.
 *
 * @param sev Severity value.
 * @return Static string pointer (e.g. "INFO", "ERR", "DEBUG").
 *
 * @par Examples
 * | Input            | Output   |
 * |------------------|----------|
 * | EVENT_SEV_EMERG  | "EMERG"  |
 * | EVENT_SEV_ERR    | "ERR"    |
 * | EVENT_SEV_INFO   | "INFO"   |
 * | EVENT_SEV_DEBUG  | "DEBUG"  |
 */
const char *severity_to_str(enum event_severity sev);

/**
 * @brief Convert an event type enum to a human-readable string.
 *
 * @param type Event type value.
 * @return Static string pointer (e.g. "BOOT", "NETWORK", "CONFIG").
 *
 * @par Examples
 * | Input               | Output    |
 * |---------------------|-----------|
 * | EVENT_TYPE_BOOT     | "BOOT"    |
 * | EVENT_TYPE_NETWORK  | "NETWORK" |
 * | EVENT_TYPE_CONFIG   | "CONFIG"  |
 */
const char *event_type_to_str(enum event_type type);

/**
 * @brief Event log thread entry point.
 *
 * Sequence:
 *   1. Call event_log_init() to mount the FCB partition.
 *   2. Read mcu_log_verbosity from config_store → set filter level.
 *   3. Log the current filter level to the serial debug output.
 *   4. Report ready to the OTA health check.
 *   5. Write a boot event: "system boot".
 *   6. Sleep forever — events are written from other threads.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void event_log_thread_entry(void *p1, void *p2, void *p3);

#endif /* EVENT_LOG_H */
