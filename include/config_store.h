#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

/**
 * @file config_store.h
 * @brief Persistent configuration store using Zephyr's settings subsystem.
 *
 * Provides persistent key-value storage backed by ZMS flash. All configs
 * survive reboots and firmware updates (the storage partition is outside
 * the MCUboot slot0/slot1 swap area).
 *
 * Architecture:
 *   - Each config group (e.g. "heartbeat", "system") is a settings subtree.
 *   - The init thread calls settings_subsys_init -> settings_load -> dump.
 *   - Setters persist immediately via settings_save_one().
 *   - Thread-safe: a mutex protects all config variables.
 *
 * Adding a new config:
 *   1. Add a variable with a default in config_store.c
 *   2. Add a case in the h_set callback (for load from flash)
 *   3. Add an export line in h_export (for save-all)
 *   4. Add getter/setter functions
 *   5. Add a line to config_store_dump()
 */

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------- Thread configuration ---------- */
#define CONFIG_STORE_STACK_SIZE  4096
#define CONFIG_STORE_PRIORITY    6

/* ---------- String length limits ---------- */
#define CONFIG_STORE_URL_MAX_LEN         256
#define CONFIG_STORE_NAME_MAX_LEN        64
#define CONFIG_STORE_NTP_SERVER_MAX_LEN  64
#define CONFIG_STORE_IP_ADDR_MAX_LEN    16  /* "255.255.255.255\0" */

/* ---------- Log verbosity (used by event_log) ---------- */

/**
 * @brief Log verbosity levels persisted under "event_log/logVerbosity".
 *
 * Maps to event_log severity filter:
 *   ERROR → drops WARN/NOTICE/INFO/DEBUG
 *   WARN  → drops NOTICE/INFO/DEBUG
 *   INFO  → drops only DEBUG
 *   DEBUG → logs everything
 */
enum mcu_log_verbosity {
    MCU_LOG_ERROR = 0,
    MCU_LOG_WARN  = 1,
    MCU_LOG_INFO  = 2,
    MCU_LOG_DEBUG = 3,
};

/* ---------- Thread resources (defined in config_store.c) ---------- */
extern struct k_thread config_store_thread_data;
extern k_thread_stack_t config_store_stack[];

void config_store_thread_entry(void *p1, void *p2, void *p3);
void config_store_dump(void);

/* ---------- heartbeat/ getters & setters ---------- */

bool config_store_get_heartbeat_enabled(void);
int config_store_set_heartbeat_enabled(bool enabled);
int config_store_get_heartbeat_url(char *buf, size_t buf_len);
int config_store_set_heartbeat_url(const char *url);
uint32_t config_store_get_heartbeat_interval(void);
int config_store_set_heartbeat_interval(uint32_t interval_s);

/* ---------- system/ getters & setters ---------- */

int config_store_get_system_name(char *buf, size_t buf_len);
int config_store_set_system_name(const char *name);

/* ---------- event_log/ getters & setters ---------- */

enum mcu_log_verbosity config_store_get_log_verbosity(void);
int config_store_set_log_verbosity(enum mcu_log_verbosity v);
uint32_t config_store_get_boot_count(void);
int config_store_set_boot_count(uint32_t count);

/* ---------- time_service/ getters & setters ---------- */

int config_store_get_ntp_server(char *buf, size_t buf_len);
int config_store_set_ntp_server(const char *server);
uint32_t config_store_get_ntp_sync_interval(void);
int config_store_set_ntp_sync_interval(uint32_t seconds);

/* ---------- network/ getters & setters ---------- */

int config_store_get_ip_address(char *buf, size_t buf_len);
int config_store_set_ip_address(const char *addr);
int config_store_get_ip_mask(char *buf, size_t buf_len);
int config_store_set_ip_mask(const char *mask);
int config_store_get_ip_gateway(char *buf, size_t buf_len);
int config_store_set_ip_gateway(const char *gw);

/* ---------- Factory reset ---------- */

int config_store_factory_reset(void);

#endif /* CONFIG_STORE_H */
