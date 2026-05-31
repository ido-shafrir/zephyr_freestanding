/**
 * @file stubs.h
 * @brief Stub declarations for time_service test dependencies.
 */

#ifndef TEST_STUBS_H
#define TEST_STUBS_H

#include <stdint.h>

/* Allow tests to control stub return values */
void stub_set_ntp_server(const char *server);
void stub_set_ntp_sync_interval(uint32_t interval);

#endif /* TEST_STUBS_H */
