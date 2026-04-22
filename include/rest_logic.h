#ifndef REST_LOGIC_H
#define REST_LOGIC_H

/**
 * @file rest_logic.h
 * @brief Pure request-parsing functions for the REST API.
 *
 * These functions extract and validate fields from JSON request
 * bodies without touching the HTTP layer. They depend only on
 * utils.h and can be unit-tested on native_sim.
 */

#include <stddef.h>

/**
 * @brief Parse a "set name" JSON request body.
 *
 * Extracts the value of the "name" key from the JSON body.
 *
 * @param body     JSON body string (not necessarily null-terminated).
 * @param body_len Length of the body in bytes.
 * @param name     Output buffer for the extracted name.
 * @param name_size Size of the output buffer.
 * @return 0 on success, -1 on missing / malformed field.
 */
int parse_set_name_request(const char *body, size_t body_len,
                           char *name, size_t name_size);

/**
 * @brief Parse a "set IP" JSON request body and validate the fields.
 *
 * Extracts "address", "mask", and "gateway" from the JSON body,
 * then validates them as a complete IPv4 configuration.
 *
 * @param body     JSON body string (not necessarily null-terminated).
 * @param body_len Length of the body in bytes.
 * @param addr     Output buffer for IP address.
 * @param mask     Output buffer for subnet mask.
 * @param gateway  Output buffer for gateway address.
 * @param buf_size Size of each output buffer.
 * @return 0 on success, -1 on missing field, -2 on validation failure.
 */
int parse_set_ip_request(const char *body, size_t body_len,
                         char *addr, char *mask, char *gateway,
                         size_t buf_size);

#endif /* REST_LOGIC_H */
