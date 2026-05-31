/**
 * @file event_log.c
 * @brief Flash-backed circular event log — Zephyr FCB implementation.
 *
 * Stores fixed-size 76-byte event records on the "event-log" flash
 * partition (256 KB, 2 sectors, 1 scratch).  When the active sector
 * fills up, FCB automatically rotates to the scratch sector and erases
 * the oldest data — a true ring buffer.
 *
 * The runtime severity filter is initialised from config_store's
 * mcu_log_verbosity on boot and can be changed at runtime via
 * event_log_set_level() (called from REST PATCH /api/mcu and UART).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include <stdio.h>

#include "event_log.h"
#include "config_store.h"
#include "ota.h"
#include "time_service.h"

LOG_MODULE_REGISTER(event_log, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(event_log_stack, EVENT_LOG_STACK_SIZE);
struct k_thread event_log_thread_data;

/* ---------- FCB instance ---------- */
#define EVENT_LOG_SECTOR_COUNT  2
#define EVENT_LOG_MAX_SECTORS  128  /* max sectors for any platform */
#define EVENT_LOG_SCRATCH_COUNT 1
#define EVENT_LOG_MAGIC         0x45564C49  /* "EVLI" — bumped for boot_id field (80-byte entry) */

static struct flash_sector sectors[EVENT_LOG_MAX_SECTORS];
static struct fcb event_fcb;

/* ---------- Mutex for FCB access ---------- */
static K_MUTEX_DEFINE(log_lock);

/* ---------- Runtime filter level (atomic for ISR safety) ---------- */
/*
* This declares a thread-safe log level variable using Zephyr's atomic API.
* atomic_t — Zephyr's atomic integer type (<zephyr/sys/atomic.h>), guaranteeing that reads and writes are indivisible —
* no locks needed when multiple threads access it
* ATOMIC_INIT(EVENT_SEV_INFO) — compile-time initializer that sets the initial value to EVENT_SEV_INFO
* The purpose: the current log/event severity threshold is stored atomically so any thread can read or change it without a mutex. 
* You'd access it via atomic_get(&log_level) and update with atomic_set(&log_level, new_value).
*/
static atomic_t log_level = ATOMIC_INIT(EVENT_SEV_INFO);

/* ---------- Initialisation flag ---------- */
static bool fcb_ready;

/* ---------- Boot ID for the current session ----------
 * Loaded from config_store's mcu/bootCount, incremented, and
 * persisted back inside event_log_init(). Stamped on every entry
 * written during this boot so cross-boot queries can filter out
 * noise from previous sessions (see bug report #019).
 */
static uint32_t current_boot_id;

uint32_t event_log_get_boot_id(void)
{
    return current_boot_id;
}

/* ================================================================
 * String helpers
 * ================================================================ */

/**
 * @brief Convert a severity enum to a human-readable string.
 *
 * @param sev Severity value.
 * @return Static string (e.g. "INFO", "ERR").  Returns "???" for
 *         out-of-range values.
 */
const char *severity_to_str(enum event_severity sev)
{
    static const char *names[] = {
        [EVENT_SEV_EMERG]  = "EMERG",
        [EVENT_SEV_ALERT]  = "ALERT",
        [EVENT_SEV_CRIT]   = "CRIT",
        [EVENT_SEV_ERR]    = "ERR",
        [EVENT_SEV_WARN]   = "WARN",
        [EVENT_SEV_NOTICE] = "NOTICE",
        [EVENT_SEV_INFO]   = "INFO",
        [EVENT_SEV_DEBUG]  = "DEBUG",
    };
    if (sev <= EVENT_SEV_DEBUG) {
        return names[sev];
    }
    return "???";
}

/**
 * @brief Convert an event type enum to a human-readable string.
 *
 * @param type Event type value.
 * @return Static string (e.g. "BOOT", "NETWORK").  Returns "???"
 *         for out-of-range values.
 */
const char *event_type_to_str(enum event_type type)
{
    static const char *names[] = {
        [EVENT_TYPE_BOOT]    = "BOOT",
        [EVENT_TYPE_NETWORK] = "NETWORK",
        [EVENT_TYPE_OTA]     = "OTA",
        [EVENT_TYPE_CONFIG]  = "CONFIG",
        [EVENT_TYPE_WEBHOOK] = "WEBHOOK",
        [EVENT_TYPE_SYSTEM]  = "SYSTEM",
        [EVENT_TYPE_USER]    = "USER",
    };
    if (type < EVENT_TYPE_COUNT) {
        return names[type];
    }
    return "???";
}

/**
 * @brief Map config_store's mcu_log_verbosity to an event_severity filter.
 *
 * @param v  The mcu_log_verbosity value from config_store.
 * @return   Corresponding event_severity threshold.
 *
 * @par Mapping
 * | mcu_log_verbosity | event_severity |
 * |-------------------|----------------|
 * | MCU_LOG_ERROR (0) | EVENT_SEV_ERR  (3) |
 * | MCU_LOG_WARN  (1) | EVENT_SEV_WARN (4) |
 * | MCU_LOG_INFO  (2) | EVENT_SEV_INFO (6) |
 * | MCU_LOG_DEBUG (3) | EVENT_SEV_DEBUG(7) |
 */
static enum event_severity severity_from_verbosity(enum mcu_log_verbosity v)
{
    switch (v) {
    case MCU_LOG_ERROR: return EVENT_SEV_ERR;
    case MCU_LOG_WARN:  return EVENT_SEV_WARN;
    case MCU_LOG_INFO:  return EVENT_SEV_INFO;
    case MCU_LOG_DEBUG: return EVENT_SEV_DEBUG;
    default:            return EVENT_SEV_INFO;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * @brief Initialize the FCB event log subsystem.
 *
 * Queries the flash partition geometry and calls fcb_init().
 * Safe to call multiple times — subsequent calls are no-ops.
 *
 * @return 0 on success, negative errno on failure.
 */
int event_log_init(void)
{
    if (fcb_ready) {
        return 0;
    }

    const struct flash_area *fa;
    int rc = flash_area_open(PARTITION_ID(event_log_partition), &fa);
    if (rc) {
        LOG_ERR("Failed to open event-log partition: %d", rc);
        return rc;
    }

    /* Query sector layout from flash driver */
    uint32_t sector_count = EVENT_LOG_MAX_SECTORS;
    rc = flash_area_get_sectors(PARTITION_ID(event_log_partition),
                                &sector_count, sectors);
    if (rc) {
        LOG_ERR("Failed to get sector info: %d", rc);
        flash_area_close(fa);
        return rc;
    }

    flash_area_close(fa);

    memset(&event_fcb, 0, sizeof(event_fcb));
    event_fcb.f_magic    = EVENT_LOG_MAGIC;
    event_fcb.f_version  = 1;
    event_fcb.f_sector_cnt = sector_count;
    event_fcb.f_scratch_cnt = EVENT_LOG_SCRATCH_COUNT;
    event_fcb.f_sectors  = sectors;

    rc = fcb_init(PARTITION_ID(event_log_partition), &event_fcb);
    if (rc) {
        /* Most common cause on OTA upgrades: the partition still holds a
         * previous firmware's FCB data with a different magic/version
         * (fcb_init returns -ENOMSG / -35).  MCUboot only swaps slot0, so
         * data partitions survive across upgrades and can look stale.
         * Wipe the partition and retry once before giving up. */
        LOG_WRN("fcb_init failed: %d — wiping event-log partition and retrying",
                rc);

        const struct flash_area *fa_wipe;
        int orc = flash_area_open(PARTITION_ID(event_log_partition), &fa_wipe);
        if (orc) {
            LOG_ERR("flash_area_open for wipe failed: %d", orc);
            return rc;
        }

        int erc = flash_area_erase(fa_wipe, 0, fa_wipe->fa_size);
        flash_area_close(fa_wipe);
        if (erc) {
            LOG_ERR("flash_area_erase failed: %d", erc);
            return rc;
        }

        /* Re-zero the fcb struct; fcb_init mutates internal fields. */
        memset(&event_fcb, 0, sizeof(event_fcb));
        event_fcb.f_magic       = EVENT_LOG_MAGIC;
        event_fcb.f_version     = 1;
        event_fcb.f_sector_cnt  = sector_count;
        event_fcb.f_scratch_cnt = EVENT_LOG_SCRATCH_COUNT;
        event_fcb.f_sectors     = sectors;

        rc = fcb_init(PARTITION_ID(event_log_partition), &event_fcb);
        if (rc) {
            LOG_ERR("fcb_init after wipe still failed: %d", rc);
            return rc;
        }

        LOG_WRN("event-log partition reformatted after stale magic");
    }

    fcb_ready = true;

    /* Increment and persist the boot counter so every entry written
     * during this session can be tagged with a unique, cross-reboot
     * identifier. Done here (after fcb_ready) so that if the
     * config_store write itself triggers an event_log_write it still
     * finds the FCB ready. Failure to persist is logged but non-fatal:
     * the in-memory current_boot_id still uniquely identifies this
     * session for filtering until reboot. */
    uint32_t prev = config_store_get_boot_count();
    current_boot_id = prev + 1U;
    int src = config_store_set_boot_count(current_boot_id);
    if (src) {
        LOG_WRN("failed to persist bootCount (%d); using in-memory id=%u",
                src, current_boot_id);
    }

    LOG_INF("FCB event log ready (%u sectors, scratch=%u, boot_id=%u)",
            sector_count, EVENT_LOG_SCRATCH_COUNT, current_boot_id);
    return 0;
}

/**
 * @brief Write an event to the log.
 *
 * Builds an event_entry_t on the stack, appends it to the FCB.
 * Events with severity > current filter level are silently dropped.
 *
 * @param sev  Severity level.
 * @param type Event category.
 * @param msg  Human-readable message (truncated at 64 bytes).
 * @return 0 on success or filtered-out, negative errno on flash error.
 */
int event_log_write(enum event_severity sev, enum event_type type,
                    const char *msg)
{
    /* Severity filter: higher numeric value = lower priority */
    if ((int)sev > atomic_get(&log_level)) {
        return 0;
    }

    if (!fcb_ready) {
        return -ENODEV;
    }

    /* Build entry on stack */
    event_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = (uint32_t)(k_uptime_get() / 1000);
    entry.wall_clock = (uint32_t)time_service_get();
    entry.boot_id    = current_boot_id;
    entry.event_type = (uint8_t)type;
    entry.severity   = (uint8_t)sev;

    if (msg) {
        size_t len = strlen(msg);
        if (len > EVENT_LOG_MSG_MAX_LEN) {
            len = EVENT_LOG_MSG_MAX_LEN;
        }
        memcpy(entry.message, msg, len);
        entry.data_len = (uint16_t)len;
    }

    /* Append to FCB */
    k_mutex_lock(&log_lock, K_FOREVER);

    struct fcb_entry loc;
    int rc = fcb_append(&event_fcb, sizeof(entry), &loc);
    if (rc == -ENOSPC) {
        /* Rotate: erase oldest sector, retry */
        fcb_rotate(&event_fcb);
        rc = fcb_append(&event_fcb, sizeof(entry), &loc);
    }
    if (rc) {
        k_mutex_unlock(&log_lock);
        LOG_ERR("fcb_append failed: %d", rc);
        return rc;
    }

    rc = flash_area_write(event_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc),
                          &entry, sizeof(entry));
    if (rc) {
        k_mutex_unlock(&log_lock);
        LOG_ERR("flash_area_write failed: %d", rc);
        return rc;
    }

    rc = fcb_append_finish(&event_fcb, &loc);
    k_mutex_unlock(&log_lock);

    if (rc) {
        LOG_ERR("fcb_append_finish failed: %d", rc);
    }
    return rc;
}

/** @brief Context passed to the internal FCB walk callback. */
struct read_ctx {
    uint32_t          since_wall;
    uint32_t          since_uptime;
    uint32_t          boot_id;
    bool              boot_id_strict;
    /* Pass-1 output: smallest boot_id whose sync'd entry falls in
     * the wall window. Pre-sync entries belonging to boots with
     * boot_id > min_in_window are emitted alongside their boot's
     * sync'd lines (they are inside the wall timeline that the
     * window picks). Boots strictly older than min_in_window are
     * suppressed in default mode to avoid prior-boot pre-sync leak.
     * min_valid is false when no sync'd entry passed the window —
     * in that case there is no cross-boot anchor and pre-sync is
     * gated only by ctx->boot_id (default-mode behaviour collapses
     * to "current boot only", same as iteration 4). */
    uint32_t          min_in_window;
    bool              min_valid;
    event_log_walk_cb cb;
    void             *user_data;
    int               count;
    bool              stop;
};

/**
 * @brief Pass-1 walk callback: discover the smallest boot_id whose
 *        sync'd entry falls in the wall window.
 *
 * Only sync'd entries (wall_clock != 0) contribute. Strict mode
 * (boot_id_strict) ignores this pass — strict mode wants exact
 * boot match on every entry, never cross-boot pre-sync inclusion.
 */
static int min_boot_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
    struct read_ctx *ctx = arg;

    event_entry_t entry;
    int rc = flash_area_read(loc_ctx->fap,
                             FCB_ENTRY_FA_DATA_OFF(loc_ctx->loc),
                             &entry, sizeof(entry));
    if (rc) {
        return 0;
    }

    if (entry.wall_clock == 0) {
        return 0; /* pre-sync entries don't anchor the window */
    }
    if (ctx->since_wall != 0 && entry.wall_clock < ctx->since_wall) {
        return 0; /* outside the wall window */
    }

    if (!ctx->min_valid || entry.boot_id < ctx->min_in_window) {
        ctx->min_in_window = entry.boot_id;
        ctx->min_valid     = true;
    }
    return 0;
}

/**
 * @brief Internal FCB walk callback — deserializes and filters entries.
 *
 * @param loc      FCB entry location.
 * @param arg      Pointer to struct read_ctx.
 * @return 0 to continue walking, non-zero to stop.
 */
static int walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
    struct read_ctx *ctx = arg;

    if (ctx->stop) {
        return -ECANCELED;
    }

    event_entry_t entry;
    int rc = flash_area_read(loc_ctx->fap,
                             FCB_ENTRY_FA_DATA_OFF(loc_ctx->loc),
                             &entry, sizeof(entry));
    if (rc) {
        return 0; /* skip unreadable entries */
    }

    /* Filter order (see event_log_read() docstring for full
     * semantics). The rule is asymmetric by design: a boot_id only
     * helps disambiguate entries whose wall_clock is 0, because
     * sync'd entries already carry a monotonic cross-reboot clock.
     *
     *   - Strict mode (boot_id_strict): every entry must match
     *     ctx->boot_id. Used by `event_log_boot <id>` to replay
     *     one boot's full log.
     *   - Default mode: sync'd entries are filtered purely by the
     *     wall window. Pre-sync entries are emitted iff their boot
     *     is "in scope": either it is the current boot
     *     (ctx->boot_id) or its boot_id is strictly greater than
     *     the smallest boot whose sync'd entries fell in the
     *     window (min_in_window). The latter rule lets a long
     *     window pick up the pre-sync banner / DHCP / SNTP-failed
     *     diagnostics of every boot that occurred *inside* the
     *     window, without re-introducing iteration 2's prior-boot
     *     leak from boots that started long before the window.
     */
    if (ctx->boot_id_strict &&
        ctx->boot_id != 0 &&
        entry.boot_id != ctx->boot_id) {
        return 0;
    }

    if (entry.wall_clock != 0) {
        if (ctx->since_wall != 0 && entry.wall_clock < ctx->since_wall) {
            return 0;
        }
    } else {
        /* Pre-sync entry. */
        bool emit;
        if (ctx->boot_id_strict) {
            /* Strict already gated above — accept. */
            emit = true;
        } else if (ctx->boot_id != 0 && entry.boot_id == ctx->boot_id) {
            /* Always emit current-boot pre-sync. */
            emit = true;
        } else if (ctx->min_valid && entry.boot_id > ctx->min_in_window) {
            /* Boot is inside the wall window (a younger boot than
             * the oldest one whose sync'd entries qualified) —
             * include its pre-sync diagnostics too. */
            emit = true;
        } else if (!ctx->min_valid && ctx->boot_id == 0 &&
                   ctx->since_wall == 0) {
            /* "Dump everything" path: no anchor, no caller filter. */
            emit = true;
        } else {
            emit = false;
        }
        if (!emit) {
            return 0;
        }
        if (ctx->since_uptime != 0 && entry.timestamp < ctx->since_uptime) {
            return 0;
        }
    }

    if (!ctx->cb(&entry, ctx->user_data)) {
        ctx->stop = true;
        return -ECANCELED;
    }

    ctx->count++;
    return 0;
}

/**
 * @brief Read events from the log, filtered by time and/or boot ID.
 *
 * @param since_wall     Wall-clock cutoff for sync'd entries (0 = any).
 * @param since_uptime   Uptime cutoff for pre-sync entries (0 = any).
 * @param boot_id        Boot ID to match (0 = any boot).
 * @param boot_id_strict If true, @p boot_id applies to every entry;
 *                       else default asymmetric filtering applies
 *                       (sync'd entries by wall window; pre-sync
 *                       entries by current-boot OR
 *                       boot_id > min_in_window).
 * @param cb             Callback per matching entry.
 * @param user_data      Opaque context for @p cb.
 * @return Count of entries walked, or negative errno.
 */
int event_log_read(uint32_t since_wall, uint32_t since_uptime,
                   uint32_t boot_id, bool boot_id_strict,
                   event_log_walk_cb cb, void *user_data)
{
    if (!cb) {
        return -EINVAL;
    }
    if (!fcb_ready) {
        return -ENODEV;
    }

    struct read_ctx ctx = {
        .since_wall     = since_wall,
        .since_uptime   = since_uptime,
        .boot_id        = boot_id,
        .boot_id_strict = boot_id_strict,
        .min_in_window  = 0,
        .min_valid      = false,
        .cb             = cb,
        .user_data      = user_data,
        .count          = 0,
        .stop           = false,
    };

    k_mutex_lock(&log_lock, K_FOREVER);

    /* Pass 1 — only needed in default (non-strict) mode, and only
     * when there is something for the cross-boot pre-sync rule to
     * anchor against (a wall window or a caller-supplied boot_id). */
    if (!boot_id_strict) {
        int rc1 = fcb_walk(&event_fcb, NULL, min_boot_walk_cb, &ctx);
        if (rc1 && rc1 != -ECANCELED) {
            k_mutex_unlock(&log_lock);
            return rc1;
        }
    }

    int rc = fcb_walk(&event_fcb, NULL, walk_cb, &ctx);
    k_mutex_unlock(&log_lock);

    /* fcb_walk returns -ECANCELED if our callback stopped early — that's OK */
    if (rc && rc != -ECANCELED) {
        return rc;
    }
    return ctx.count;
}

/**
 * @brief Erase all events from the log.
 *
 * @return 0 on success, negative errno on flash erase failure.
 */
int event_log_clear(void)
{
    if (!fcb_ready) {
        return -ENODEV;
    }

    k_mutex_lock(&log_lock, K_FOREVER);
    int rc = fcb_clear(&event_fcb);
    k_mutex_unlock(&log_lock);

    if (rc) {
        LOG_ERR("fcb_clear failed: %d", rc);
    } else {
        LOG_INF("Event log cleared");
    }
    return rc;
}

/**
 * @brief Set the runtime severity filter level.
 *
 * @param level Maximum severity to accept.
 */
void event_log_set_level(enum event_severity level)
{
    enum event_severity old = (enum event_severity)atomic_get(&log_level);

    atomic_set(&log_level, (atomic_val_t)level);

    if (old != level) {
        char msg[48];
        snprintf(msg, sizeof(msg), "log level changed: %s -> %s",
                 severity_to_str(old), severity_to_str(level));
        /* Write at INFO so it's logged unless filter is ERR-only */
        event_log_write(EVENT_SEV_INFO, EVENT_TYPE_CONFIG, msg);
    }
}

/**
 * @brief Get the current runtime severity filter level.
 *
 * @return Current filter level.
 */
enum event_severity event_log_get_level(void)
{
    return (enum event_severity)atomic_get(&log_level);
}

/* ================================================================
 * Thread entry
 * ================================================================ */

/**
 * @brief Event log thread entry point.
 *
 * Initializes FCB, syncs the filter level from config_store,
 * reports OTA readiness, writes a boot event, then sleeps forever.
 */
void event_log_thread_entry(void *p1, void *p2, void *p3)
{
    int rc = event_log_init();
    if (rc) {
        LOG_ERR("Event log init failed: %d — module not available", rc);
        return;
    }

    /* Sync filter level from persisted config */
    enum mcu_log_verbosity v = config_store_get_log_verbosity();
    enum event_severity sev = severity_from_verbosity(v);
    event_log_set_level(sev);

    LOG_INF("Event log initialized — filter level: %s (%d)",
            severity_to_str(sev), (int)sev);

    ota_report_module_ready(OTA_MODULE_EVENT_LOG);

    /* First event: boot (include firmware version for traceability) */
    char boot_msg[EVENT_LOG_MSG_MAX_LEN];
    snprintf(boot_msg, sizeof(boot_msg), "system boot fw=%s",
             ota_get_fw_version());
    event_log_write(EVENT_SEV_INFO, EVENT_TYPE_BOOT, boot_msg);
    event_log_write(EVENT_SEV_DEBUG, EVENT_TYPE_SYSTEM, "event_log init done");

    /* Nothing more to do — events are written from other threads */
    while (1) {
        k_sleep(K_FOREVER);
    }
}
