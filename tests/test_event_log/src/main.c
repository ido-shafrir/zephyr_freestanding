/**
 * @file main.c
 * @brief ztest suite for the event_log module (FCB-backed).
 *
 * Runs on native_sim which provides a simulated flash device,
 * allowing real FCB operations without hardware.
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdio.h>
#include "event_log.h"

/* ---------- Walk callback helpers ---------- */

struct collect_ctx {
    event_entry_t entries[64];
    int count;
};

static bool collect_cb(const event_entry_t *entry, void *user_data)
{
    struct collect_ctx *ctx = user_data;
    if (ctx->count < 64) {
        ctx->entries[ctx->count++] = *entry;
    }
    return true;
}

static bool count_cb(const event_entry_t *entry, void *user_data)
{
    int *count = user_data;
    (*count)++;
    return true;
}

static bool stop_after_one_cb(const event_entry_t *entry, void *user_data)
{
    int *count = user_data;
    (*count)++;
    return false; /* stop after first */
}

/* ================================================================
 * Suite: init
 * ================================================================ */

ZTEST_SUITE(event_log_init_suite, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_log_init_suite, test_init_succeeds)
{
    int rc = event_log_init();
    zassert_equal(rc, 0, "event_log_init failed: %d", rc);
}

ZTEST(event_log_init_suite, test_init_idempotent)
{
    int rc1 = event_log_init();
    int rc2 = event_log_init();
    zassert_equal(rc1, 0);
    zassert_equal(rc2, 0, "Second init should be no-op");
}

/* ================================================================
 * Suite: write — tests depend on init having been called
 * ================================================================ */

static void *write_setup(void)
{
    event_log_init();
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
    return NULL;
}

ZTEST_SUITE(event_log_write_suite, NULL, write_setup, NULL, NULL, NULL);

ZTEST(event_log_write_suite, test_write_and_read_one)
{
    int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "test boot");
    zassert_equal(rc, 0);

    struct collect_ctx ctx = { .count = 0 };
    int n = event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_true(n >= 1, "expected at least 1 entry, got %d", n);
    zassert_equal(ctx.entries[0].event_type, EVENT_TYPE_BOOT);
    zassert_equal(ctx.entries[0].severity, EVENT_SEV_INFO);
    zassert_true(strncmp(ctx.entries[0].message, "test boot", 9) == 0);
}

ZTEST(event_log_write_suite, test_write_multiple)
{
    event_log_write(EVENT_SEV_ERR, EVENT_TYPE_OTA, "err1");
    event_log_write(EVENT_SEV_WARN, EVENT_TYPE_NETWORK, "warn1");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_CONFIG, "info1");

    int count = 0;
    int n = event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_true(n >= 3, "expected >= 3 entries, got %d", n);
}

ZTEST(event_log_write_suite, test_write_truncates_long_message)
{
    char long_msg[128];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, long_msg);
    zassert_equal(rc, 0);

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_true(ctx.count >= 1);
    zassert_equal(ctx.entries[ctx.count - 1].data_len, EVENT_LOG_MSG_MAX_LEN,
                  "message should be truncated to %d", EVENT_LOG_MSG_MAX_LEN);
}

ZTEST(event_log_write_suite, test_write_null_message)
{
    int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, NULL);
    zassert_equal(rc, 0);

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_true(ctx.count >= 1);
    zassert_equal(ctx.entries[ctx.count - 1].data_len, 0);
}

ZTEST(event_log_write_suite, test_write_empty_message)
{
    int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "");
    zassert_equal(rc, 0);

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_true(ctx.count >= 1);
    zassert_equal(ctx.entries[ctx.count - 1].data_len, 0);
}

/* ================================================================
 * Suite: read
 * ================================================================ */

static void *read_setup(void)
{
    event_log_init();
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
    return NULL;
}

static void read_before(void *fixture)
{
    ARG_UNUSED(fixture);
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
}

ZTEST_SUITE(event_log_read_suite, NULL, read_setup, read_before, NULL, NULL);

ZTEST(event_log_read_suite, test_read_empty_log)
{
    int count = 0;
    int n = event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(n, 0, "empty log should return 0 entries, got %d", n);
}

ZTEST(event_log_read_suite, test_read_null_callback)
{
    int rc = event_log_read(0, 0, 0, false, NULL, NULL);
    zassert_equal(rc, -EINVAL);
}

ZTEST(event_log_read_suite, test_read_stop_early)
{
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "a");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "b");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "c");

    int count = 0;
    event_log_read(0, 0, 0, false, stop_after_one_cb, &count);
    zassert_equal(count, 1, "callback should have been called exactly once");
}

ZTEST(event_log_read_suite, test_read_since_filter)
{
    /* Write two entries, the timestamp will be the same (uptime ~0 in test) */
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "early");

    /* Since the test runs in milliseconds, use a large since_epoch to filter */
    int count = 0;
    int n = event_log_read(999999, 999999, 0, false, count_cb, &count);
    zassert_equal(n, 0, "no entries should match a far-future timestamp");
}

/**
 * @brief boot_id filter matches only entries stamped with the current
 *        boot's id (see bug report #019).
 *
 * The stub config_store increments bootCount each time event_log_init()
 * runs. With the log cleared at suite setup, we have a fresh run: every
 * new write gets stamped with event_log_get_boot_id() (== 1 here, after
 * one init pass). Querying with boot_id = current id returns all our
 * writes; querying with a different id returns zero.
 */
ZTEST(event_log_read_suite, test_read_boot_id_filter)
{
    uint32_t bid = event_log_get_boot_id();
    zassert_true(bid > 0, "boot_id should be >=1 after init, got %u", bid);

    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "b1-a");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "b1-b");

    int count_all = 0;
    event_log_read(0, 0, 0, false, count_cb, &count_all);
    zassert_true(count_all >= 2, "all-boots filter saw %d, want >=2", count_all);

    int count_current = 0;
    event_log_read(0, 0, bid, false, count_cb, &count_current);
    zassert_equal(count_current, count_all,
                  "current boot_id should see the same set as no filter");

    int count_other = 0;
    event_log_read(0, 0, bid + 999, false, count_cb, &count_other);
    zassert_equal(count_other, 0,
                  "non-matching boot_id must match nothing, got %d",
                  count_other);
}

/**
 * @brief boot_id field is populated on every written entry.
 */
ZTEST(event_log_read_suite, test_entry_carries_boot_id)
{
    uint32_t bid = event_log_get_boot_id();
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "tagged");

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, bid, false, collect_cb, &ctx);
    zassert_true(ctx.count >= 1, "expected entry, got %d", ctx.count);
    zassert_equal(ctx.entries[ctx.count - 1].boot_id, bid,
                  "entry boot_id=%u expected %u",
                  ctx.entries[ctx.count - 1].boot_id, bid);
}

/* Exposed by stubs.c \u2014 drives time_service_is_synced()/get() */
void test_time_service_set(bool synced, int64_t epoch);

/**
 * @brief In default (non-strict) mode, sync'd entries bypass the
 *        boot_id filter so `event_log 300` picks up prior-boot
 *        sync'd entries that fall in the wall-clock window.
 *        Pre-sync entries from other boots are still filtered.
 *
 * Simulates three entries that look like they came from two different
 * boots: one pre-sync (boot_id=bid), one sync'd "from boot 1"
 * (boot_id=bid but wall_clock=1_000_000), and one sync'd "from boot 2"
 * (we can't literally change boot_id on write, so this test uses the
 * wall_clock axis and checks emission across both modes).
 */
ZTEST(event_log_read_suite, test_nonstrict_sync_bypasses_boot_id)
{
    uint32_t bid = event_log_get_boot_id();

    /* Write a pre-sync entry (wall_clock=0). */
    test_time_service_set(false, 0);
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "pre-sync");

    /* Write a sync'd entry (wall_clock=1_000_100). */
    test_time_service_set(true, 1000100);
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "synced");

    /* Ask with a far-future wall cutoff and a bogus boot_id. In
     * non-strict mode the sync'd entry is filtered by the wall cutoff
     * (too high \u2192 dropped). */
    int c1 = 0;
    event_log_read(/*since_wall=*/1000500, 0, bid + 999, false, count_cb, &c1);
    zassert_equal(c1, 0, "non-strict + wall cutoff 1000500 should drop all, got %d", c1);

    /* Ask with a wall cutoff below the sync'd entry and a non-matching
     * boot_id. Sync'd entry should be emitted (boot_id bypassed);
     * pre-sync dropped because boot_id mismatch. */
    int c2 = 0;
    event_log_read(/*since_wall=*/1000000, 0, bid + 999, false, count_cb, &c2);
    zassert_equal(c2, 1,
                  "non-strict: sync'd entry should bypass boot_id, got %d", c2);

    /* Same query in strict mode: sync'd entry must also match boot_id,
     * so nothing survives. */
    int c3 = 0;
    event_log_read(1000000, 0, bid + 999, true, count_cb, &c3);
    zassert_equal(c3, 0, "strict: nothing should match bogus boot_id, got %d", c3);

    /* Restore stub for subsequent tests. */
    test_time_service_set(false, 0);
}

/* ================================================================
 * Suite: clear
 * ================================================================ */

static void *clear_setup(void)
{
    event_log_init();
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
    return NULL;
}

ZTEST_SUITE(event_log_clear_suite, NULL, clear_setup, NULL, NULL, NULL);

ZTEST(event_log_clear_suite, test_clear_empties_log)
{
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "to be cleared");
    event_log_write(EVENT_SEV_ERR, EVENT_TYPE_OTA, "also cleared");

    int rc = event_log_clear();
    zassert_equal(rc, 0);

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 0, "log should be empty after clear");
}

ZTEST(event_log_clear_suite, test_clear_then_write)
{
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "before");
    event_log_clear();
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "after");

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_equal(ctx.count, 1);
    zassert_true(strncmp(ctx.entries[0].message, "after", 5) == 0);
}

/* ================================================================
 * Suite: level (severity filter)
 * ================================================================ */

static void *level_setup(void)
{
    event_log_init();
    event_log_clear();
    return NULL;
}

static void level_before(void *fixture)
{
    ARG_UNUSED(fixture);
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
}

ZTEST_SUITE(event_log_level_suite, NULL, level_setup, level_before, NULL, NULL);

ZTEST(event_log_level_suite, test_set_get_level)
{
    event_log_set_level(EVENT_SEV_WARN);
    zassert_equal(event_log_get_level(), EVENT_SEV_WARN);

    event_log_set_level(EVENT_SEV_DEBUG);
    zassert_equal(event_log_get_level(), EVENT_SEV_DEBUG);
}

ZTEST(event_log_level_suite, test_filter_drops_debug_at_info)
{
    event_log_set_level(EVENT_SEV_INFO);
    event_log_clear(); /* flush the "log level changed" event */

    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "should be dropped");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "should be kept");

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_equal(ctx.count, 1, "only INFO should survive, got %d", ctx.count);
    zassert_true(strncmp(ctx.entries[0].message, "should be kept", 14) == 0);
}

ZTEST(event_log_level_suite, test_filter_drops_info_at_warn)
{
    event_log_set_level(EVENT_SEV_WARN);
    event_log_clear(); /* flush the "log level changed" event */

    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "dropped");
    event_log_write(EVENT_SEV_WARN, EVENT_TYPE_SYSTEM, "kept");
    event_log_write(EVENT_SEV_ERR, EVENT_TYPE_SYSTEM, "also kept");

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 2, "WARN+ERR should survive, got %d", count);
}

ZTEST(event_log_level_suite, test_filter_err_only)
{
    event_log_set_level(EVENT_SEV_ERR);
    event_log_clear(); /* flush the "log level changed" event */

    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "no");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "no");
    event_log_write(EVENT_SEV_WARN, EVENT_TYPE_SYSTEM, "no");
    event_log_write(EVENT_SEV_ERR, EVENT_TYPE_SYSTEM, "yes");

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 1, "only ERR should survive, got %d", count);
}

ZTEST(event_log_level_suite, test_filter_debug_passes_all)
{
    event_log_set_level(EVENT_SEV_DEBUG);

    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "d");
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "i");
    event_log_write(EVENT_SEV_WARN, EVENT_TYPE_SYSTEM, "w");
    event_log_write(EVENT_SEV_ERR, EVENT_TYPE_SYSTEM, "e");

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 4, "all 4 should pass at DEBUG, got %d", count);
}

/* ================================================================
 * Suite: string helpers
 * ================================================================ */

ZTEST_SUITE(event_log_strings_suite, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_log_strings_suite, test_severity_to_str)
{
    zassert_str_equal(severity_to_str(EVENT_SEV_EMERG), "EMERG");
    zassert_str_equal(severity_to_str(EVENT_SEV_ERR), "ERR");
    zassert_str_equal(severity_to_str(EVENT_SEV_WARN), "WARN");
    zassert_str_equal(severity_to_str(EVENT_SEV_INFO), "INFO");
    zassert_str_equal(severity_to_str(EVENT_SEV_DEBUG), "DEBUG");
}

ZTEST(event_log_strings_suite, test_severity_to_str_invalid)
{
    zassert_str_equal(severity_to_str((enum event_severity)99), "???");
}

ZTEST(event_log_strings_suite, test_event_type_to_str)
{
    zassert_str_equal(event_type_to_str(EVENT_TYPE_BOOT), "BOOT");
    zassert_str_equal(event_type_to_str(EVENT_TYPE_NETWORK), "NETWORK");
    zassert_str_equal(event_type_to_str(EVENT_TYPE_OTA), "OTA");
    zassert_str_equal(event_type_to_str(EVENT_TYPE_CONFIG), "CONFIG");
    zassert_str_equal(event_type_to_str(EVENT_TYPE_SYSTEM), "SYSTEM");
}

ZTEST(event_log_strings_suite, test_event_type_to_str_invalid)
{
    zassert_str_equal(event_type_to_str((enum event_type)99), "???");
}

/* ================================================================
 * Suite: rotation
 * ================================================================ */

static void *rotation_setup(void)
{
    event_log_init();
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
    return NULL;
}

static void rotation_before(void *fixture)
{
    ARG_UNUSED(fixture);
    event_log_clear();
}

ZTEST_SUITE(event_log_rotation_suite, NULL, rotation_setup, rotation_before,
            NULL, NULL);

/**
 * @brief Write enough entries to exceed sector capacity and trigger rotation.
 *
 * On qemu_x86 the test overlay defines a 3 KB partition (3 × 1024-byte
 * sectors).  With scratch_cnt=1, two sectors are usable.  Each 72-byte
 * packed entry plus FCB framing occupies roughly 80 bytes, so ~12 entries
 * fill one sector and ~24 fill both usable sectors.  Writing 40 entries
 * forces at least one fcb_rotate().
 *
 * Expectation: all 40 writes succeed (rc == 0) even though the partition
 * cannot hold them all simultaneously — rotation reclaims space.
 */
ZTEST(event_log_rotation_suite, test_writes_succeed_past_capacity)
{
    const int TOTAL = 40;
    char msg[32];

    for (int i = 0; i < TOTAL; i++) {
        snprintf(msg, sizeof(msg), "rot-%d", i);
        int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, msg);
        zassert_equal(rc, 0, "write %d failed: %d", i, rc);
    }
}

/**
 * @brief After rotation the newest entries survive while oldest are evicted.
 *
 * Write 40 entries ("rot-0" … "rot-39"), then read them all back.
 * The count should be less than 40 (oldest erased) but greater than 0,
 * and the *last* entry should be the most recent one written.
 */
ZTEST(event_log_rotation_suite, test_oldest_entries_evicted)
{
    const int TOTAL = 40;
    char msg[32];

    for (int i = 0; i < TOTAL; i++) {
        snprintf(msg, sizeof(msg), "rot-%d", i);
        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, msg);
    }

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);

    zassert_true(ctx.count > 0, "should have surviving entries");
    zassert_true(ctx.count < TOTAL,
                 "some entries should have been evicted, got %d", ctx.count);

    /* Last collected entry should be the most recently written */
    char expected[32];
    snprintf(expected, sizeof(expected), "rot-%d", TOTAL - 1);
    zassert_true(
        strncmp(ctx.entries[ctx.count - 1].message, expected,
                strlen(expected)) == 0,
        "newest entry should be '%s', got '%.32s'",
        expected, ctx.entries[ctx.count - 1].message);
}

/**
 * @brief Read still works correctly after multiple rotations.
 *
 * Perform two rounds of heavy writes (each triggers rotation), clear
 * in between, then verify the second round's data is readable.
 */
ZTEST(event_log_rotation_suite, test_read_after_multiple_rotations)
{
    /* First round — fill and rotate */
    for (int i = 0; i < 40; i++) {
        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "round1");
    }

    event_log_clear();

    /* Second round */
    for (int i = 0; i < 10; i++) {
        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, "round2");
    }

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 10,
                  "expected 10 entries from round2, got %d", count);
}

/* ================================================================
 * Suite: bad-path tests
 *
 * Exercise invalid inputs, edge-case severity/type enum values, and
 * repeated no-op operations.  None of these should crash or corrupt
 * the log.
 * ================================================================ */

static void *badpath_setup(void)
{
    event_log_init();
    event_log_clear();
    event_log_set_level(EVENT_SEV_DEBUG);
    return NULL;
}

static void badpath_before(void *fixture)
{
    ARG_UNUSED(fixture);
    event_log_set_level(EVENT_SEV_DEBUG);
    event_log_clear();
}

ZTEST_SUITE(event_log_badpath_suite, NULL, badpath_setup, badpath_before,
            NULL, NULL);

/**
 * @brief Reading with since_epoch = UINT32_MAX should match nothing.
 */
ZTEST(event_log_badpath_suite, test_read_since_uint32_max)
{
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "hello");

    int count = 0;
    int n = event_log_read(UINT32_MAX, UINT32_MAX, 0, false, count_cb, &count);
    zassert_equal(n, 0, "UINT32_MAX since should match nothing, got %d", n);
    zassert_equal(count, 0);
}

/**
 * @brief event_log_read with a NULL cb and since=0 returns -EINVAL
 *        even when there are entries.
 */
ZTEST(event_log_badpath_suite, test_read_null_cb_with_entries)
{
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, "not empty");
    int rc = event_log_read(0, 0, 0, false, NULL, NULL);
    zassert_equal(rc, -EINVAL, "NULL cb should return -EINVAL, got %d", rc);
}

/**
 * @brief Clearing an already-empty log is a no-op success.
 */
ZTEST(event_log_badpath_suite, test_clear_empty_log)
{
    int rc = event_log_clear();
    zassert_equal(rc, 0, "first clear returned %d", rc);
    rc = event_log_clear();
    zassert_equal(rc, 0, "second clear on empty log returned %d", rc);

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 0);
}

/**
 * @brief Writes with the highest (EMERG) and lowest (DEBUG) severities
 *        both succeed.
 */
ZTEST(event_log_badpath_suite, test_write_extreme_severities)
{
    zassert_equal(event_log_write(EVENT_SEV_EMERG, EVENT_TYPE_SYSTEM,
                                  "emerg!"), 0);
    zassert_equal(event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM,
                                  "dbg."), 0);

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 2, "both should be recorded, got %d", count);
}

/**
 * @brief Every valid event_type value writes successfully.
 */
ZTEST(event_log_badpath_suite, test_write_all_event_types)
{
    for (int t = 0; t < EVENT_TYPE_COUNT; t++) {
        int rc = event_log_write(EVENT_SEV_INFO, (enum event_type)t, "x");
        zassert_equal(rc, 0, "type %d failed rc=%d", t, rc);
    }

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, EVENT_TYPE_COUNT,
                  "expected %d entries, got %d", EVENT_TYPE_COUNT, count);
}

/**
 * @brief Setting the filter level to the same value twice is idempotent.
 */
ZTEST(event_log_badpath_suite, test_set_level_idempotent)
{
    event_log_set_level(EVENT_SEV_WARN);
    event_log_set_level(EVENT_SEV_WARN);
    zassert_equal(event_log_get_level(), EVENT_SEV_WARN);
}

/**
 * @brief Messages at exactly EVENT_LOG_MSG_MAX_LEN bytes (no NUL room)
 *        are truncated to EVENT_LOG_MSG_MAX_LEN and stored intact.
 */
ZTEST(event_log_badpath_suite, test_write_exactly_max_len)
{
    char msg[EVENT_LOG_MSG_MAX_LEN + 1];
    memset(msg, 'B', EVENT_LOG_MSG_MAX_LEN);
    msg[EVENT_LOG_MSG_MAX_LEN] = '\0';

    int rc = event_log_write(EVENT_SEV_INFO, EVENT_TYPE_SYSTEM, msg);
    zassert_equal(rc, 0);

    struct collect_ctx ctx = { .count = 0 };
    event_log_read(0, 0, 0, false, collect_cb, &ctx);
    zassert_equal(ctx.count, 1);
    zassert_equal(ctx.entries[0].data_len, EVENT_LOG_MSG_MAX_LEN);
}

/**
 * @brief Writing with severity higher than the current filter level
 *        silently drops the event (still returns 0).
 */
ZTEST(event_log_badpath_suite, test_write_dropped_returns_ok)
{
    event_log_set_level(EVENT_SEV_ERR);
    event_log_clear();

    int rc = event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "dropped");
    zassert_equal(rc, 0, "dropped write should still return 0, got %d", rc);

    int count = 0;
    event_log_read(0, 0, 0, false, count_cb, &count);
    zassert_equal(count, 0, "nothing should have been stored");
}

/**
 * @brief event_log_init may be called many times without side effects.
 */
ZTEST(event_log_badpath_suite, test_init_many_times)
{
    for (int i = 0; i < 5; i++) {
        int rc = event_log_init();
        zassert_equal(rc, 0, "repeat init #%d rc=%d", i, rc);
    }
}

