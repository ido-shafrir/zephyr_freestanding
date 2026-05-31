/**
 * @file ota.c
 * @brief OTA firmware update module — MCUmgr UDP transport + health-based confirmation.
 *
 * This module provides two things:
 *
 * 1. **MCUmgr SMP server over UDP** — Zephyr's MCUmgr subsystem handles
 *    this automatically once CONFIG_MCUMGR_TRANSPORT_UDP=y is set.
 *    The SMP server listens on the configured UDP port (default 1337)
 *    and accepts image upload / management commands from the `mcumgr` CLI.
 *
 * 2. **Health-based image confirmation** — After an OTA swap, MCUboot
 *    boots the new image in "test" mode. If the image is not confirmed
 *    before the next reset, MCUboot reverts to the previous image.
 *    This module tracks which application subsystems have initialized
 *    successfully via an atomic bitmask. Once all modules report ready
 *    (or a timeout expires), the thread either confirms the image or
 *    leaves it unconfirmed so MCUboot can revert.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include <app_version.h>
#include "ota.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(ota_stack, OTA_STACK_SIZE);
struct k_thread ota_thread_data;

/* ---------- Module health registry ---------- */
static atomic_t module_status = ATOMIC_INIT(0);

/**
 * @brief Human-readable names for each module, used in log messages.
 *
 * Must have one entry per value in the ota_module enum (ota.h).
 * The BUILD_ASSERT below catches mismatches at compile time
 * (see bug_reports/018_ota_module_names_missing_entry.md).
 */
static const char *const module_names[] = {
    [OTA_MODULE_NET]          = "net",
    [OTA_MODULE_UART]         = "command_uart",
    [OTA_MODULE_REST_API]     = "rest_api",
    [OTA_MODULE_CONFIG_STORE] = "config_store",
    [OTA_MODULE_EVENT_LOG]    = "event_log",
    [OTA_MODULE_TIME_SERVICE] = "time_service",
};
BUILD_ASSERT(ARRAY_SIZE(module_names) == OTA_MODULE_COUNT,
             "module_names[] must have one entry per ota_module enum value");

/**
 * @brief Count the number of set bits in a value (population count).
 *
 * Simple portable implementation used for log messages only.
 *
 * @param v The value to count bits in.
 * @return Number of set bits.
 */
static int popcount(atomic_val_t v)
{
    int count = 0;
    while (v) {
        count += v & 1;
        v >>= 1;
    }
    return count;
}

/**
 * @brief Report that a module has successfully initialized.
 *
 * Sets the corresponding bit in the module_status bitmask using an
 * atomic OR. This is lock-free and safe to call from any context
 * (thread, ISR, or work queue). Calling multiple times is harmless.
 *
 * @param mod The module that is now ready (from enum ota_module).
 */
void ota_report_module_ready(enum ota_module mod)
{
    if (mod >= OTA_MODULE_COUNT) {
        return;
    }

    atomic_or(&module_status, BIT(mod));
    LOG_INF("Module ready: %s (%d/%d)",
            module_names[mod],
            popcount(atomic_get(&module_status)),
            OTA_MODULE_COUNT);
}

const char *ota_get_fw_version(void)
{
    return APP_VERSION_STRING;
}

/**
 * @brief Check if all registered modules have reported ready.
 *
 * Compares the module_status bitmask against a mask with all
 * OTA_MODULE_COUNT bits set.
 *
 * @return true if every module bit is set, false otherwise.
 */
static bool all_modules_ready(void)
{
    atomic_val_t expected = BIT_MASK(OTA_MODULE_COUNT);
    return (atomic_get(&module_status) & expected) == expected;
}

/**
 * @brief OTA module thread entry point.
 *
 * Performs health-based image confirmation for MCUboot OTA updates.
 *
 * Flow:
 *   1. If image is already confirmed (or no MCUboot), log and exit.
 *   2. Poll all_modules_ready() every 500 ms for up to OTA_CONFIRM_TIMEOUT_S.
 *   3. If all modules ready -> boot_write_img_confirmed() -> success.
 *   4. If timeout -> leave unconfirmed -> MCUboot reverts on next reset.
 *   5. Sleep forever (MCUmgr UDP server runs in its own Zephyr thread).
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void ota_thread_entry(void *p1, void *p2, void *p3)
{
    LOG_INF("OTA thread started");

    /* Check if we're running under MCUboot and need confirmation */
    if (boot_is_img_confirmed()) {
        LOG_INF("Image already confirmed (direct flash or previously confirmed)");
        LOG_INF("MCUmgr SMP server active — ready for OTA uploads");
        goto done;
    }

    LOG_INF("Image is NOT confirmed — running health check (timeout: %ds)",
            OTA_CONFIRM_TIMEOUT_S);

    /* Poll for all modules to report ready */
    int elapsed_ms = 0;
    const int poll_interval_ms = 500;
    const int timeout_ms = OTA_CONFIRM_TIMEOUT_S * 1000;

    while (elapsed_ms < timeout_ms) {
        if (all_modules_ready()) {
            int ret = boot_write_img_confirmed();
            if (ret == 0) {
                LOG_INF("========================================");
                LOG_INF("  All modules healthy — image CONFIRMED");
                LOG_INF("  Elapsed: %d.%ds", elapsed_ms / 1000, (elapsed_ms % 1000) / 100);
                LOG_INF("========================================");
            } else {
                LOG_ERR("boot_write_img_confirmed() failed: %d", ret);
                LOG_ERR("MCUboot may revert on next reset!");
            }
            goto done;
        }

        k_msleep(poll_interval_ms);
        elapsed_ms += poll_interval_ms;
    }

    /* Timeout — not all modules reported ready */
    LOG_ERR("========================================");
    LOG_ERR("  HEALTH CHECK TIMEOUT — image NOT confirmed!");
    LOG_ERR("  Ready modules: %d/%d",
            popcount(atomic_get(&module_status)), OTA_MODULE_COUNT);
    for (int i = 0; i < OTA_MODULE_COUNT; i++) {
        if (!(atomic_get(&module_status) & BIT(i))) {
            LOG_ERR("  MISSING: %s", module_names[i]);
        }
    }
    LOG_ERR("  MCUboot will REVERT to previous image on next reset.");
    LOG_ERR("========================================");

done:
    /* MCUmgr SMP UDP transport runs in its own Zephyr thread —
     * nothing more for this thread to do. Sleep forever. */
    while (1) {
        k_sleep(K_FOREVER);
    }
}
