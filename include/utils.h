#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>

/**
 * Validate an IPv4 address string (e.g. "192.168.1.1").
 * Returns true if the string is a valid dotted-decimal IPv4 address.
 */
bool is_valid_ipv4(const char *str);

/**
 * Validate an IPv4 netmask string (e.g. "255.255.255.0").
 * A valid netmask is a contiguous run of 1-bits followed by 0-bits.
 * Returns true if valid, false otherwise.
 */
bool is_valid_netmask(const char *str);

#endif /* UTILS_H */
