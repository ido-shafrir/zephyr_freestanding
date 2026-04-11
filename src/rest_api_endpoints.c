/**
 * @file rest_api_endpoints.c
 * @brief Business-logic HTTP endpoint implementations.
 *
 * Each handler follows the http_resource_dynamic_cb_t signature,
 * builds a JSON response in a scratch buffer, and sends it via
 * send_json_response() (or a local helper).
 *
 * Endpoints:
 *   POST /api/echo            – echo the JSON payload
 *   GET  /api/ion/<name>/test – return the ion name
 *   POST /api/set_ip_dhcp     – switch to DHCP
 *   POST /api/set_ip          – set a static IPv4 address
 */

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "rest_api.h"
#include "rest_api_endpoints.h"
#include "w5500_net.h"

LOG_MODULE_REGISTER(rest_api_ep, LOG_LEVEL_DBG);

/**
 * @brief Log an incoming HTTP request (method + URL).
 *
 * Call once per request, typically at the DATA_FINAL entry point.
 */
#define LOG_REQUEST(client) \
    LOG_INF("%s %s", http_method_str(client->method), client->url_buffer)

/* ==========================================================================
 * Delayed HTTP server restart – used after IP changes
 *
 * When the IP address changes, the TCP listening socket's internal
 * net_context becomes stale and can't accept new connections. We
 * restart the server from a delayed work item so that the current
 * response finishes sending before teardown.
 * ========================================================================== */

/**
 * @brief Work handler that stops and restarts the HTTP server.
 *
 * Scheduled by set_ip / set_ip_dhcp handlers after sending the
 * response. The 500 ms delay gives the response time to complete
 * on the old connection before the listening socket is torn down.
 *
 * @param work Pointer to the k_work_delayable struct.
 */
static void http_restart_work_handler(struct k_work *work)
{
    LOG_INF("Restarting HTTP server after IP change...");
    http_server_stop();

    /*
     * Wait for the server thread to fully shut down. After stop()
     * signals the eventfd, the server thread still needs to:
     *   1. Exit http_server_run() poll loop
     *   2. close_all_sockets()
     *   3. Loop back to k_sem_take(&server_start)
     *
     * Only then can http_server_start() safely give the semaphore.
     * 1 second is generous enough for the thread to complete.
     */
    k_msleep(1000);

    int ret = http_server_start();
    if (ret < 0) {
        LOG_ERR("Failed to restart HTTP server: %d", ret);
    } else {
        LOG_INF("HTTP server restarted successfully");
    }
}

static K_WORK_DELAYABLE_DEFINE(http_restart_work, http_restart_work_handler);

/* ==========================================================================
 * Helper: common JSON response sender (local copy – keeps module standalone)
 * ========================================================================== */

/**
 * @brief Send a JSON response with the given status code and body.
 *
 * Sets Content-Type to application/json and marks the response as complete.
 *
 * @param response_ctx Response context to populate.
 * @param status       HTTP status code.
 * @param json         JSON string to send.
 * @param json_len     Length of the JSON string.
 */
static void send_json(struct http_response_ctx *response_ctx,
                      enum http_status status,
                      const char *json, size_t json_len)
{
    static const struct http_header json_hdrs[] = {
        { .name = "Content-Type", .value = "application/json" },
    };

    response_ctx->status = status;
    response_ctx->headers = json_hdrs;
    response_ctx->header_count = ARRAY_SIZE(json_hdrs);
    response_ctx->body = (const uint8_t *)json;
    response_ctx->body_len = json_len;
    response_ctx->final_chunk = true;
}

/* ==========================================================================
 * Endpoint: POST /api/echo
 * ========================================================================== */

/** @brief Scratch buffer for /api/echo response. */
static char echo_buf[512];

/** @brief Receive buffer for accumulating /api/echo request body. */
static char echo_recv_buf[256];
static size_t echo_recv_len;

/**
 * @brief Handler for POST /api/echo.
 *
 * Receives a JSON payload and returns it wrapped as:
 *   {"result": <original payload>}
 *
 * This is a test endpoint and will be removed later.
 */
static int api_echo_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
        /* First chunk — reset and start accumulating */
        size_t copy = request_ctx->data_len;
        if (copy > sizeof(echo_recv_buf)) {
            copy = sizeof(echo_recv_buf);
        }
        memcpy(echo_recv_buf, request_ctx->data, copy);
        echo_recv_len = copy;
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        /* Append any remaining data from this final chunk */
        if (request_ctx->data_len > 0) {
            size_t avail = sizeof(echo_recv_buf) - echo_recv_len;
            size_t copy = (request_ctx->data_len < avail)
                          ? request_ctx->data_len : avail;
            memcpy(echo_recv_buf + echo_recv_len, request_ctx->data, copy);
            echo_recv_len += copy;
        }

        int off = snprintf(echo_buf, sizeof(echo_buf), "{\"result\":");

        size_t avail = sizeof(echo_buf) - off - 2; /* room for '}' + NUL */
        size_t copy = (echo_recv_len < avail) ? echo_recv_len : avail;
        memcpy(echo_buf + off, echo_recv_buf, copy);
        off += (int)copy;

        off += snprintf(echo_buf + off, sizeof(echo_buf) - off, "}");

        send_json(response_ctx, HTTP_200_OK, echo_buf, off);
        echo_recv_len = 0;
    }

    return 0;
}

static struct http_resource_detail_dynamic api_echo_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .path_len = sizeof("/api/echo") - 1,
    },
    .cb = api_echo_handler,
};

HTTP_RESOURCE_DEFINE(api_echo_res, icb_api, "/api/echo", &api_echo_detail);

ENDPOINT_ENTRY_DEFINE(ep_echo, "/api/echo", "POST",
    "Echoes the JSON payload back as {\"result\": <payload>}.");

/* ==========================================================================
 * Endpoint: GET /api/ion/<name>/test
 *
 * Registered as a wildcard on "/api/ion/" — the handler parses
 * the <name> segment from the URL itself.
 * ========================================================================== */

/** @brief Scratch buffer for /api/ion response. */
static char ion_buf[256];

/**
 * @brief Handler for GET /api/ion/<name>/test.
 *
 * Extracts the ion name from the URL path and returns:
 *   {"result": "<name>"}
 *
 * Expects the URL to be exactly /api/ion/<name>/test.
 * Returns 400 if the URL format is wrong.
 */
static int api_ion_test_handler(struct http_client_ctx *client,
                                enum http_transaction_status status,
                                const struct http_request_ctx *request_ctx,
                                struct http_response_ctx *response_ctx,
                                void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        const char *url = (const char *)client->url_buffer;
        size_t url_len = strlen(url);

        /* Expected format: /api/ion/<name>/test
         * Skip the leading "/api/ion/" (9 chars) */
        const char prefix[] = "/api/ion/";
        const size_t prefix_len = sizeof(prefix) - 1;

        if (url_len <= prefix_len ||
            memcmp(url, prefix, prefix_len) != 0) {
            const char err[] = "{\"error\":\"invalid URL format\"}";
            send_json(response_ctx, HTTP_400_BAD_REQUEST,
                      err, sizeof(err) - 1);
            return 0;
        }

        /* Find the ion name between "/api/ion/" and "/test" */
        const char *name_start = url + prefix_len;
        const char *name_end = NULL;
        const char suffix[] = "/test";
        size_t suffix_len = sizeof(suffix) - 1;

        if (url_len > prefix_len + suffix_len) {
            const char *tail = url + url_len - suffix_len;
            if (memcmp(tail, suffix, suffix_len) == 0) {
                name_end = tail;
            }
        }

        if (name_end == NULL || name_end <= name_start) {
            const char err[] = "{\"error\":\"URL must be /api/ion/<name>/test\"}";
            send_json(response_ctx, HTTP_400_BAD_REQUEST,
                      err, sizeof(err) - 1);
            return 0;
        }

        size_t name_len = name_end - name_start;
        int off = snprintf(ion_buf, sizeof(ion_buf),
                           "{\"result\":\"%.*s\"}",
                           (int)name_len, name_start);

        send_json(response_ctx, HTTP_200_OK, ion_buf, off);
    }

    return 0;
}

static struct http_resource_detail_dynamic api_ion_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/api/ion/*/test") - 1,
    },
    .cb = api_ion_test_handler,
};

HTTP_RESOURCE_DEFINE(api_ion_res, icb_api, "/api/ion/*/test", &api_ion_detail);

ENDPOINT_ENTRY_DEFINE(ep_ion_test, "/api/ion/{name}/test", "GET",
    "Returns {\"result\": \"{name}\"}.");

/* ==========================================================================
 * Endpoint: POST /api/set_ip_dhcp
 * ========================================================================== */

/**
 * @brief Handler for POST /api/set_ip_dhcp.
 *
 * Calls net_set_dhcp() to switch the network interface to DHCP mode.
 * Returns {"result": "success"} on success, or
 *         {"result": "failure", "error": "<msg>"} on error.
 */
static int api_set_ip_dhcp_handler(struct http_client_ctx *client,
                                   enum http_transaction_status status,
                                   const struct http_request_ctx *request_ctx,
                                   struct http_response_ctx *response_ctx,
                                   void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        int ret = net_set_dhcp();

        if (ret == 0) {
            static const char ok[] = "{\"result\":\"success\"}";
            send_json(response_ctx, HTTP_200_OK, ok, sizeof(ok) - 1);
            k_work_schedule(&http_restart_work, K_MSEC(500));
        } else {
            static const char fail[] =
                "{\"result\":\"failure\",\"error\":\"net_set_dhcp failed\"}";
            send_json(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR,
                      fail, sizeof(fail) - 1);
        }
    }

    return 0;
}

static struct http_resource_detail_dynamic api_set_ip_dhcp_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .path_len = sizeof("/api/set_ip_dhcp") - 1,
    },
    .cb = api_set_ip_dhcp_handler,
};

HTTP_RESOURCE_DEFINE(api_set_ip_dhcp_res, icb_api, "/api/set_ip_dhcp",
                     &api_set_ip_dhcp_detail);

ENDPOINT_ENTRY_DEFINE(ep_set_ip_dhcp, "/api/set_ip_dhcp", "POST",
    "Switches the network interface to DHCP.");

/* ==========================================================================
 * Endpoint: POST /api/set_ip
 * ========================================================================== */

/** @brief Scratch buffer for /api/set_ip response. */
static char set_ip_buf[256];

/** @brief Receive buffer for accumulating /api/set_ip request body. */
static char set_ip_recv_buf[256];
static size_t set_ip_recv_len;

/**
 * @brief Extract a JSON string value for the given key from a buffer.
 *
 * Performs a minimal parse: finds "key":"value" and copies value into
 * dst (up to dst_size - 1 chars, NUL-terminated).
 *
 * @param json     The JSON string to search.
 * @param json_len Length of the JSON string.
 * @param key      Key name to look for (without quotes).
 * @param dst      Destination buffer for the extracted value.
 * @param dst_size Size of the destination buffer.
 * @return 0 on success, -1 if the key was not found.
 */
static int json_get_string(const char *json, size_t json_len,
                           const char *key, char *dst, size_t dst_size)
{
    /* Build the search pattern: "key": */
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) {
        return -1;
    }

    /* Search for pattern in json[0..json_len) */
    const char *p = NULL;
    if ((size_t)plen <= json_len) {
        for (size_t i = 0; i <= json_len - plen; i++) {
            if (memcmp(json + i, pattern, plen) == 0) {
                p = json + i;
                break;
            }
        }
    }
    if (p == NULL) {
        return -1;
    }

    /* Skip past "key": and any whitespace before the opening quote */
    const char *val_start = p + plen;
    const char *json_end = json + json_len;
    while (val_start < json_end && (*val_start == ' ' || *val_start == '\t' ||
           *val_start == '\n' || *val_start == '\r')) {
        val_start++;
    }

    if (val_start >= json_end || *val_start != '"') {
        return -1;
    }
    val_start++; /* skip opening quote */

    const char *end = memchr(val_start, '"', json_end - val_start);
    if (end == NULL) {
        return -1;
    }

    size_t vlen = end - val_start;
    if (vlen >= dst_size) {
        vlen = dst_size - 1;
    }
    memcpy(dst, val_start, vlen);
    dst[vlen] = '\0';
    return 0;
}

/**
 * @brief Handler for POST /api/set_ip.
 *
 * Expects a JSON payload:
 *   {"address": "<ip>", "mask": "<mask>", "gateway": "<gw>"}
 *
 * Calls net_set_ip() to apply the static configuration.
 * Returns {"result": "success"} on 200, or
 *         {"result": "<error>"} with an appropriate status on failure.
 */
static int api_set_ip_handler(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *request_ctx,
                              struct http_response_ctx *response_ctx,
                              void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
        size_t copy = request_ctx->data_len;
        if (copy > sizeof(set_ip_recv_buf)) {
            copy = sizeof(set_ip_recv_buf);
        }
        memcpy(set_ip_recv_buf, request_ctx->data, copy);
        set_ip_recv_len = copy;
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        /* Append any remaining data from this final chunk */
        if (request_ctx->data_len > 0) {
            size_t avail = sizeof(set_ip_recv_buf) - set_ip_recv_len;
            size_t copy = (request_ctx->data_len < avail)
                          ? request_ctx->data_len : avail;
            memcpy(set_ip_recv_buf + set_ip_recv_len, request_ctx->data, copy);
            set_ip_recv_len += copy;
        }

        const char *body = set_ip_recv_buf;
        size_t body_len = set_ip_recv_len;

        char addr_s[NET_IPV4_ADDR_LEN];
        char mask_s[NET_IPV4_ADDR_LEN];
        char gw_s[NET_IPV4_ADDR_LEN];

        if (json_get_string(body, body_len, "address", addr_s, sizeof(addr_s)) ||
            json_get_string(body, body_len, "mask",    mask_s, sizeof(mask_s)) ||
            json_get_string(body, body_len, "gateway", gw_s,   sizeof(gw_s))) {
            static const char err[] =
                "{\"result\":\"missing required field(s): address, mask, gateway\"}";
            send_json(response_ctx, HTTP_400_BAD_REQUEST,
                      err, sizeof(err) - 1);
            return 0;
        }

        struct net_ipv4_config cfg;

        if (net_addr_pton(AF_INET, addr_s, &cfg.addr) < 0) {
            int n = snprintf(set_ip_buf, sizeof(set_ip_buf),
                             "{\"result\":\"invalid address: %s\"}", addr_s);
            send_json(response_ctx, HTTP_400_BAD_REQUEST, set_ip_buf, n);
            return 0;
        }
        if (net_addr_pton(AF_INET, mask_s, &cfg.netmask) < 0) {
            int n = snprintf(set_ip_buf, sizeof(set_ip_buf),
                             "{\"result\":\"invalid mask: %s\"}", mask_s);
            send_json(response_ctx, HTTP_400_BAD_REQUEST, set_ip_buf, n);
            return 0;
        }
        if (net_addr_pton(AF_INET, gw_s, &cfg.gw) < 0) {
            int n = snprintf(set_ip_buf, sizeof(set_ip_buf),
                             "{\"result\":\"invalid gateway: %s\"}", gw_s);
            send_json(response_ctx, HTTP_400_BAD_REQUEST, set_ip_buf, n);
            return 0;
        }

        int ret = net_set_ip(&cfg);

        if (ret == 0) {
            static const char ok[] = "{\"result\":\"success\"}";
            send_json(response_ctx, HTTP_200_OK, ok, sizeof(ok) - 1);
            k_work_schedule(&http_restart_work, K_MSEC(500));
        } else {
            int n = snprintf(set_ip_buf, sizeof(set_ip_buf),
                             "{\"result\":\"net_set_ip failed (%d)\"}", ret);
            send_json(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR,
                      set_ip_buf, n);
        }

        set_ip_recv_len = 0;
    }

    return 0;
}

static struct http_resource_detail_dynamic api_set_ip_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .path_len = sizeof("/api/set_ip") - 1,
    },
    .cb = api_set_ip_handler,
};

HTTP_RESOURCE_DEFINE(api_set_ip_res, icb_api, "/api/set_ip", &api_set_ip_detail);

ENDPOINT_ENTRY_DEFINE(ep_set_ip, "/api/set_ip", "POST",
    "Sets a static IP. Payload: {\"address\":\"\", \"mask\":\"\", \"gateway\":\"\"}.");
