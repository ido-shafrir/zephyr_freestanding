/**
 * @file main.c
 * @brief ztest suite for config_store — integration tests on native_sim.
 *
 * The Zephyr settings subsystem runs with a real flash backend on
 * native_sim, so these tests exercise the actual config_store API
 * without any mocking.
 */

#include <zephyr/ztest.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include "config_store.h"

/* ---------- Fixture: init settings once ---------- */

struct config_store_fixture {
    bool initialised;
};

static void *config_store_setup(void)
{
    static struct config_store_fixture fixture;
    int rc = settings_subsys_init();
    zassert_equal(rc, 0, "settings_subsys_init failed: %d", rc);
    rc = settings_load();
    zassert_equal(rc, 0, "settings_load failed: %d", rc);
    fixture.initialised = true;
    return &fixture;
}

static void config_store_before(void *f)
{
    /* Restore factory defaults before each test for isolation */
    config_store_factory_reset();
}

ZTEST_SUITE(config_store_tests, NULL, config_store_setup,
            config_store_before, NULL, NULL);

/* ---------- Default values ---------- */

ZTEST(config_store_tests, test_default_heartbeat_enabled)
{
    zassert_false(config_store_get_heartbeat_enabled());
}

ZTEST(config_store_tests, test_default_heartbeat_interval)
{
    zassert_equal(config_store_get_heartbeat_interval(), 60);
}

ZTEST(config_store_tests, test_default_heartbeat_url_empty)
{
    char buf[CONFIG_STORE_URL_MAX_LEN];
    int len = config_store_get_heartbeat_url(buf, sizeof(buf));
    zassert_equal(len, 0, "Expected empty URL, got length %d", len);
    zassert_str_equal(buf, "");
}

ZTEST(config_store_tests, test_default_system_name)
{
    char buf[CONFIG_STORE_NAME_MAX_LEN];
    int len = config_store_get_system_name(buf, sizeof(buf));
    zassert_true(len > 0);
    zassert_str_equal(buf, "My-Device");
}

/* ---------- Set and get round-trips ---------- */

ZTEST(config_store_tests, test_set_get_heartbeat_enabled)
{
    zassert_equal(config_store_set_heartbeat_enabled(true), 0);
    zassert_true(config_store_get_heartbeat_enabled());

    zassert_equal(config_store_set_heartbeat_enabled(false), 0);
    zassert_false(config_store_get_heartbeat_enabled());
}

ZTEST(config_store_tests, test_set_get_heartbeat_interval)
{
    zassert_equal(config_store_set_heartbeat_interval(120), 0);
    zassert_equal(config_store_get_heartbeat_interval(), 120);
}

ZTEST(config_store_tests, test_set_get_heartbeat_url)
{
    const char *url = "http://example.com/webhook";
    zassert_equal(config_store_set_heartbeat_url(url), 0);

    char buf[CONFIG_STORE_URL_MAX_LEN];
    int len = config_store_get_heartbeat_url(buf, sizeof(buf));
    zassert_equal(len, (int)strlen(url));
    zassert_str_equal(buf, url);
}

ZTEST(config_store_tests, test_set_get_system_name)
{
    const char *name = "MyDevice";
    zassert_equal(config_store_set_system_name(name), 0);

    char buf[CONFIG_STORE_NAME_MAX_LEN];
    int len = config_store_get_system_name(buf, sizeof(buf));
    zassert_equal(len, (int)strlen(name));
    zassert_str_equal(buf, name);
}

/* ---------- Boundary conditions ---------- */

ZTEST(config_store_tests, test_interval_zero_rejected)
{
    zassert_equal(config_store_set_heartbeat_interval(0), -EINVAL);
    /* Value should remain at the default (60) since factory_reset ran */
    zassert_equal(config_store_get_heartbeat_interval(), 60);
}

ZTEST(config_store_tests, test_name_at_max_length_rejected)
{
    /* Build a string exactly CONFIG_STORE_NAME_MAX_LEN chars (too long) */
    char long_name[CONFIG_STORE_NAME_MAX_LEN + 1];
    memset(long_name, 'A', CONFIG_STORE_NAME_MAX_LEN);
    long_name[CONFIG_STORE_NAME_MAX_LEN] = '\0';
    zassert_equal(config_store_set_system_name(long_name), -EINVAL);
}

ZTEST(config_store_tests, test_name_at_max_minus_one_accepted)
{
    char name[CONFIG_STORE_NAME_MAX_LEN];
    memset(name, 'B', CONFIG_STORE_NAME_MAX_LEN - 1);
    name[CONFIG_STORE_NAME_MAX_LEN - 1] = '\0';
    zassert_equal(config_store_set_system_name(name), 0);

    char buf[CONFIG_STORE_NAME_MAX_LEN];
    config_store_get_system_name(buf, sizeof(buf));
    zassert_str_equal(buf, name);
}

ZTEST(config_store_tests, test_url_at_max_length_rejected)
{
    char long_url[CONFIG_STORE_URL_MAX_LEN + 1];
    memset(long_url, 'u', CONFIG_STORE_URL_MAX_LEN);
    long_url[CONFIG_STORE_URL_MAX_LEN] = '\0';
    zassert_equal(config_store_set_heartbeat_url(long_url), -EINVAL);
}

/* ---------- Factory reset ---------- */

ZTEST(config_store_tests, test_factory_reset_restores_defaults)
{
    /* Change everything */
    config_store_set_heartbeat_enabled(true);
    config_store_set_heartbeat_interval(999);
    config_store_set_heartbeat_url("http://changed.example.com");
    config_store_set_system_name("Changed");

    /* Reset */
    zassert_equal(config_store_factory_reset(), 0);

    /* Verify defaults */
    zassert_false(config_store_get_heartbeat_enabled());
    zassert_equal(config_store_get_heartbeat_interval(), 60);

    char buf[CONFIG_STORE_URL_MAX_LEN];
    config_store_get_heartbeat_url(buf, sizeof(buf));
    zassert_str_equal(buf, "");

    char name[CONFIG_STORE_NAME_MAX_LEN];
    config_store_get_system_name(name, sizeof(name));
    zassert_str_equal(name, "My-Device");
}
