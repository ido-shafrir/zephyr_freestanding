/**
 * @file main.c
 * @brief ztest suite for command_parse (parse_command).
 */

#include <zephyr/ztest.h>
#include <string.h>
#include "command_parse.h"

ZTEST_SUITE(command_parse_tests, NULL, NULL, NULL, NULL, NULL);

/* ---------- Help variants ---------- */

ZTEST(command_parse_tests, test_help)
{
    struct parsed_command pc;
    zassert_equal(parse_command("help", &pc), 0);
    zassert_equal(pc.id, CMD_HELP);
}

ZTEST(command_parse_tests, test_help_h)
{
    struct parsed_command pc;
    zassert_equal(parse_command("h", &pc), 0);
    zassert_equal(pc.id, CMD_HELP);
}

ZTEST(command_parse_tests, test_help_question)
{
    struct parsed_command pc;
    zassert_equal(parse_command("?", &pc), 0);
    zassert_equal(pc.id, CMD_HELP);
}

/* ---------- Basic commands ---------- */

ZTEST(command_parse_tests, test_ping)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ping", &pc), 0);
    zassert_equal(pc.id, CMD_PING);
}

ZTEST(command_parse_tests, test_ip_get)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ip_get", &pc), 0);
    zassert_equal(pc.id, CMD_IP_GET);
}

ZTEST(command_parse_tests, test_ip_set_dhcp)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ip_set_dhcp", &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET_DHCP);
}

ZTEST(command_parse_tests, test_config_dump)
{
    struct parsed_command pc;
    zassert_equal(parse_command("config_dump", &pc), 0);
    zassert_equal(pc.id, CMD_CONFIG_DUMP);
}

ZTEST(command_parse_tests, test_config_factory_reset)
{
    struct parsed_command pc;
    zassert_equal(parse_command("config_factory_reset", &pc), 0);
    zassert_equal(pc.id, CMD_CONFIG_FACTORY_RESET);
}

/* ---------- ip_set with arguments ---------- */

ZTEST(command_parse_tests, test_ip_set_with_args)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ip_set 192.168.0.1 255.255.255.0 192.168.0.254", &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET);
    zassert_str_equal(pc.arg1, "192.168.0.1");
    zassert_str_equal(pc.arg2, "255.255.255.0");
    zassert_str_equal(pc.arg3, "192.168.0.254");
}

ZTEST(command_parse_tests, test_ip_set_no_args)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ip_set", &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET);
    zassert_str_equal(pc.arg1, "");
}

ZTEST(command_parse_tests, test_ip_set_partial_args)
{
    struct parsed_command pc;
    zassert_equal(parse_command("ip_set 192.168.0.1", &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET);
    zassert_str_equal(pc.arg1, "192.168.0.1");
    zassert_str_equal(pc.arg2, "");
    zassert_str_equal(pc.arg3, "");
}

/* ---------- Unknown / edge cases ---------- */

ZTEST(command_parse_tests, test_unknown_command)
{
    struct parsed_command pc;
    zassert_equal(parse_command("gibberish", &pc), 0);
    zassert_equal(pc.id, CMD_UNKNOWN);
}

ZTEST(command_parse_tests, test_empty_string)
{
    struct parsed_command pc;
    zassert_equal(parse_command("", &pc), 0);
    zassert_equal(pc.id, CMD_UNKNOWN);
}

ZTEST(command_parse_tests, test_whitespace_only)
{
    struct parsed_command pc;
    zassert_equal(parse_command("   ", &pc), 0);
    zassert_equal(pc.id, CMD_UNKNOWN);
}

ZTEST(command_parse_tests, test_null_input)
{
    struct parsed_command pc;
    zassert_equal(parse_command(NULL, &pc), -1);
}

ZTEST(command_parse_tests, test_null_output)
{
    zassert_equal(parse_command("ping", NULL), -1);
}

ZTEST(command_parse_tests, test_leading_whitespace)
{
    struct parsed_command pc;
    zassert_equal(parse_command("  ping", &pc), 0);
    zassert_equal(pc.id, CMD_PING);
}

ZTEST(command_parse_tests, test_case_sensitive)
{
    struct parsed_command pc;
    zassert_equal(parse_command("PING", &pc), 0);
    zassert_equal(pc.id, CMD_UNKNOWN, "Commands should be case-sensitive");
}

ZTEST(command_parse_tests, test_command_with_trailing_garbage)
{
    struct parsed_command pc;
    /* "ping extra" is not "ping" — should be unknown */
    zassert_equal(parse_command("ping extra", &pc), 0);
    zassert_equal(pc.id, CMD_UNKNOWN);
}

ZTEST(command_parse_tests, test_ip_set_extra_args_ignored)
{
    struct parsed_command pc;
    /* 4th arg should be silently ignored */
    zassert_equal(parse_command("ip_set 1.1.1.1 2.2.2.2 3.3.3.3 extra", &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET);
    zassert_str_equal(pc.arg1, "1.1.1.1");
    zassert_str_equal(pc.arg2, "2.2.2.2");
    zassert_str_equal(pc.arg3, "3.3.3.3");
}

ZTEST(command_parse_tests, test_ip_set_arg_too_long_truncated)
{
    struct parsed_command pc;
    /* Build an arg longer than CMD_ARG_MAX_LEN */
    char cmd[256];
    memset(cmd, 0, sizeof(cmd));
    strcpy(cmd, "ip_set ");
    memset(cmd + 7, 'A', CMD_ARG_MAX_LEN + 10);
    cmd[7 + CMD_ARG_MAX_LEN + 10] = '\0';
    zassert_equal(parse_command(cmd, &pc), 0);
    zassert_equal(pc.id, CMD_IP_SET);
    /* arg1 should be truncated to CMD_ARG_MAX_LEN - 1 */
    zassert_equal(strlen(pc.arg1), CMD_ARG_MAX_LEN - 1);
}
