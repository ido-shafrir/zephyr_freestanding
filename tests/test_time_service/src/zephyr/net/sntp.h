/*
 * Minimal SNTP stub header for unit testing without networking stack.
 * Provides only the types and prototypes used by time_service.c.
 */

#ifndef ZEPHYR_INCLUDE_NET_SNTP_H_
#define ZEPHYR_INCLUDE_NET_SNTP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sntp_time {
    uint64_t seconds;
};

int sntp_simple(const char *server, uint32_t timeout, struct sntp_time *ts);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_SNTP_H_ */
