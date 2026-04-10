#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "utils.h"

/**
 * @brief Validate an IPv4 address string in dotted-decimal notation.
 *
 * Checks that the string contains exactly 4 octets separated by dots,
 * each in range 0–255, with no leading zeros.
 *
 * @param str The string to validate (e.g. "192.168.1.1").
 * @return true if valid, false otherwise.
 */
bool is_valid_ipv4(const char *str)
{
    if (str == NULL) {
        return false;
    }

    int octets = 0;
    const char *p = str;

    while (*p) {
        /* Each octet must start with a digit */
        if (*p < '0' || *p > '9') {
            return false;
        }

        /* Parse the octet value */
        unsigned long val = 0;
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            p++;
        }

        /* Reject leading zeros (e.g. "01.02.03.04") and values > 255 */
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
 *
 * Parses the dotted-decimal string into a 32-bit value and checks that
 * it is a contiguous run of 1-bits followed by 0-bits (e.g. 255.255.255.0).
 * Also accepts 255.255.255.255 (host mask) and 0.0.0.0.
 *
 * @param str The netmask string to validate.
 * @return true if valid contiguous netmask, false otherwise.
 */
bool is_valid_netmask(const char *str)
{
    if (!is_valid_ipv4(str)) {
        return false;
    }

    /* Parse four octets into a 32-bit host-order value */
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

    /* A valid netmask in binary is all 1s followed by all 0s.
     * Inverting gives all 0s followed by all 1s.
     * Adding 1 to that should yield a power of 2 (single bit set),
     * or 0 if mask was 0xFFFFFFFF.
     */
    if (mask == 0) {
        return true;
    }

    uint32_t inv = ~mask;

    return (inv & (inv + 1)) == 0;
}
