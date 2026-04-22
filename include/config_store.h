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
#define CONFIG_STORE_URL_MAX_LEN   256
#define CONFIG_STORE_NAME_MAX_LEN  64

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

/* ---------- Factory reset ---------- */

int config_store_factory_reset(void);

#endif /* CONFIG_STORE_H */
