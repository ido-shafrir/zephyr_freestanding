/**
 * @file command_parse.c
 * @brief Pure command-string parser — no Zephyr or hardware dependencies.
 *
 * Implements parse_command(): strips leading whitespace, identifies the
 * keyword by exact match, and extracts up to three space-separated
 * arguments for commands that need them (currently only `ip_set`).
 *
 * Because this module depends only on <string.h>, it compiles and
 * runs on native_sim (host unit tests) without any stubs.
 */

#include <string.h>
#include "command_parse.h"

int parse_command(const char *input, struct parsed_command *out)
{
    if (input == NULL || out == NULL) {
        return -1;
    }

    /* Zero the output */
    memset(out, 0, sizeof(*out));

    /* Skip leading whitespace */
    while (*input == ' ' || *input == '\t') {
        input++;
    }

    if (*input == '\0') {
        out->id = CMD_UNKNOWN;
        return 0;
    }

    /* Simple keyword matching */
    if (strcmp(input, "help") == 0 ||
        strcmp(input, "h") == 0 ||
        strcmp(input, "?") == 0) {
        out->id = CMD_HELP;
    } else if (strcmp(input, "ping") == 0) {
        out->id = CMD_PING;
    } else if (strcmp(input, "ip_get") == 0) {
        out->id = CMD_IP_GET;
    } else if (strncmp(input, "ip_set ", 7) == 0) {
        out->id = CMD_IP_SET;

        /* Parse up to 3 space-separated arguments */
        char buf[CMD_ARG_MAX_LEN * 3 + 3];
        strncpy(buf, input + 7, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *saveptr;
        char *tok;

        tok = strtok_r(buf, " ", &saveptr);
        if (tok) {
            strncpy(out->arg1, tok, CMD_ARG_MAX_LEN - 1);
        }
        tok = strtok_r(NULL, " ", &saveptr);
        if (tok) {
            strncpy(out->arg2, tok, CMD_ARG_MAX_LEN - 1);
        }
        tok = strtok_r(NULL, " ", &saveptr);
        if (tok) {
            strncpy(out->arg3, tok, CMD_ARG_MAX_LEN - 1);
        }
    } else if (strcmp(input, "ip_set") == 0) {
        /* ip_set with no args — still identified, dispatch handles error */
        out->id = CMD_IP_SET;
    } else if (strcmp(input, "ip_set_dhcp") == 0) {
        out->id = CMD_IP_SET_DHCP;
    } else if (strcmp(input, "config_dump") == 0) {
        out->id = CMD_CONFIG_DUMP;
    } else if (strcmp(input, "config_factory_reset") == 0) {
        out->id = CMD_CONFIG_FACTORY_RESET;
    } else {
        out->id = CMD_UNKNOWN;
    }

    return 0;
}
