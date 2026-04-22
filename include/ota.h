#ifndef OTA_H
#define OTA_H

/**
 * @file ota.h
 * @brief OTA firmware update module interface.
 *
 * Provides MCUmgr-based OTA updates over UDP with a health-based
 * image confirmation system. Each application module reports its
 * readiness after successful initialization. The OTA thread waits
 * for all modules to report ready before confirming the running
 * image with MCUboot, preventing auto-revert of a known-good firmware.
 *
 * To add a new module to the health check:
 *   1. Add an entry to the ota_module enum (before OTA_MODULE_COUNT).
 *   2. Call ota_report_module_ready() from the module after init succeeds.
 *   That's it — the OTA thread automatically checks all registered modules.
 */

#include <zephyr/kernel.h>

/* ---------- Thread configuration ---------- */
#define OTA_STACK_SIZE         2048
#define OTA_PRIORITY           8
#define OTA_CONFIRM_TIMEOUT_S  30

/* ---------- Module health registry ---------- */

/**
 * @brief Enumeration of all application modules tracked by the OTA health system.
 *
 * Each module that must be healthy before an OTA image is confirmed
 * gets an entry here. OTA_MODULE_COUNT is always last and serves as
 * the total module count for the bitmask.
 *
 * Customize this enum for your application — add/remove entries as needed.
 * Max 32 modules (limited by atomic_t width).
 */
enum ota_module {
    OTA_MODULE_NET,          /**< Network — ready after interface up and DHCP started */
    OTA_MODULE_UART,         /**< Command UART — ready after IRQ enabled */
    OTA_MODULE_REST_API,     /**< REST API — ready after HTTP server started */
    OTA_MODULE_CONFIG_STORE, /**< Config store — ready after settings loaded */
    OTA_MODULE_COUNT         /**< Sentinel — must be last. */
};

/* ---------- Thread resources (defined in ota.c) ---------- */
extern struct k_thread ota_thread_data;
extern k_thread_stack_t ota_stack[];

/* ---------- Module API ---------- */

/**
 * @brief Report that a module has successfully initialized.
 *
 * Sets the corresponding bit in the health bitmask. Lock-free and
 * ISR-safe (uses atomic_or). Idempotent — calling twice is harmless.
 *
 * @param mod The module that is now ready (from enum ota_module).
 */
void ota_report_module_ready(enum ota_module mod);

/**
 * @brief OTA module thread entry point.
 *
 * Waits for all registered modules to report ready (up to
 * OTA_CONFIRM_TIMEOUT_S seconds), then confirms the running image
 * with MCUboot via boot_write_img_confirmed(). If not all modules
 * report in time, the image is left unconfirmed and MCUboot will
 * revert to the previous image on the next reset.
 *
 * When not running under MCUboot (e.g. direct flash via debugger),
 * the thread detects this and exits gracefully.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void ota_thread_entry(void *p1, void *p2, void *p3);

#endif /* OTA_H */
