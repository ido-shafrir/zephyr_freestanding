/**
 * @file stubs.c
 * @brief Stubs for symbols required by config_store.c but not
 *        available in the test build.
 */

#include "ota.h"

void ota_report_module_ready(enum ota_module mod)
{
    (void)mod;
}
