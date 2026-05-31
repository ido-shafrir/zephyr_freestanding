#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

/**
 * @file time_service.h
 * @brief Wall-clock time service using SNTP and POSIX clocks.
 *
 * Periodically syncs with a configured NTP server using SNTP,
 * then sets CLOCK_REALTIME via clock_settime(). Other modules
 * can read the wall clock with time_service_get().
 *
 * Before the first real sync, the clock is seeded at init with a
 * fallback placeholder (2025-01-01T00:00:00Z) so downstream
 * formatters always produce a readable date.  Callers that need to
 * know whether the clock is "real" should use
 * time_service_is_synced() — a bare time_service_get() always
 * returns a valid epoch value.
 *
 * Architecture:
 *   - A background thread runs the periodic sync loop.
 *   - An external trigger (semaphore) allows immediate re-sync
 *     on network reconnect.
 *   - Manual set via time_service_set() overrides the clock and
 *     marks the service as synced.
 *   - An empty NTP server string is a valid configuration that
 *     means "clock will be set manually" — sync attempts skip
 *     silently and the module still reports ready to OTA.
 */

#include <zephyr/kernel.h>
#include <stdint.h>

/* ---------- Thread configuration ---------- */
#define TIME_SERVICE_STACK_SIZE  2048
#define TIME_SERVICE_PRIORITY    10

/* ---------- Thread resources (defined in time_service.c) ---------- */
extern struct k_thread time_service_thread_data;
extern k_thread_stack_t time_service_stack[];

/* ---------- Module API ---------- */

/**
 * @brief Initialise the time service module.
 *
 * Resets internal state (sync flag, re-sync semaphore), seeds
 * `CLOCK_REALTIME` with a placeholder fallback date
 * (`2025-01-01T00:00:00Z`) so wall-clock stamps are readable before
 * the first real sync, and emits a DEBUG event-log entry
 * (`"time service initialised"`).
 *
 * `time_service_is_synced()` stays `false` until a real SNTP sync or
 * an explicit `time_service_set()` call succeeds.  Called from
 * @ref time_service_thread_entry immediately after the thread starts
 * and also safely callable from unit tests that don't spawn the thread.
 *
 * Idempotent — calling it a second time re-seeds the fallback clock
 * and re-arms internal state.
 *
 * @return 0 on success (cannot currently fail).
 *
 * @par Example
 * @code
 * time_service_init();
 * // time_service_is_synced() == false
 * // time_service_get()       == ~1735689600 (2025-01-01 placeholder)
 * @endcode
 */
int time_service_init(void);

/**
 * @brief Time service thread entry point.
 *
 * Calls @ref time_service_init once, waits briefly for the network,
 * performs an initial SNTP sync, then loops forever at the configured
 * interval (or on external @ref time_service_sync trigger).
 * Created by main() via k_thread_create().
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void time_service_thread_entry(void *p1, void *p2, void *p3);

/**
 * @brief Get the current wall-clock time as a UNIX timestamp.
 *
 * Always returns a valid epoch value.  Before the first real sync
 * the value is the fallback placeholder seeded at init
 * (2025-01-01T00:00:00Z plus elapsed time).  After a successful
 * SNTP sync or manual @ref time_service_set it reflects the true
 * wall-clock.
 *
 * @return UNIX timestamp (seconds since 1970-01-01T00:00:00Z).
 *
 * @par Example
 * @code
 * int64_t now = time_service_get();
 * // If synced:  now == 1718049600  (real epoch)
 * // Else:       now ~= 1735689600  (2025-01-01 placeholder + uptime)
 * @endcode
 */
int64_t time_service_get(void);

/**
 * @brief Manually set the wall clock to a UNIX timestamp.
 *
 * Sets CLOCK_REALTIME and marks the service as synced.
 *
 * @param epoch_sec UNIX timestamp in seconds.
 * @return 0 on success, negative errno on failure.
 *
 * @par Example
 * @code
 * time_service_set(1718000000);  // 2024-06-10T...
 * // time_service_is_synced() == true
 * @endcode
 */
int time_service_set(int64_t epoch_sec);

/**
 * @brief Trigger an immediate SNTP sync.
 *
 * Gives the internal semaphore so the sync thread wakes up
 * and re-syncs without waiting for the next interval tick.
 * Non-blocking, ISR-safe.
 *
 * @par Example
 * @code
 * time_service_sync();
 * // The background thread will perform SNTP sync shortly
 * @endcode
 */
void time_service_sync(void);

/**
 * @brief Run one SNTP sync attempt synchronously on the caller's thread.
 *
 * Unlike @ref time_service_sync (which only signals the background
 * thread), this performs the full SNTP exchange inline and returns
 * the result.  Used mainly by tests and by callers that need the
 * outcome before continuing.
 *
 * If the NTP server string in the config store is empty, the call
 * returns **0** immediately without contacting any server — an empty
 * server is a valid configuration that means "clock will be set
 * manually".
 *
 * @return 0 on success (including the "empty server → skip" case),
 *         negative errno on SNTP or `clock_settime` failure.
 *
 * @par Example
 * @code
 * int rc = time_service_try_sync_now();
 * // rc == 0          → synced (or skipped because server is empty)
 * // rc == -ETIMEDOUT → SNTP request timed out
 * @endcode
 */
int time_service_try_sync_now(void);

/**
 * @brief Check whether the wall clock has been synced.
 *
 * @return true if clock_settime has been called at least once
 *         (via SNTP or manual set), false otherwise.
 *
 * @par Example
 * @code
 * if (time_service_is_synced()) {
 *     // wall clock is trustworthy
 * }
 * @endcode
 */
bool time_service_is_synced(void);

/**
 * @brief Format a UNIX timestamp as ISO 8601 into a buffer.
 *
 * Output format: "YYYY-MM-DDTHH:MM:SSZ" (always UTC, 20 chars + NUL).
 *
 * @param epoch_sec UNIX timestamp.
 * @param buf       Output buffer (must be >= 21 bytes).
 * @param buf_len   Size of @p buf.
 * @return Number of chars written (excluding NUL), or -ENOMEM if too small.
 *
 * @par Example
 * @code
 * char iso[21];
 * time_service_format_iso8601(1718049600, iso, sizeof(iso));
 * // iso == "2024-06-10T20:00:00Z"
 * @endcode
 */
int time_service_format_iso8601(int64_t epoch_sec, char *buf, size_t buf_len);

/**
 * @brief Parse an ISO 8601 UTC string into a UNIX timestamp.
 *
 * Accepts "YYYY-MM-DDTHH:MM:SSZ" format only.
 *
 * @param str       ISO 8601 string.
 * @param epoch_sec Output UNIX timestamp.
 * @return 0 on success, -EINVAL on parse error.
 *
 * @par Example
 * @code
 * int64_t ts;
 * time_service_parse_iso8601("2024-06-10T20:00:00Z", &ts);
 * // ts == 1718049600
 *
 * time_service_parse_iso8601("invalid", &ts);
 * // returns -EINVAL
 * @endcode
 */
int time_service_parse_iso8601(const char *str, int64_t *epoch_sec);

#endif /* TIME_SERVICE_H */
