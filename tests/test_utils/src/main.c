/**
 * @file main.c
 * @brief ztest suite for utility functions (is_valid_ipv4, is_valid_netmask,
 *        json_get_string, parse_wildcard_name, validate_ip_config).
 */

#include <zephyr/ztest.h>
#include "utils.h"

/* ======================================================================
 * is_valid_ipv4
 * ====================================================================== */

ZTEST_SUITE(ipv4_validation, NULL, NULL, NULL, NULL, NULL);

ZTEST(ipv4_validation, test_valid_addresses)
{
    zassert_true(is_valid_ipv4("0.0.0.0"));
    zassert_true(is_valid_ipv4("192.168.1.1"));
    zassert_true(is_valid_ipv4("255.255.255.255"));
    zassert_true(is_valid_ipv4("10.0.0.1"));
    zassert_true(is_valid_ipv4("1.2.3.4"));
}

ZTEST(ipv4_validation, test_invalid_null_and_empty)
{
    zassert_false(is_valid_ipv4(NULL));
    zassert_false(is_valid_ipv4(""));
}

ZTEST(ipv4_validation, test_invalid_too_many_octets)
{
    zassert_false(is_valid_ipv4("1.2.3.4.5"));
}

ZTEST(ipv4_validation, test_invalid_too_few_octets)
{
    zassert_false(is_valid_ipv4("1.2.3"));
    zassert_false(is_valid_ipv4("1"));
}

ZTEST(ipv4_validation, test_invalid_leading_zeros)
{
    zassert_false(is_valid_ipv4("01.02.03.04"));
    zassert_false(is_valid_ipv4("192.168.01.1"));
}

ZTEST(ipv4_validation, test_invalid_octet_over_255)
{
    zassert_false(is_valid_ipv4("256.1.1.1"));
    zassert_false(is_valid_ipv4("1.1.1.999"));
}

ZTEST(ipv4_validation, test_invalid_trailing_dot)
{
    zassert_false(is_valid_ipv4("1.2.3.4."));
    zassert_false(is_valid_ipv4("1.2.3."));
}

ZTEST(ipv4_validation, test_invalid_non_numeric)
{
    zassert_false(is_valid_ipv4("a.b.c.d"));
    zassert_false(is_valid_ipv4("192.168.1.x"));
}

ZTEST(ipv4_validation, test_invalid_double_dot)
{
    zassert_false(is_valid_ipv4("1..2.3.4"));
}

ZTEST(ipv4_validation, test_invalid_spaces)
{
    zassert_false(is_valid_ipv4(" 192.168.1.1"));
    zassert_false(is_valid_ipv4("192.168.1.1 "));
    zassert_false(is_valid_ipv4("192. 168.1.1"));
}

ZTEST(ipv4_validation, test_invalid_negative)
{
    zassert_false(is_valid_ipv4("-1.0.0.0"));
}

ZTEST(ipv4_validation, test_invalid_hex_octet)
{
    zassert_false(is_valid_ipv4("0xFF.0.0.1"));
}

/* ======================================================================
 * is_valid_netmask
 * ====================================================================== */

ZTEST_SUITE(netmask_validation, NULL, NULL, NULL, NULL, NULL);

ZTEST(netmask_validation, test_valid_masks)
{
    zassert_true(is_valid_netmask("255.255.255.0"));
    zassert_true(is_valid_netmask("255.255.0.0"));
    zassert_true(is_valid_netmask("255.0.0.0"));
    zassert_true(is_valid_netmask("255.255.255.255"));
    zassert_true(is_valid_netmask("0.0.0.0"));
    zassert_true(is_valid_netmask("255.255.255.128"));
    zassert_true(is_valid_netmask("255.255.254.0"));
}

ZTEST(netmask_validation, test_invalid_non_contiguous)
{
    zassert_false(is_valid_netmask("255.255.0.255"));
    zassert_false(is_valid_netmask("255.0.255.0"));
}

ZTEST(netmask_validation, test_invalid_not_an_ip)
{
    zassert_false(is_valid_netmask("not_a_mask"));
    zassert_false(is_valid_netmask(NULL));
}

/* ======================================================================
 * validate_ip_config
 * ====================================================================== */

ZTEST_SUITE(ip_config_validation, NULL, NULL, NULL, NULL, NULL);

ZTEST(ip_config_validation, test_valid_config)
{
    zassert_equal(validate_ip_config("192.168.0.1", "255.255.255.0", "192.168.0.254"), 0);
}

ZTEST(ip_config_validation, test_invalid_ip)
{
    zassert_equal(validate_ip_config("999.1.1.1", "255.255.255.0", "192.168.0.1"), -1);
}

ZTEST(ip_config_validation, test_invalid_mask)
{
    zassert_equal(validate_ip_config("192.168.0.1", "255.0.255.0", "192.168.0.1"), -1);
}

ZTEST(ip_config_validation, test_invalid_gateway)
{
    zassert_equal(validate_ip_config("192.168.0.1", "255.255.255.0", "bad"), -1);
}

ZTEST(ip_config_validation, test_null_args)
{
    zassert_equal(validate_ip_config(NULL, "255.255.255.0", "192.168.0.1"), -1);
    zassert_equal(validate_ip_config("192.168.0.1", NULL, "192.168.0.1"), -1);
    zassert_equal(validate_ip_config("192.168.0.1", "255.255.255.0", NULL), -1);
}

/* ======================================================================
 * json_get_string
 * ====================================================================== */

ZTEST_SUITE(json_get_string_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(json_get_string_tests, test_basic_extraction)
{
    const char json[] = "{\"name\":\"hello\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "name", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "hello");
}

ZTEST(json_get_string_tests, test_with_whitespace_after_colon)
{
    const char json[] = "{\"key\":  \"value\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "value");
}

ZTEST(json_get_string_tests, test_with_spaces_and_tabs)
{
    const char json[] = "{\"key\":\t \n\"spaced\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "spaced");
}

ZTEST(json_get_string_tests, test_missing_key)
{
    const char json[] = "{\"name\":\"hello\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "missing", buf, sizeof(buf)), -1);
}

ZTEST(json_get_string_tests, test_empty_value)
{
    const char json[] = "{\"key\":\"\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "");
}

ZTEST(json_get_string_tests, test_no_closing_quote)
{
    const char json[] = "{\"key\":\"noclosing";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), -1);
}

ZTEST(json_get_string_tests, test_value_not_a_string)
{
    const char json[] = "{\"key\":42}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), -1);
}

ZTEST(json_get_string_tests, test_multiple_keys)
{
    const char json[] = "{\"a\":\"1\",\"b\":\"2\",\"c\":\"3\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "b", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "2");
    zassert_equal(json_get_string(json, strlen(json), "c", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "3");
}

ZTEST(json_get_string_tests, test_null_inputs)
{
    char buf[32];
    zassert_equal(json_get_string(NULL, 0, "key", buf, sizeof(buf)), -1);
    zassert_equal(json_get_string("{}", 2, NULL, buf, sizeof(buf)), -1);
    zassert_equal(json_get_string("{}", 2, "key", NULL, sizeof(buf)), -1);
    zassert_equal(json_get_string("{}", 2, "key", buf, 0), -1);
}

ZTEST(json_get_string_tests, test_buffer_truncation)
{
    const char json[] = "{\"key\":\"longvalue\"}";
    char buf[5]; /* only room for 4 chars + NUL */
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "long");
}

ZTEST(json_get_string_tests, test_whitespace_before_colon)
{
    const char json[] = "{\"key\" : \"val\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), 0);
    zassert_str_equal(buf, "val");
}

ZTEST(json_get_string_tests, test_zero_length_json)
{
    char buf[32];
    zassert_equal(json_get_string("", 0, "key", buf, sizeof(buf)), -1);
}

ZTEST(json_get_string_tests, test_malformed_json_no_braces)
{
    const char json[] = "not json at all";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "key", buf, sizeof(buf)), -1);
}

ZTEST(json_get_string_tests, test_key_substring_no_false_match)
{
    /* "name" should not match "username" */
    const char json[] = "{\"username\":\"admin\"}";
    char buf[32];
    zassert_equal(json_get_string(json, strlen(json), "name", buf, sizeof(buf)), -1);
}

/* ======================================================================
 * parse_wildcard_name
 * ====================================================================== */

#define PFX "/api/device/"
#define SFX "/info"

ZTEST_SUITE(parse_wildcard_name_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(parse_wildcard_name_tests, test_valid_url)
{
    const char url[] = "/api/device/sensor1/info";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), 0);
    zassert_str_equal(name, "sensor1");
}

ZTEST(parse_wildcard_name_tests, test_single_char_name)
{
    const char url[] = "/api/device/x/info";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), 0);
    zassert_str_equal(name, "x");
}

ZTEST(parse_wildcard_name_tests, test_missing_suffix)
{
    const char url[] = "/api/device/sensor1";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_empty_name)
{
    const char url[] = "/api/device//info";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_wrong_prefix)
{
    const char url[] = "/api/other/sensor1/info";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_null_inputs)
{
    char name[64];
    zassert_equal(parse_wildcard_name(NULL, 0, PFX, SFX, name, sizeof(name)), -1);
    zassert_equal(parse_wildcard_name("/api/device/x/info", 18, PFX, SFX, NULL, 64), -1);
    zassert_equal(parse_wildcard_name("/api/device/x/info", 18, PFX, SFX, name, 0), -1);
    zassert_equal(parse_wildcard_name("/api/device/x/info", 18, NULL, SFX, name, sizeof(name)), -1);
    zassert_equal(parse_wildcard_name("/api/device/x/info", 18, PFX, NULL, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_url_too_short)
{
    const char url[] = "/api/device/";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_only_prefix_and_suffix)
{
    /* prefix + suffix with nothing between -> empty name -> reject */
    const char url[] = "/api/device//info";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}

ZTEST(parse_wildcard_name_tests, test_name_truncation)
{
    /* Name longer than the output buffer */
    const char url[] = "/api/device/very_long_device_name/info";
    char name[5]; /* only room for 4 chars + NUL */
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), 0);
    zassert_str_equal(name, "very");
}

ZTEST(parse_wildcard_name_tests, test_completely_wrong_url)
{
    const char url[] = "/something/else/entirely";
    char name[64];
    zassert_equal(parse_wildcard_name(url, strlen(url), PFX, SFX, name, sizeof(name)), -1);
}
