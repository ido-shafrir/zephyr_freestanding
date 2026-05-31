/**
 * @file stubs.c
 * @brief Stub functions for dependencies used by event_log.c in tests.
 */

#include "config_store.h"
#include "ota.h"
#include "time_service.h"

enum mcu_log_verbosity config_store_get_log_verbosity(void)
{
    return MCU_LOG_INFO;
}

void ota_report_module_ready(enum ota_module mod)
{
    (void)mod;
}

const char *ota_get_fw_version(void)
{
    return "test-0.0.0";
}

/* ---- toggleable time_service stubs ----
 * Tests can drive wall_clock stamping by calling
 * test_time_service_set(true, epoch) from their setup; by default
 * the device is unsynced so event_log_write() records wall_clock=0.
 */
static bool    stub_synced;
static int64_t stub_epoch;

void test_time_service_set(bool synced, int64_t epoch)
{
    stub_synced = synced;
    stub_epoch  = epoch;
}

bool time_service_is_synced(void)
{
    return stub_synced;
}

int64_t time_service_get(void)
{
    return stub_epoch;
}

/* ---- bootCount stubs ---- */
static uint32_t stub_boot_count;

uint32_t config_store_get_boot_count(void)
{
    return stub_boot_count;
}

int config_store_set_boot_count(uint32_t count)
{
    stub_boot_count = count;
    return 0;
}
