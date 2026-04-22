/**
 * @file config_store.c
 * @brief Persistent configuration store — Zephyr settings with ZMS backend.
 *
 * Two settings subtrees:
 *   "heartbeat" — webhook heartbeat config (enabled, url, interval)
 *   "system"    — device identity (name)
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

#define DEFAULT_HEARTBEAT_ENABLED   false
#define DEFAULT_HEARTBEAT_URL       ""
#define DEFAULT_HEARTBEAT_INTERVAL  60
#define DEFAULT_SYSTEM_NAME         "My-Device"

/* ================================================================
 * Config variables (initialised to factory defaults)
 * ================================================================ */

static bool     heartbeat_enabled  = DEFAULT_HEARTBEAT_ENABLED;
static char     heartbeat_url[CONFIG_STORE_URL_MAX_LEN] = DEFAULT_HEARTBEAT_URL;
static uint32_t heartbeat_interval = DEFAULT_HEARTBEAT_INTERVAL;

static char     system_name[CONFIG_STORE_NAME_MAX_LEN] = DEFAULT_SYSTEM_NAME;

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
    LOG_INF("  heartbeat/enabled  = %s", heartbeat_enabled ? "true" : "false");
    LOG_INF("  heartbeat/url      = \"%s\"",
            heartbeat_url[0] ? heartbeat_url : "(empty)");
    LOG_INF("  heartbeat/interval = %u s", heartbeat_interval);
    LOG_INF("  system/name        = \"%s\"", system_name);
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
