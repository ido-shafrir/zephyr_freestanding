#ifndef UTILS_H
#define UTILS_H

/**
 * @file utils.h
 * @brief Shared pure-logic utility functions.
 *
 * All functions here are free of Zephyr / hardware dependencies
 * so they can be compiled and unit-tested on native_sim.
 */

#include <stdbool.h>
#include <stddef.h>

/* ---------- IPv4 validation ---------- */

bool is_valid_ipv4(const char *str);
bool is_valid_netmask(const char *str);
int validate_ip_config(const char *ip, const char *mask, const char *gw);

/* ---------- JSON helpers ---------- */

int json_get_string(const char *json, size_t json_len,
                    const char *key, char *dst, size_t dst_size);

/* ---------- URL parsing ---------- */

int parse_wildcard_name(const char *url, size_t url_len,
                        const char *prefix, const char *suffix,
                        char *name, size_t name_size);

#endif /* UTILS_H */
