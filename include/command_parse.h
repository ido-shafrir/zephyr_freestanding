#ifndef COMMAND_PARSE_H
#define COMMAND_PARSE_H

/**
 * @file command_parse.h
 * @brief Pure command-string parser for the UART interface.
 *
 * Converts a raw command string into a structured result (command ID
 * plus arguments) without performing any side effects.  This module
 * has no Zephyr dependencies and can be unit-tested on native_sim.
 */

#include <stddef.h>

/** Maximum length of a single argument extracted from a command. */
#define CMD_ARG_MAX_LEN 48

/** Recognised command identifiers. */
enum cmd_id {
    CMD_HELP,
    CMD_PING,
    CMD_IP_GET,
    CMD_IP_SET,
    CMD_IP_SET_DHCP,
    CMD_CONFIG_DUMP,
    CMD_CONFIG_FACTORY_RESET,
    CMD_EVENT_LOG,
    CMD_LOG_LEVEL_SET,
    CMD_LOG_DROP,
    CMD_TIME_GET,
    CMD_TIME_SET,
    CMD_TIME_SYNC,
    CMD_UNKNOWN,
};

/**
 * @brief Result of parsing a command string.
 *
 * `id` identifies the command.  For commands that accept positional
 * arguments (currently only `ip_set`) the first three space-separated
 * tokens after the keyword are stored in arg1 / arg2 / arg3.
 * Unused argument slots are zero-filled (empty strings).
 */
struct parsed_command {
    enum cmd_id id;
    char arg1[CMD_ARG_MAX_LEN];
    char arg2[CMD_ARG_MAX_LEN];
    char arg3[CMD_ARG_MAX_LEN];
};

/**
 * @brief Parse a null-terminated UART command string into a structured result.
 *
 * Leading whitespace is stripped.  The first token is matched against
 * known keywords (case-sensitive).  For `ip_set`, the three tokens
 * following the keyword are extracted into @p out->arg1 / arg2 / arg3.
 * If the input is empty after trimming, the command is reported as
 * CMD_UNKNOWN.  The function itself never fails on malformed input —
 * unrecognised strings yield CMD_UNKNOWN with return code 0.
 *
 * @param input Null-terminated command string received from UART.
 *              May be NULL (returns -1).
 * @param out   Output structure populated on return.
 *              May be NULL (returns -1).
 * @return 0 on success (including CMD_UNKNOWN), -1 only if a pointer is NULL.
 */
int parse_command(const char *input, struct parsed_command *out);

#endif /* COMMAND_PARSE_H */
