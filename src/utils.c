#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "utils.h"

/**
 * @brief Validate an IPv4 address string in dotted-decimal notation.
 */
bool is_valid_ipv4(const char *str)
{
    if (str == NULL) {
        return false;
    }

    int octets = 0;
    const char *p = str;

    while (*p) {
        if (*p < '0' || *p > '9') {
            return false;
        }

        unsigned long val = 0;
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            p++;
        }

        if (val > 255 || digits > 3) {
            return false;
        }
        if (digits > 1 && *(p - digits) == '0') {
            return false;
        }

        octets++;

        if (*p == '.') {
            if (octets >= 4) {
                return false;
            }
            p++;
        } else if (*p != '\0') {
            return false;
        }
    }

    return octets == 4;
}

/**
 * @brief Validate an IPv4 netmask string.
 */
bool is_valid_netmask(const char *str)
{
    if (!is_valid_ipv4(str)) {
        return false;
    }

    const char *p = str;
    uint32_t mask = 0;

    for (int i = 0; i < 4; i++) {
        unsigned long val = 0;

        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        mask = (mask << 8) | (val & 0xFF);
        if (*p == '.') {
            p++;
        }
    }

    if (mask == 0) {
        return true;
    }

    uint32_t inv = ~mask;

    return (inv & (inv + 1)) == 0;
}

/**
 * @brief Validate a complete static IPv4 configuration.
 */
int validate_ip_config(const char *ip, const char *mask, const char *gw)
{
    if (!is_valid_ipv4(ip)) {
        return -1;
    }
    if (!is_valid_ipv4(mask)) {
        return -1;
    }
    if (!is_valid_netmask(mask)) {
        return -1;
    }
    if (!is_valid_ipv4(gw)) {
        return -1;
    }
    return 0;
}

/**
 * @brief Extract a JSON string value for the given key.
 */
int json_get_string(const char *json, size_t json_len,
                    const char *key, char *dst, size_t dst_size)
{
    if (json == NULL || key == NULL || dst == NULL || dst_size == 0) {
        return -1;
    }

    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) {
        return -1;
    }

    const char *p = NULL;
    if ((size_t)plen <= json_len) {
        for (size_t i = 0; i <= json_len - (size_t)plen; i++) {
            if (memcmp(json + i, pattern, (size_t)plen) == 0) {
                p = json + i;
                break;
            }
        }
    }
    if (p == NULL) {
        return -1;
    }

    const char *after_key = p + plen;
    const char *json_end = json + json_len;
    while (after_key < json_end && (*after_key == ' ' || *after_key == '\t' ||
           *after_key == '\n' || *after_key == '\r')) {
        after_key++;
    }
    if (after_key >= json_end || *after_key != ':') {
        return -1;
    }
    after_key++;

    const char *val_start = after_key;
    while (val_start < json_end && (*val_start == ' ' || *val_start == '\t' ||
           *val_start == '\n' || *val_start == '\r')) {
        val_start++;
    }

    if (val_start >= json_end || *val_start != '"') {
        return -1;
    }
    val_start++;

    const char *end = memchr(val_start, '"', (size_t)(json_end - val_start));
    if (end == NULL) {
        return -1;
    }

    size_t vlen = (size_t)(end - val_start);
    if (vlen >= dst_size) {
        vlen = dst_size - 1;
    }
    memcpy(dst, val_start, vlen);
    dst[vlen] = '\0';
    return 0;
}

/**
 * @brief Extract the wildcard segment from a URL matching a prefix/suffix pattern.
 *
 * Given a URL like "/api/device/mydev/info" with prefix "/api/device/" and
 * suffix "/info", extracts "mydev" into the output buffer.
 */
int parse_wildcard_name(const char *url, size_t url_len,
                        const char *prefix, const char *suffix,
                        char *name, size_t name_size)
{
    if (url == NULL || prefix == NULL || suffix == NULL ||
        name == NULL || name_size == 0) {
        return -1;
    }

    const size_t prefix_len = strlen(prefix);
    const size_t suffix_len = strlen(suffix);

    if (url_len <= prefix_len + suffix_len) {
        return -1;
    }
    if (memcmp(url, prefix, prefix_len) != 0) {
        return -1;
    }

    const char *tail = url + url_len - suffix_len;
    if (memcmp(tail, suffix, suffix_len) != 0) {
        return -1;
    }

    const char *name_start = url + prefix_len;
    size_t name_len = (size_t)(tail - name_start);
    if (name_len == 0) {
        return -1;
    }
    if (name_len >= name_size) {
        name_len = name_size - 1;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    return 0;
}
