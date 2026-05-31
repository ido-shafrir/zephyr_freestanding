/**
 * @file stubs.c
 * @brief Stub functions for time_service.c dependencies in tests.
 *
 * Provides minimal implementations of config_store, ota, and event_log
 * functions so that time_service.c compiles and links in the test harness.
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <zephyr/net/sntp.h>
#include "config_store.h"
#include "ota.h"
#include "event_log.h"

static char stub_ntp_server[64] = "216.239.35.0";
static uint32_t stub_ntp_sync_interval = 600;

void stub_set_ntp_server(const char *server)
{
    strncpy(stub_ntp_server, server, sizeof(stub_ntp_server) - 1);
    stub_ntp_server[sizeof(stub_ntp_server) - 1] = '\0';
}

void stub_set_ntp_sync_interval(uint32_t interval)
{
    stub_ntp_sync_interval = interval;
}

int config_store_get_ntp_server(char *buf, size_t buf_len)
{
    size_t len = strlen(stub_ntp_server);
    if (buf_len < len + 1) return -1;
    memcpy(buf, stub_ntp_server, len + 1);
    return (int)len;
}

uint32_t config_store_get_ntp_sync_interval(void)
{
    return stub_ntp_sync_interval;
}

void ota_report_module_ready(enum ota_module mod)
{
    (void)mod;
}

int event_log_write(enum event_severity sev, enum event_type type, const char *msg)
{
    (void)sev;
    (void)type;
    (void)msg;
    return 0;
}

int sntp_simple(const char *server, uint32_t timeout, struct sntp_time *ts)
{
    (void)server;
    (void)timeout;
    (void)ts;
    return -ETIMEDOUT;
}
