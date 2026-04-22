/**
 * @file main.c
 * @brief ztest suite for rest_logic (parse_set_name_request, parse_set_ip_request).
 */

#include <zephyr/ztest.h>
#include <string.h>
#include "rest_logic.h"

/* ======================================================================
 * parse_set_name_request
 * ====================================================================== */

ZTEST_SUITE(set_name_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(set_name_tests, test_valid_name)
{
    const char body[] = "{\"name\":\"MyDevice\"}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), 0);
    zassert_str_equal(name, "MyDevice");
}

ZTEST(set_name_tests, test_name_with_spaces)
{
    const char body[] = "{\"name\": \"My Device 001\"}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), 0);
    zassert_str_equal(name, "My Device 001");
}

ZTEST(set_name_tests, test_missing_name_key)
{
    const char body[] = "{\"other\":\"value\"}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), -1);
}

ZTEST(set_name_tests, test_empty_body)
{
    const char body[] = "{}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), -1);
}

ZTEST(set_name_tests, test_null_body)
{
    char name[64];
    zassert_equal(parse_set_name_request(NULL, 0, name, sizeof(name)), -1);
}

ZTEST(set_name_tests, test_null_output)
{
    const char body[] = "{\"name\":\"x\"}";
    zassert_equal(parse_set_name_request(body, strlen(body), NULL, 64), -1);
}

ZTEST(set_name_tests, test_empty_name_value)
{
    const char body[] = "{\"name\":\"\"}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), 0);
    zassert_str_equal(name, "");
}

ZTEST(set_name_tests, test_zero_length_body)
{
    char name[64];
    zassert_equal(parse_set_name_request("", 0, name, sizeof(name)), -1);
}

ZTEST(set_name_tests, test_name_buf_size_zero)
{
    const char body[] = "{\"name\":\"x\"}";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, 0), -1);
}

ZTEST(set_name_tests, test_malformed_json)
{
    const char body[] = "this is not json";
    char name[64];
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), -1);
}

ZTEST(set_name_tests, test_name_value_truncation)
{
    const char body[] = "{\"name\":\"abcdefghijklmnop\"}";
    char name[5]; /* only room for 4 chars + NUL */
    zassert_equal(parse_set_name_request(body, strlen(body), name, sizeof(name)), 0);
    zassert_str_equal(name, "abcd");
}

/* ======================================================================
 * parse_set_ip_request
 * ====================================================================== */

ZTEST_SUITE(set_ip_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(set_ip_tests, test_valid_request)
{
    const char body[] =
        "{\"address\":\"192.168.0.88\",\"mask\":\"255.255.255.0\",\"gateway\":\"192.168.0.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), 0);
    zassert_str_equal(addr, "192.168.0.88");
    zassert_str_equal(mask, "255.255.255.0");
    zassert_str_equal(gw, "192.168.0.1");
}

ZTEST(set_ip_tests, test_missing_address)
{
    const char body[] = "{\"mask\":\"255.255.255.0\",\"gateway\":\"192.168.0.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_missing_mask)
{
    const char body[] = "{\"address\":\"192.168.0.88\",\"gateway\":\"192.168.0.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_missing_gateway)
{
    const char body[] = "{\"address\":\"192.168.0.88\",\"mask\":\"255.255.255.0\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_invalid_address_value)
{
    const char body[] =
        "{\"address\":\"999.1.1.1\",\"mask\":\"255.255.255.0\",\"gateway\":\"192.168.0.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -2);
}

ZTEST(set_ip_tests, test_invalid_mask_non_contiguous)
{
    const char body[] =
        "{\"address\":\"192.168.0.1\",\"mask\":\"255.0.255.0\",\"gateway\":\"192.168.0.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -2);
}

ZTEST(set_ip_tests, test_null_body)
{
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(NULL, 0,
                  addr, mask, gw, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_null_output_buffer)
{
    const char body[] =
        "{\"address\":\"1.2.3.4\",\"mask\":\"255.255.255.0\",\"gateway\":\"1.2.3.1\"}";
    char addr[16], mask[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, NULL, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_with_whitespace_json)
{
    const char body[] =
        "{ \"address\" : \"10.0.0.1\" , \"mask\" : \"255.255.0.0\" , \"gateway\" : \"10.0.0.254\" }";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), 0);
    zassert_str_equal(addr, "10.0.0.1");
}

ZTEST(set_ip_tests, test_zero_length_body)
{
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request("", 0,
                  addr, mask, gw, sizeof(addr)), -1);
}

ZTEST(set_ip_tests, test_buf_size_zero)
{
    const char body[] =
        "{\"address\":\"1.2.3.4\",\"mask\":\"255.255.255.0\",\"gateway\":\"1.2.3.1\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, 0), -1);
}

ZTEST(set_ip_tests, test_all_fields_invalid)
{
    const char body[] =
        "{\"address\":\"bad\",\"mask\":\"bad\",\"gateway\":\"bad\"}";
    char addr[16], mask[16], gw[16];
    zassert_equal(parse_set_ip_request(body, strlen(body),
                  addr, mask, gw, sizeof(addr)), -2);
}
