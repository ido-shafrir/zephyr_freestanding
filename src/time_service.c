/**
 * @file time_service.c
 * @brief Wall-clock time service — SNTP sync + POSIX clock.
 *
 * Periodically queries the configured NTP server via sntp_simple(),
 * then applies the result with clock_settime(CLOCK_REALTIME).
 * Exposes time_service_get() for other modules to read the wall clock.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "time_service.h"
#include "config_store.h"
#include "ota.h"
#include "event_log.h"

LOG_MODULE_REGISTER(time_service, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(time_service_stack, TIME_SERVICE_STACK_SIZE);
struct k_thread time_service_thread_data;

/* ---------- Sync semaphore (given to trigger immediate re-sync) ---------- */
static K_SEM_DEFINE(sync_sem, 0, 1);

/* ---------- Sync state ---------- */
static volatile bool synced;

/* ---------- SNTP timeout (ms) ---------- */
#define SNTP_TIMEOUT_MS  5000

/* ---------- Fallback wall clock applied at init ----------
 * Used so wall_clock stamps and ISO-8601 formatters produce a
 * plausible date even before the first successful sync or manual
 * time_service_set().  Picked as 2025-01-01T00:00:00Z so the year
 * is obviously "placeholder / not synced" when seen in logs.
 */
#define TIME_SERVICE_FALLBACK_EPOCH  1735689600LL  /* 2025-01-01T00:00:00Z */

/* ================================================================
 * ISO 8601 helpers (no strptime on Zephyr)
 * ================================================================ */

/** Days in each month (non-leap). */
static const uint8_t days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/**
 * @brief Convert broken-down UTC date/time to UNIX epoch seconds.
 *
 * @param year  Full year (e.g. 2024).
 * @param month 1-12.
 * @param day   1-31.
 * @param hour  0-23.
 * @param min   0-59.
 * @param sec   0-59.
 * @return UNIX timestamp, or -1 on range error.
 */
static int64_t ymdhms_to_epoch(int year, int month, int day,
                                int hour, int min, int sec)
{
    if (year < 1970 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        return -1;
    }

    int64_t days = 0;

    /* Years -> days */
    for (int y = 1970; y < year; y++) {
        days += is_leap(y) ? 366 : 365;
    }
    /* Months -> days */
    for (int m = 1; m < month; m++) {
        days += days_in_month[m - 1];
        if (m == 2 && is_leap(year)) {
            days += 1;
        }
    }
    /* Days */
    days += day - 1;

    return days * 86400LL + hour * 3600LL + min * 60LL + sec;
}

/**
 * @brief Break a UNIX timestamp into UTC date/time components.
 */
static void epoch_to_ymdhms(int64_t epoch, int *year, int *month, int *day,
                             int *hour, int *min, int *sec)
{
    int64_t rem = epoch;
    int y = 1970;

    /* Find year */
    while (1) {
        int64_t yd = is_leap(y) ? 366 : 365;
        if (rem < yd * 86400LL) break;
        rem -= yd * 86400LL;
        y++;
    }
    *year = y;

    /* Find month */
    int m = 1;
    while (m <= 12) {
        int md = days_in_month[m - 1];
        if (m == 2 && is_leap(y)) md++;
        if (rem < (int64_t)md * 86400LL) break;
        rem -= (int64_t)md * 86400LL;
        m++;
    }
    *month = m;

    *day = (int)(rem / 86400LL) + 1;
    rem %= 86400LL;
    *hour = (int)(rem / 3600LL);
    rem %= 3600LL;
    *min = (int)(rem / 60LL);
    *sec = (int)(rem % 60LL);
}

int time_service_format_iso8601(int64_t epoch_sec, char *buf, size_t buf_len)
{
    if (buf_len < 21) return -ENOMEM;

    int y, mo, d, h, mi, s;
    epoch_to_ymdhms(epoch_sec, &y, &mo, &d, &h, &mi, &s);

    int n = snprintf(buf, buf_len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     y, mo, d, h, mi, s);
    return n;
}

int time_service_parse_iso8601(const char *str, int64_t *epoch_sec)
{
    if (str == NULL || epoch_sec == NULL) return -EINVAL;

    /* Expect exactly "YYYY-MM-DDTHH:MM:SSZ" (20 chars) */
    size_t len = strlen(str);
    if (len != 20) return -EINVAL;
    if (str[4] != '-' || str[7] != '-' || str[10] != 'T' ||
        str[13] != ':' || str[16] != ':' || str[19] != 'Z') {
        return -EINVAL;
    }

    /* Parse digits manually (no sscanf on some Zephyr configs) */
    int year  = (str[0]-'0')*1000 + (str[1]-'0')*100 + (str[2]-'0')*10 + (str[3]-'0');
    int month = (str[5]-'0')*10 + (str[6]-'0');
    int day   = (str[8]-'0')*10 + (str[9]-'0');
    int hour  = (str[11]-'0')*10 + (str[12]-'0');
    int min   = (str[14]-'0')*10 + (str[15]-'0');
    int sec   = (str[17]-'0')*10 + (str[18]-'0');

    int64_t ts = ymdhms_to_epoch(year, month, day, hour, min, sec);
    if (ts < 0) return -EINVAL;

    *epoch_sec = ts;
    return 0;
}

/* ================================================================
 * Public API
 * ================================================================ */

int64_t time_service_get(void)
{
    /* CLOCK_REALTIME is always valid: seeded with the fallback epoch
     * at init, then updated by SNTP or manual set.  Callers that need
     * to distinguish "real sync" vs. "fallback placeholder" should
     * use time_service_is_synced().
     */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

int time_service_set(int64_t epoch_sec)
{
    struct timespec ts = {
        .tv_sec  = (time_t)epoch_sec,
        .tv_nsec = 0,
    };
    int rc = clock_settime(CLOCK_REALTIME, &ts);
    if (rc == 0) {
        synced = true;
        LOG_INF("Wall clock set to %lld (manual)", (long long)epoch_sec);
    }
    return rc;
}

void time_service_sync(void)
{
    k_sem_give(&sync_sem);
}

bool time_service_is_synced(void)
{
    return synced;
}

/* ================================================================
 * Public: perform one SNTP sync synchronously
 * ================================================================ */

int time_service_try_sync_now(void)
{
    char server[CONFIG_STORE_NTP_SERVER_MAX_LEN];
    config_store_get_ntp_server(server, sizeof(server));

    if (server[0] == '\0') {
        /* Empty NTP server is a valid configuration — the user may
         * set the clock manually via UART/REST or receive time via a
         * keepalive response.  Report success so the module is still
         * considered ready for OTA health purposes.
         */
        LOG_DBG("NTP server not configured - skipping sync");
        event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM,
                        "sntp skip: no server configured");
        return 0;
    }

    struct sntp_time sntp_ts;
    int rc = sntp_simple(server, SNTP_TIMEOUT_MS, &sntp_ts);
    if (rc != 0) {
        LOG_WRN("SNTP sync failed: %d (server=%s)", rc, server);
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "sntp sync failed rc=%d server=%.40s", rc, server);
        event_log_write(EVENT_SEV_WARN, EVENT_TYPE_SYSTEM, msg);
        return rc;
    }

    struct timespec tspec = {
        .tv_sec  = (time_t)sntp_ts.seconds,
        .tv_nsec = 0,
    };
    rc = clock_settime(CLOCK_REALTIME, &tspec);
    if (rc != 0) {
        LOG_ERR("clock_settime failed: %d", rc);
        char msg[48];
        snprintf(msg, sizeof(msg),
                 "clock_settime failed rc=%d", rc);
        event_log_write(EVENT_SEV_WARN, EVENT_TYPE_SYSTEM, msg);
        return rc;
    }

    bool was_synced = synced;
    synced = true;

    char iso[21];
    time_service_format_iso8601((int64_t)sntp_ts.seconds, iso, sizeof(iso));
    LOG_INF("SNTP sync OK: %s (server=%s)", iso, server);

    /* DEBUG event on every successful clock sync. */
    char dbg_msg[48];
    snprintf(dbg_msg, sizeof(dbg_msg), "sntp sync ok: %s", iso);
    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, dbg_msg);

    /* INFO event only on the very first successful sync. */
    if (!was_synced) {
        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM,
                        "time synced via SNTP");
    }

    return 0;
}

/* ================================================================
 * Init + thread entry
 * ================================================================ */

int time_service_init(void)
{
    synced = false;
    k_sem_reset(&sync_sem);

    /* Seed CLOCK_REALTIME with a plausible fallback date so wall_clock
     * stamps and ISO-8601 formatters produce a recognisable
     * "placeholder" value (2025-01-01) before the first real sync.
     * time_service_is_synced() remains false until a real SNTP sync or
     * an explicit time_service_set() call succeeds.
     */
    struct timespec fallback = {
        .tv_sec  = (time_t)TIME_SERVICE_FALLBACK_EPOCH,
        .tv_nsec = 0,
    };
    (void)clock_settime(CLOCK_REALTIME, &fallback);

    LOG_DBG("Time service initialised (fallback clock=2025-01-01T00:00:00Z)");
    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM,
                    "time service initialised");
    return 0;
}

void time_service_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    int rc = time_service_init();

    LOG_INF("Time service starting...");

    /* Report ready to OTA as soon as init succeeds.  The SNTP sync is
     * best-effort and must not gate the image-confirm health check:
     * the network stack may not have a DHCP lease yet on first boot,
     * and an upstream NTP outage should never revert a healthy image.
     * The wall clock is already seeded with the fallback epoch, so
     * other modules can read a plausible time immediately.
     */
    if (rc == 0) {
        ota_report_module_ready(OTA_MODULE_TIME_SERVICE);
    }

    /* Wait a few seconds for network to come up before the first sync
     * attempt.  This is only a best-effort delay — real readiness
     * depends on DHCP, which may take longer.  If the first sync
     * fails, the periodic loop below will retry.
     */
    k_sleep(K_SECONDS(5));

    /* Initial sync attempt.  Failures here are non-fatal: OTA has
     * already been notified, and the periodic loop will keep trying.
     */
    (void)time_service_try_sync_now();

    while (1) {
        uint32_t interval = config_store_get_ntp_sync_interval();

        /* Sleep for the sync interval, or wake early on trigger */
        (void)k_sem_take(&sync_sem, K_SECONDS(interval));

        /* Periodic sync is best-effort; OTA readiness is not
         * reported here because it was already reported after init.
         */
        (void)time_service_try_sync_now();
    }
}
