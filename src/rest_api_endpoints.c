/**
 * @file rest_api_endpoints.c
 * @brief Example HTTP endpoint implementations.
 *
 * Each handler follows the http_resource_dynamic_cb_t signature,
 * builds a JSON response in a scratch buffer, and sends it via
 * send_json().
 *
 * Example endpoints:
 *   POST /api/echo            - echo the JSON payload
 *   GET  /api/ion/<name>/test - return the ion name (wildcard path)
 */

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "rest_api.h"
#include "rest_api_endpoints.h"

LOG_MODULE_REGISTER(rest_api_ep, LOG_LEVEL_DBG);

/**
 * @brief Log an incoming HTTP request (method + URL).
 *
 * Call once per request, typically at the DATA_FINAL entry point.
 */
#define LOG_REQUEST(client) \
    LOG_INF("%s %s", http_method_str(client->method), client->url_buffer)

/* ==========================================================================
 * Helper: common JSON response sender
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
 *
 * Demonstrates a POST handler that receives a JSON payload and returns
 * it wrapped in a result object. Shows the multi-chunk body accumulation
 * pattern required by Zephyr's HTTP server (see bug_reports/003).
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
 * The body may arrive across multiple callback invocations:
 *   - DATA_MORE: body data (may be partial)
 *   - DATA_FINAL: end of request (may contain remaining data)
 *
 * We accumulate into echo_recv_buf, then process on DATA_FINAL.
 */
static int api_echo_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
        /* First/middle chunk — accumulate body data */
        size_t copy = request_ctx->data_len;
        if (copy > sizeof(echo_recv_buf)) {
            copy = sizeof(echo_recv_buf);
        }
        memcpy(echo_recv_buf, request_ctx->data, copy);
        echo_recv_len = copy;
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        LOG_REQUEST(client);
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

HTTP_RESOURCE_DEFINE(api_echo_res, rest_api_svc, "/api/echo", &api_echo_detail);

ENDPOINT_ENTRY_DEFINE(ep_echo, "/api/echo", "POST",
    "Echoes the JSON payload back as {\"result\": <payload>}.");

/* ==========================================================================
 * Endpoint: GET /api/ion/<name>/test
 *
 * Demonstrates a wildcard path endpoint. Registered as "/api/ion/*/test"
 * — the handler parses the <name> segment from the URL itself.
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
        LOG_REQUEST(client);
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

HTTP_RESOURCE_DEFINE(api_ion_res, rest_api_svc, "/api/ion/*/test", &api_ion_detail);

ENDPOINT_ENTRY_DEFINE(ep_ion_test, "/api/ion/{name}/test", "GET",
    "Returns {\"result\": \"{name}\"}.");
