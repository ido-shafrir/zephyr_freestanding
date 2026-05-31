/**
 * @file main.c
 * @brief ztest suite for time_service (ISO 8601 helpers, set/get, sync flag).
 *
 * Tests the pure-logic portions of time_service.c that don't require
 * actual SNTP networking:
 *   - ISO 8601 formatting and parsing
 *   - Manual clock set/get
 *   - Sync state tracking
 *
 * Platform: qemu_x86 (requires CONFIG_POSIX_TIMERS).
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <errno.h>
#include "time_service.h"
#include "stubs.h"

ZTEST_SUITE(time_service_tests, NULL, NULL, NULL, NULL, NULL);

/* ==========================================================================
 * ISO 8601 formatting
 * ========================================================================== */

ZTEST(time_service_tests, test_format_epoch_zero)
{
    char buf[21];
    int n = time_service_format_iso8601(0, buf, sizeof(buf));
    zassert_true(n > 0, "format returned %d", n);
    zassert_str_equal(buf, "1970-01-01T00:00:00Z");
}

ZTEST(time_service_tests, test_format_known_date)
{
    /* 2024-06-10T20:00:00Z = 1718049600 */
    char buf[21];
    int n = time_service_format_iso8601(1718049600, buf, sizeof(buf));
    zassert_true(n > 0, "format returned %d", n);
    zassert_str_equal(buf, "2024-06-10T20:00:00Z");
}

ZTEST(time_service_tests, test_format_leap_year)
{
    /* 2024-02-29T12:00:00Z = 1709208000 */
    char buf[21];
    int n = time_service_format_iso8601(1709208000, buf, sizeof(buf));
    zassert_true(n > 0, "format returned %d", n);
    zassert_str_equal(buf, "2024-02-29T12:00:00Z");
}

ZTEST(time_service_tests, test_format_y2k)
{
    /* 2000-01-01T00:00:00Z = 946684800 */
    char buf[21];
    int n = time_service_format_iso8601(946684800, buf, sizeof(buf));
    zassert_true(n > 0, "format returned %d", n);
    zassert_str_equal(buf, "2000-01-01T00:00:00Z");
}

ZTEST(time_service_tests, test_format_buffer_too_small)
{
    char buf[10];
    int rc = time_service_format_iso8601(0, buf, sizeof(buf));
    zassert_equal(rc, -ENOMEM, "expected -ENOMEM, got %d", rc);
}

/* ==========================================================================
 * ISO 8601 parsing
 * ========================================================================== */

ZTEST(time_service_tests, test_parse_epoch_zero)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("1970-01-01T00:00:00Z", &ts);
    zassert_equal(rc, 0, "parse failed: %d", rc);
    zassert_equal(ts, 0);
}

ZTEST(time_service_tests, test_parse_known_date)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-10T20:00:00Z", &ts);
    zassert_equal(rc, 0, "parse failed: %d", rc);
    zassert_equal(ts, 1718049600);
}

ZTEST(time_service_tests, test_parse_roundtrip)
{
    int64_t original = 1609459200; /* 2021-01-01T00:00:00Z */
    char buf[21];
    time_service_format_iso8601(original, buf, sizeof(buf));

    int64_t parsed;
    int rc = time_service_parse_iso8601(buf, &parsed);
    zassert_equal(rc, 0, "roundtrip parse failed: %d", rc);
    zassert_equal(parsed, original,
                  "roundtrip mismatch: %lld != %lld",
                  (long long)parsed, (long long)original);
}

ZTEST(time_service_tests, test_parse_invalid_format)
{
    int64_t ts;
    zassert_equal(time_service_parse_iso8601("not-a-date", &ts), -EINVAL);
    zassert_equal(time_service_parse_iso8601("2024/06/10 20:00:00", &ts), -EINVAL);
    zassert_equal(time_service_parse_iso8601("", &ts), -EINVAL);
}

ZTEST(time_service_tests, test_parse_null_args)
{
    int64_t ts;
    zassert_equal(time_service_parse_iso8601(NULL, &ts), -EINVAL);
    zassert_equal(time_service_parse_iso8601("1970-01-01T00:00:00Z", NULL), -EINVAL);
}

/* ==========================================================================
 * Manual clock set / get / sync state
 * ========================================================================== */

ZTEST(time_service_tests, test_set_and_get)
{
    int64_t epoch = 1718049600; /* 2024-06-10T20:00:00Z */
    int rc = time_service_set(epoch);
    zassert_equal(rc, 0, "set failed: %d", rc);

    int64_t got = time_service_get();
    /* Allow 1 second tolerance for test execution time */
    zassert_true(got >= epoch && got <= epoch + 2,
                 "expected ~%lld, got %lld", (long long)epoch, (long long)got);
}

ZTEST(time_service_tests, test_synced_after_set)
{
    int rc = time_service_set(1000000);
    zassert_equal(rc, 0);
    zassert_true(time_service_is_synced(), "should be synced after set");
}

ZTEST(time_service_tests, test_sync_trigger_does_not_crash)
{
    /* Just verify the API doesn't crash — actual SNTP won't work in test */
    time_service_sync();
}

/* ==========================================================================
 * Leap year edge cases
 * ========================================================================== */

ZTEST(time_service_tests, test_parse_feb29_leap)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-02-29T00:00:00Z", &ts);
    zassert_equal(rc, 0, "leap year feb 29 should parse");
}

ZTEST(time_service_tests, test_parse_dec31)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2023-12-31T23:59:59Z", &ts);
    zassert_equal(rc, 0, "dec 31 should parse");

    char buf[21];
    time_service_format_iso8601(ts, buf, sizeof(buf));
    zassert_str_equal(buf, "2023-12-31T23:59:59Z");
}

/* ==========================================================================
 * Bad-path parsing — range / format errors
 * ========================================================================== */

ZTEST(time_service_tests, test_parse_invalid_month_zero)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-00-15T12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "month 0 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_month_13)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-13-15T12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "month 13 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_day_zero)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-00T12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "day 0 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_day_32)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-32T12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "day 32 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_hour)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T24:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "hour 24 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_minute)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T12:60:00Z", &ts);
    zassert_equal(rc, -EINVAL, "minute 60 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_invalid_second)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T12:00:60Z", &ts);
    zassert_equal(rc, -EINVAL, "second 60 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_year_before_1970)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("1969-12-31T23:59:59Z", &ts);
    zassert_equal(rc, -EINVAL, "years < 1970 should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_string_too_short)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T12:00:0Z", &ts);
    zassert_equal(rc, -EINVAL, "19-char string should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_string_too_long)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T12:00:00Z ", &ts);
    zassert_equal(rc, -EINVAL, "21-char string should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_missing_z_suffix)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15T12:00:00.", &ts);
    zassert_equal(rc, -EINVAL, "missing 'Z' should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_missing_t_separator)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024-06-15 12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "missing 'T' should be rejected, got %d", rc);
}

ZTEST(time_service_tests, test_parse_wrong_dash_positions)
{
    int64_t ts;
    int rc = time_service_parse_iso8601("2024/06/15T12:00:00Z", &ts);
    zassert_equal(rc, -EINVAL, "slashes should be rejected, got %d", rc);
}

/* ==========================================================================
 * Bad-path formatting
 * ========================================================================== */

ZTEST(time_service_tests, test_format_buffer_exactly_20)
{
    /* Needs 21 bytes (20 + NUL); 20 is too small. */
    char buf[20];
    int rc = time_service_format_iso8601(0, buf, sizeof(buf));
    zassert_equal(rc, -ENOMEM, "buffer 20 should be too small, got %d", rc);
}

ZTEST(time_service_tests, test_format_buffer_zero)
{
    char buf[1];
    int rc = time_service_format_iso8601(0, buf, 0);
    zassert_equal(rc, -ENOMEM, "zero-length buffer should fail, got %d", rc);
}

/* ==========================================================================
 * init
 * ========================================================================== */

ZTEST(time_service_tests, test_init_returns_ok)
{
    int rc = time_service_init();
    zassert_equal(rc, 0, "init returned %d", rc);
}

ZTEST(time_service_tests, test_init_resets_sync_flag)
{
    /* Force synced=true via manual set, then init clears it. */
    time_service_set(1000000);
    zassert_true(time_service_is_synced(),
                 "should be synced after manual set");
    time_service_init();
    zassert_false(time_service_is_synced(),
                  "init should clear the synced flag");
}

/* ==========================================================================
 * Fallback clock at init
 * ==========================================================================
 *
 * After init, CLOCK_REALTIME should be seeded with a plausible
 * placeholder (2025-01-01T00:00:00Z) so wall-clock consumers don't
 * have to special-case "not yet synced".  The sync flag remains false.
 */

ZTEST(time_service_tests, test_init_seeds_fallback_clock)
{
    time_service_init();

    int64_t now = time_service_get();
    /* Fallback epoch is 1735689600 (2025-01-01T00:00:00Z).  Allow a
     * little slack for test execution time.
     */
    zassert_true(now >= 1735689600 && now < 1735689600 + 10,
                 "expected fallback clock near 1735689600, got %lld",
                 (long long)now);
    zassert_false(time_service_is_synced(),
                  "fallback clock must NOT mark the service as synced");
}

ZTEST(time_service_tests, test_init_formats_fallback_as_2025)
{
    time_service_init();

    int64_t now = time_service_get();
    char iso[21];
    int n = time_service_format_iso8601(now, iso, sizeof(iso));
    zassert_true(n > 0);
    zassert_true(strncmp(iso, "2025-01-01T", 11) == 0,
                 "fallback should format as 2025-01-01, got '%s'", iso);
}

/* ==========================================================================
 * Empty NTP server is a valid configuration
 * ==========================================================================
 *
 * When `mcu/ntpServer` is empty, `time_service_try_sync_now()` must:
 *   - return 0 (success)
 *   - NOT contact any SNTP server
 *   - NOT touch CLOCK_REALTIME
 *   - leave `time_service_is_synced()` unchanged (false)
 *
 * This lets deployments without NTP set the clock manually (UART,
 * REST) or via keepalive responses, without blocking OTA readiness.
 */

ZTEST(time_service_tests, test_try_sync_now_empty_server_returns_ok)
{
    time_service_init();
    stub_set_ntp_server("");

    int rc = time_service_try_sync_now();
    zassert_equal(rc, 0,
                  "empty NTP server should return 0 (skip), got %d", rc);
}

ZTEST(time_service_tests, test_try_sync_now_empty_server_not_synced)
{
    time_service_init();
    stub_set_ntp_server("");

    (void)time_service_try_sync_now();
    zassert_false(time_service_is_synced(),
                  "sync flag must remain false when sync is skipped");
}

ZTEST(time_service_tests, test_try_sync_now_empty_server_keeps_fallback)
{
    time_service_init();
    stub_set_ntp_server("");

    int64_t before = time_service_get();
    (void)time_service_try_sync_now();
    int64_t after = time_service_get();

    /* Clock must NOT have been overwritten — tolerate 2s elapsed. */
    zassert_true(after >= before && after <= before + 2,
                 "clock changed unexpectedly: before=%lld after=%lld",
                 (long long)before, (long long)after);
}

ZTEST(time_service_tests, test_try_sync_now_configured_server_fails_in_test)
{
    /* The sntp_simple stub always returns -ETIMEDOUT.  With a
     * non-empty server configured, do_sntp_sync propagates that error
     * and must NOT mark the service synced.
     */
    time_service_init();
    stub_set_ntp_server("216.239.35.0");

    int rc = time_service_try_sync_now();
    zassert_equal(rc, -ETIMEDOUT,
                  "stub should force -ETIMEDOUT, got %d", rc);
    zassert_false(time_service_is_synced(),
                  "failed sync must leave synced=false");
}

ZTEST(time_service_tests, test_manual_set_still_works_with_empty_server)
{
    /* Even with NTP disabled, manual set must sync the clock. */
    time_service_init();
    stub_set_ntp_server("");

    int rc = time_service_set(1718049600); /* 2024-06-10T20:00:00Z */
    zassert_equal(rc, 0);
    zassert_true(time_service_is_synced(),
                 "manual set must mark the service as synced");

    int64_t now = time_service_get();
    zassert_true(now >= 1718049600 && now <= 1718049600 + 2,
                 "expected manual epoch, got %lld", (long long)now);
}
