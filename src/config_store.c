/**
 * @file config_store.c
 * @brief Persistent configuration store — Zephyr settings with ZMS backend.
 *
 * Settings subtrees (one per feature):
 *   "heartbeat"    — webhook heartbeat config (enabled, url, interval)
 *   "system"       — device identity (name)
 *   "event_log"    — log verbosity, persistent boot counter
 *   "time_service" — NTP server, sync interval
 *
 * Values are loaded from ZMS on boot and persisted immediately on set.
 * The storage partition (256 KB) sits outside MCUboot's swap area, so
 * configs survive firmware updates.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <errno.h>

#include "config_store.h"
#include "ota.h"

LOG_MODULE_REGISTER(config_store, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(config_store_stack, CONFIG_STORE_STACK_SIZE);
struct k_thread config_store_thread_data;

/* ---------- Mutex for config variable access ---------- */
static K_MUTEX_DEFINE(cfg_lock);

/* ================================================================
 * Factory defaults — edit these to change power-on defaults
 * ================================================================ */

#define DEFAULT_HEARTBEAT_ENABLED       false
#define DEFAULT_HEARTBEAT_URL           ""
#define DEFAULT_HEARTBEAT_INTERVAL      60
#define DEFAULT_SYSTEM_NAME             "My-Device"
#define DEFAULT_LOG_VERBOSITY           MCU_LOG_INFO
#define DEFAULT_BOOT_COUNT              0
#define DEFAULT_NTP_SERVER              "216.239.35.0"
#define DEFAULT_NTP_SYNC_INTERVAL_SEC   600
#define DEFAULT_IP_ADDRESS              ""
#define DEFAULT_IP_MASK                 "255.255.255.0"
#define DEFAULT_IP_GATEWAY              ""

/* ================================================================
 * Config variables (initialised to factory defaults)
 * ================================================================ */

static bool     heartbeat_enabled  = DEFAULT_HEARTBEAT_ENABLED;
static char     heartbeat_url[CONFIG_STORE_URL_MAX_LEN] = DEFAULT_HEARTBEAT_URL;
static uint32_t heartbeat_interval = DEFAULT_HEARTBEAT_INTERVAL;

static char     system_name[CONFIG_STORE_NAME_MAX_LEN] = DEFAULT_SYSTEM_NAME;

static uint8_t  log_verbosity = DEFAULT_LOG_VERBOSITY;
static uint32_t boot_count    = DEFAULT_BOOT_COUNT;

static char     ntp_server[CONFIG_STORE_NTP_SERVER_MAX_LEN] = DEFAULT_NTP_SERVER;
static uint32_t ntp_sync_interval = DEFAULT_NTP_SYNC_INTERVAL_SEC;

static char     ip_address[CONFIG_STORE_IP_ADDR_MAX_LEN] = DEFAULT_IP_ADDRESS;
static char     ip_mask[CONFIG_STORE_IP_ADDR_MAX_LEN]    = DEFAULT_IP_MASK;
static char     ip_gateway[CONFIG_STORE_IP_ADDR_MAX_LEN] = DEFAULT_IP_GATEWAY;

/* ================================================================
 * Settings handler: "heartbeat" subtree
 * ================================================================ */

static int heartbeat_set(const char *name, size_t len,
                         settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "enabled", &next) && !next) {
        if (len != sizeof(heartbeat_enabled)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &heartbeat_enabled, sizeof(heartbeat_enabled));
    }

    if (settings_name_steq(name, "url", &next) && !next) {
        if (len >= sizeof(heartbeat_url)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, heartbeat_url, sizeof(heartbeat_url) - 1);
        if (rc >= 0) {
            heartbeat_url[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    if (settings_name_steq(name, "interval", &next) && !next) {
        if (len != sizeof(heartbeat_interval)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &heartbeat_interval, sizeof(heartbeat_interval));
    }

    return -ENOENT;
}

static int heartbeat_export(int (*cb)(const char *name, const void *val,
                                      size_t val_len))
{
    cb("heartbeat/enabled",  &heartbeat_enabled,  sizeof(heartbeat_enabled));
    cb("heartbeat/url",      heartbeat_url,        strlen(heartbeat_url));
    cb("heartbeat/interval", &heartbeat_interval,  sizeof(heartbeat_interval));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(heartbeat, "heartbeat", NULL, heartbeat_set,
                               NULL, heartbeat_export);

/* ================================================================
 * Settings handler: "system" subtree
 * ================================================================ */

static int system_set(const char *name, size_t len,
                      settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "name", &next) && !next) {
        if (len >= sizeof(system_name)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, system_name, sizeof(system_name) - 1);
        if (rc >= 0) {
            system_name[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    return -ENOENT;
}

static int system_export(int (*cb)(const char *name, const void *val,
                                   size_t val_len))
{
    cb("system/name", system_name, strlen(system_name));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(system_cfg, "system", NULL, system_set,
                               NULL, system_export);

/* ================================================================
 * Settings handler: "event_log" subtree
 * ================================================================ */

static int event_log_set(const char *name, size_t len,
                         settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "logVerbosity", &next) && !next) {
        if (len != sizeof(log_verbosity)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &log_verbosity, sizeof(log_verbosity));
    }

    if (settings_name_steq(name, "bootCount", &next) && !next) {
        if (len != sizeof(boot_count)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &boot_count, sizeof(boot_count));
    }

    return -ENOENT;
}

static int event_log_export(int (*cb)(const char *name, const void *val,
                                      size_t val_len))
{
    cb("event_log/logVerbosity", &log_verbosity, sizeof(log_verbosity));
    cb("event_log/bootCount",   &boot_count,    sizeof(boot_count));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(event_log_cfg, "event_log", NULL, event_log_set,
                               NULL, event_log_export);

/* ================================================================
 * Settings handler: "time_service" subtree
 * ================================================================ */

static int time_service_set(const char *name, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "ntpServer", &next) && !next) {
        if (len >= sizeof(ntp_server)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, ntp_server, sizeof(ntp_server) - 1);
        if (rc >= 0) {
            ntp_server[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    if (settings_name_steq(name, "ntpSyncIntervalSec", &next) && !next) {
        if (len != sizeof(ntp_sync_interval)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &ntp_sync_interval, sizeof(ntp_sync_interval));
    }

    return -ENOENT;
}

static int time_service_export(int (*cb)(const char *name, const void *val,
                                         size_t val_len))
{
    cb("time_service/ntpServer",          ntp_server,         strlen(ntp_server));
    cb("time_service/ntpSyncIntervalSec", &ntp_sync_interval, sizeof(ntp_sync_interval));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(time_service_cfg, "time_service", NULL,
                               time_service_set, NULL, time_service_export);

/* ================================================================
 * Settings handler: "network" subtree
 * ================================================================ */

static int network_set(const char *name, size_t len,
                       settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "ipAddress", &next) && !next) {
        if (len >= sizeof(ip_address)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, ip_address, sizeof(ip_address) - 1);
        if (rc >= 0) {
            ip_address[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    if (settings_name_steq(name, "ipMask", &next) && !next) {
        if (len >= sizeof(ip_mask)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, ip_mask, sizeof(ip_mask) - 1);
        if (rc >= 0) {
            ip_mask[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    if (settings_name_steq(name, "ipDefaultGateway", &next) && !next) {
        if (len >= sizeof(ip_gateway)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, ip_gateway, sizeof(ip_gateway) - 1);
        if (rc >= 0) {
            ip_gateway[rc] = '\0';
        }
        return rc < 0 ? rc : 0;
    }

    return -ENOENT;
}

static int network_export(int (*cb)(const char *name, const void *val,
                                    size_t val_len))
{
    cb("network/ipAddress",        ip_address, strlen(ip_address));
    cb("network/ipMask",           ip_mask,    strlen(ip_mask));
    cb("network/ipDefaultGateway", ip_gateway, strlen(ip_gateway));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(network_cfg, "network", NULL,
                               network_set, NULL, network_export);

/* ================================================================
 * Public API — Getters & Setters
 * ================================================================ */

bool config_store_get_heartbeat_enabled(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    bool val = heartbeat_enabled;
    k_mutex_unlock(&cfg_lock);
    return val;
}

int config_store_set_heartbeat_enabled(bool enabled)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    heartbeat_enabled = enabled;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("heartbeat/enabled", &enabled, sizeof(enabled));
}

int config_store_get_heartbeat_url(char *buf, size_t buf_len)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    size_t len = strlen(heartbeat_url);
    if (buf_len < len + 1) {
        k_mutex_unlock(&cfg_lock);
        return -ENOMEM;
    }
    memcpy(buf, heartbeat_url, len + 1);
    k_mutex_unlock(&cfg_lock);
    return (int)len;
}

int config_store_set_heartbeat_url(const char *url)
{
    size_t len = strlen(url);
    if (len >= CONFIG_STORE_URL_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(heartbeat_url, url, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("heartbeat/url", url, len);
}

uint32_t config_store_get_heartbeat_interval(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    uint32_t val = heartbeat_interval;
    k_mutex_unlock(&cfg_lock);
    return val;
}

int config_store_set_heartbeat_interval(uint32_t interval_s)
{
    if (interval_s == 0) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    heartbeat_interval = interval_s;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("heartbeat/interval", &interval_s,
                             sizeof(interval_s));
}

int config_store_get_system_name(char *buf, size_t buf_len)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    size_t len = strlen(system_name);
    if (buf_len < len + 1) {
        k_mutex_unlock(&cfg_lock);
        return -ENOMEM;
    }
    memcpy(buf, system_name, len + 1);
    k_mutex_unlock(&cfg_lock);
    return (int)len;
}

int config_store_set_system_name(const char *name)
{
    size_t len = strlen(name);
    if (len >= CONFIG_STORE_NAME_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(system_name, name, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("system/name", name, len);
}

enum mcu_log_verbosity config_store_get_log_verbosity(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    enum mcu_log_verbosity v = (enum mcu_log_verbosity)log_verbosity;
    k_mutex_unlock(&cfg_lock);
    return v;
}

int config_store_set_log_verbosity(enum mcu_log_verbosity v)
{
    if (v > MCU_LOG_DEBUG) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    log_verbosity = (uint8_t)v;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("event_log/logVerbosity",
                             &log_verbosity, sizeof(log_verbosity));
}

uint32_t config_store_get_boot_count(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    uint32_t v = boot_count;
    k_mutex_unlock(&cfg_lock);
    return v;
}

int config_store_set_boot_count(uint32_t count)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    boot_count = count;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("event_log/bootCount", &count, sizeof(count));
}

int config_store_get_ntp_server(char *buf, size_t buf_len)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    size_t len = strlen(ntp_server);
    if (buf_len < len + 1) {
        k_mutex_unlock(&cfg_lock);
        return -ENOMEM;
    }
    memcpy(buf, ntp_server, len + 1);
    k_mutex_unlock(&cfg_lock);
    return (int)len;
}

int config_store_set_ntp_server(const char *server)
{
    size_t len = strlen(server);
    if (len >= CONFIG_STORE_NTP_SERVER_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(ntp_server, server, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("time_service/ntpServer", server, len);
}

uint32_t config_store_get_ntp_sync_interval(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    uint32_t v = ntp_sync_interval;
    k_mutex_unlock(&cfg_lock);
    return v;
}

int config_store_set_ntp_sync_interval(uint32_t seconds)
{
    if (seconds == 0) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    ntp_sync_interval = seconds;
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("time_service/ntpSyncIntervalSec",
                             &seconds, sizeof(seconds));
}

/* --- network/ IP address / mask / gateway --- */

static int ip_str_get(const char *src, char *buf, size_t buf_len)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);
    size_t len = strlen(src);
    if (buf_len < len + 1) {
        k_mutex_unlock(&cfg_lock);
        return -ENOMEM;
    }
    memcpy(buf, src, len + 1);
    k_mutex_unlock(&cfg_lock);
    return (int)len;
}

int config_store_get_ip_address(char *buf, size_t buf_len)
{
    return ip_str_get(ip_address, buf, buf_len);
}

int config_store_set_ip_address(const char *addr)
{
    size_t len = strlen(addr);
    if (len >= CONFIG_STORE_IP_ADDR_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(ip_address, addr, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("network/ipAddress", addr, len);
}

int config_store_get_ip_mask(char *buf, size_t buf_len)
{
    return ip_str_get(ip_mask, buf, buf_len);
}

int config_store_set_ip_mask(const char *mask)
{
    size_t len = strlen(mask);
    if (len >= CONFIG_STORE_IP_ADDR_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(ip_mask, mask, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("network/ipMask", mask, len);
}

int config_store_get_ip_gateway(char *buf, size_t buf_len)
{
    return ip_str_get(ip_gateway, buf, buf_len);
}

int config_store_set_ip_gateway(const char *gw)
{
    size_t len = strlen(gw);
    if (len >= CONFIG_STORE_IP_ADDR_MAX_LEN) {
        return -EINVAL;
    }
    k_mutex_lock(&cfg_lock, K_FOREVER);
    memcpy(ip_gateway, gw, len + 1);
    k_mutex_unlock(&cfg_lock);
    return settings_save_one("network/ipDefaultGateway", gw, len);
}

/* ================================================================
 * Factory reset — restore all values to compiled-in defaults
 * ================================================================ */

int config_store_factory_reset(void)
{
    int rc = 0;
    int ret;

    LOG_INF("Factory reset: restoring all settings to defaults...");

    k_mutex_lock(&cfg_lock, K_FOREVER);
    heartbeat_enabled  = DEFAULT_HEARTBEAT_ENABLED;
    memset(heartbeat_url, 0, sizeof(heartbeat_url));
    strncpy(heartbeat_url, DEFAULT_HEARTBEAT_URL, sizeof(heartbeat_url) - 1);
    heartbeat_interval = DEFAULT_HEARTBEAT_INTERVAL;
    memset(system_name, 0, sizeof(system_name));
    strncpy(system_name, DEFAULT_SYSTEM_NAME, sizeof(system_name) - 1);
    log_verbosity = DEFAULT_LOG_VERBOSITY;
    boot_count    = DEFAULT_BOOT_COUNT;
    memset(ntp_server, 0, sizeof(ntp_server));
    strncpy(ntp_server, DEFAULT_NTP_SERVER, sizeof(ntp_server) - 1);
    ntp_sync_interval = DEFAULT_NTP_SYNC_INTERVAL_SEC;
    strncpy(ip_address, DEFAULT_IP_ADDRESS, sizeof(ip_address) - 1);
    ip_address[sizeof(ip_address) - 1] = '\0';
    strncpy(ip_mask, DEFAULT_IP_MASK, sizeof(ip_mask) - 1);
    ip_mask[sizeof(ip_mask) - 1] = '\0';
    strncpy(ip_gateway, DEFAULT_IP_GATEWAY, sizeof(ip_gateway) - 1);
    ip_gateway[sizeof(ip_gateway) - 1] = '\0';
    k_mutex_unlock(&cfg_lock);

    ret = settings_save_one("heartbeat/enabled",
                            &heartbeat_enabled, sizeof(heartbeat_enabled));
    if (ret) { rc = ret; }

    ret = settings_save_one("heartbeat/url",
                            heartbeat_url, strlen(heartbeat_url));
    if (ret) { rc = ret; }

    ret = settings_save_one("heartbeat/interval",
                            &heartbeat_interval, sizeof(heartbeat_interval));
    if (ret) { rc = ret; }

    ret = settings_save_one("system/name",
                            system_name, strlen(system_name));
    if (ret) { rc = ret; }

    ret = settings_save_one("event_log/logVerbosity",
                            &log_verbosity, sizeof(log_verbosity));
    if (ret) { rc = ret; }

    ret = settings_save_one("event_log/bootCount",
                            &boot_count, sizeof(boot_count));
    if (ret) { rc = ret; }

    ret = settings_save_one("time_service/ntpServer",
                            ntp_server, strlen(ntp_server));
    if (ret) { rc = ret; }

    ret = settings_save_one("time_service/ntpSyncIntervalSec",
                            &ntp_sync_interval, sizeof(ntp_sync_interval));
    if (ret) { rc = ret; }

    ret = settings_save_one("network/ipAddress",
                            ip_address, strlen(ip_address));
    if (ret) { rc = ret; }

    ret = settings_save_one("network/ipMask",
                            ip_mask, strlen(ip_mask));
    if (ret) { rc = ret; }

    ret = settings_save_one("network/ipDefaultGateway",
                            ip_gateway, strlen(ip_gateway));
    if (ret) { rc = ret; }

    if (rc) {
        LOG_ERR("Factory reset: some keys failed to persist (last err %d)", rc);
    } else {
        LOG_INF("Factory reset complete");
    }

    config_store_dump();
    return rc;
}

/* ================================================================
 * Dump — log all current config values
 * ================================================================ */

void config_store_dump(void)
{
    k_mutex_lock(&cfg_lock, K_FOREVER);

    LOG_INF("========================================");
    LOG_INF("  Persistent Configuration");
    LOG_INF("========================================");
    LOG_INF("  heartbeat/enabled            = %s", heartbeat_enabled ? "true" : "false");
    LOG_INF("  heartbeat/url                = \"%s\"",
            heartbeat_url[0] ? heartbeat_url : "(empty)");
    LOG_INF("  heartbeat/interval           = %u s", heartbeat_interval);
    LOG_INF("  system/name                  = \"%s\"", system_name);
    LOG_INF("  event_log/logVerbosity       = %u", log_verbosity);
    LOG_INF("  event_log/bootCount          = %u", boot_count);
    LOG_INF("  time_service/ntpServer       = \"%s\"",
            ntp_server[0] ? ntp_server : "(empty)");
    LOG_INF("  time_service/ntpSyncIntervalSec = %u s", ntp_sync_interval);
    LOG_INF("  network/ipAddress            = \"%s\"",
            ip_address[0] ? ip_address : "(empty -> DHCP)");
    LOG_INF("  network/ipMask               = \"%s\"",
            ip_mask[0] ? ip_mask : "(empty)");
    LOG_INF("  network/ipDefaultGateway     = \"%s\"",
            ip_gateway[0] ? ip_gateway : "(empty)");
    LOG_INF("========================================");

    k_mutex_unlock(&cfg_lock);
}

/* ================================================================
 * Thread entry — init, load, dump, report ready
 * ================================================================ */

void config_store_thread_entry(void *p1, void *p2, void *p3)
{
    int rc;

    LOG_INF("Config store initializing...");

    rc = settings_subsys_init();
    if (rc) {
        LOG_ERR("settings_subsys_init failed: %d", rc);
        return;
    }

    rc = settings_load();
    if (rc) {
        LOG_ERR("settings_load failed: %d", rc);
        return;
    }

    LOG_INF("Settings loaded from flash");
    config_store_dump();

    ota_report_module_ready(OTA_MODULE_CONFIG_STORE);

    while (1) {
        k_sleep(K_FOREVER);
    }
}
