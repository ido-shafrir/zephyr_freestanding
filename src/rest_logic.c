/**
 * @file rest_logic.c
 * @brief Pure request-parsing logic — no Zephyr HTTP dependencies.
 *
 * Implementations of parse_set_name_request() and parse_set_ip_request().
 * Both rely on json_get_string() (utils.h) for JSON extraction and
 * validate_ip_config() for IP field validation.
 *
 * This module compiles on native_sim without stubs, enabling fast
 * host-based unit testing (see tests/test_rest_logic/).
 */

#include <string.h>
#include "rest_logic.h"
#include "utils.h"

int parse_set_name_request(const char *body, size_t body_len,
                           char *name, size_t name_size)
{
    if (body == NULL || name == NULL || name_size == 0) {
        return -1;
    }
    return json_get_string(body, body_len, "name", name, name_size);
}

int parse_set_ip_request(const char *body, size_t body_len,
                         char *addr, char *mask, char *gateway,
                         size_t buf_size)
{
    if (body == NULL || addr == NULL || mask == NULL ||
        gateway == NULL || buf_size == 0) {
        return -1;
    }

    if (json_get_string(body, body_len, "address", addr, buf_size) != 0 ||
        json_get_string(body, body_len, "mask", mask, buf_size) != 0 ||
        json_get_string(body, body_len, "gateway", gateway, buf_size) != 0) {
        return -1;
    }

    if (validate_ip_config(addr, mask, gateway) != 0) {
        return -2;
    }

    return 0;
}
