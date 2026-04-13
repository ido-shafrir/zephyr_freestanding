/**
 * @file rest_api.c
 * @brief HTTP REST API module implementation.
 *
 * Uses Zephyr's built-in HTTP server subsystem to serve JSON API
 * endpoints. New endpoints are added by:
 *   1. Writing a handler function matching http_resource_dynamic_cb_t
 *   2. Defining a static http_resource_detail_dynamic struct
 *   3. Calling HTTP_RESOURCE_DEFINE() to register the route
 */

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "rest_api.h"
#include "ota.h"

LOG_MODULE_REGISTER(rest_api, LOG_LEVEL_DBG);

/**
 * @brief Log an incoming HTTP request (method + URL).
 *
 * Call once per request, typically at the DATA_FINAL entry point.
 */
#define LOG_REQUEST(client) \
    LOG_INF("%s %s", http_method_str(client->method), client->url_buffer)

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(rest_api_stack, REST_API_STACK_SIZE);
struct k_thread rest_api_thread_data;

/* ---------- HTTP service definition ---------- */
static uint16_t http_port = 80;

HTTP_SERVICE_DEFINE(icb_api, NULL, &http_port, 2, 1, NULL, NULL, NULL);

/* ==========================================================================
 * Endpoint registry entries for endpoints defined in this file
 * ========================================================================== */

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
static void send_json_response(struct http_response_ctx *response_ctx,
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
 * Endpoint: GET /api/ping
 * ========================================================================== */

static const char ping_json[] = "{\"result\":\"pong\"}";

/**
 * @brief Handler for GET /api/ping.
 *
 * Returns a simple JSON response: {"result": "pong"} with HTTP 200.
 */
static int api_ping_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        LOG_REQUEST(client);
        send_json_response(response_ctx, HTTP_200_OK,
                           ping_json, sizeof(ping_json) - 1);
    }

    return 0;
}

static struct http_resource_detail_dynamic api_ping_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/api/ping") - 1,
    },
    .cb = api_ping_handler,
};

HTTP_RESOURCE_DEFINE(api_ping_res, icb_api, "/api/ping", &api_ping_detail);

ENDPOINT_ENTRY_DEFINE(ep_ping, "/api/ping", "GET",
    "Returns {\"result\":\"pong\"}. Use for connectivity testing.");

/* ==========================================================================
 * Endpoint: GET /api/help
 * ========================================================================== */

/** @brief Scratch buffer for building the /api/help JSON response. */
static char help_buf[2048];

/**
 * @brief Handler for GET /api/help.
 *
 * Dynamically builds a JSON object listing every entry in
 * endpoint_registry, including path, method, and description.
 */
static int api_help_handler(struct http_client_ctx *client,
                            enum http_transaction_status status,
                            const struct http_request_ctx *request_ctx,
                            struct http_response_ctx *response_ctx,
                            void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        LOG_REQUEST(client);
        int off = snprintf(help_buf, sizeof(help_buf),
                           "{\"endpoints\":[");

        bool first = true;
        STRUCT_SECTION_FOREACH(endpoint_entry, ep) {
            off += snprintf(help_buf + off, sizeof(help_buf) - off,
                            "%s{\"path\":\"%s\",\"method\":\"%s\","
                            "\"description\":\"",
                            first ? "" : ",",
                            ep->path, ep->method);
            first = false;
            /* Escape double-quotes and backslashes for valid JSON */
            for (const char *d = ep->description;
                 *d && off < (int)sizeof(help_buf) - 2; d++) {
                if (*d == '"' || *d == '\\') {
                    help_buf[off++] = '\\';
                }
                help_buf[off++] = *d;
            }
            off += snprintf(help_buf + off, sizeof(help_buf) - off, "\"}");
        }

        off += snprintf(help_buf + off, sizeof(help_buf) - off, "]}");

        send_json_response(response_ctx, HTTP_200_OK,
                           help_buf, off);
    }

    return 0;
}

static struct http_resource_detail_dynamic api_help_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/api/help") - 1,
    },
    .cb = api_help_handler,
};

HTTP_RESOURCE_DEFINE(api_help_res, icb_api, "/api/help", &api_help_detail);

ENDPOINT_ENTRY_DEFINE(ep_help, "/api/help", "GET",
    "Returns a JSON list of all available API endpoints.");

/* ==========================================================================
 * Endpoint: GET /index
 * ========================================================================== */

/** @brief Scratch buffer for building the /index HTML response. */
static char index_buf[2048];

/**
 * @brief Send an HTML response with the given status code and body.
 *
 * Sets Content-Type to text/html and marks the response as complete.
 *
 * @param response_ctx Response context to populate.
 * @param status       HTTP status code.
 * @param html         HTML string to send.
 * @param html_len     Length of the HTML string.
 */
static void send_html_response(struct http_response_ctx *response_ctx,
                                enum http_status status,
                                const char *html, size_t html_len)
{
    static const struct http_header html_hdrs[] = {
        { .name = "Content-Type", .value = "text/html" },
    };

    response_ctx->status = status;
    response_ctx->headers = html_hdrs;
    response_ctx->header_count = ARRAY_SIZE(html_hdrs);
    response_ctx->body = (const uint8_t *)html;
    response_ctx->body_len = html_len;
    response_ctx->final_chunk = true;
}

/**
 * @brief Handler for GET /index.
 *
 * Dynamically builds an HTML page listing every endpoint from
 * endpoint_registry, with clickable links and descriptions.
 */
static int index_handler(struct http_client_ctx *client,
                         enum http_transaction_status status,
                         const struct http_request_ctx *request_ctx,
                         struct http_response_ctx *response_ctx,
                         void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        LOG_REQUEST(client);
        int off = snprintf(index_buf, sizeof(index_buf),
                           "<!DOCTYPE html><html><head><title>ICB-FW</title></head><body>"
                           "<h1>ICB Firmware - Available Paths</h1><ul>");

        STRUCT_SECTION_FOREACH(endpoint_entry, ep) {
            off += snprintf(index_buf + off, sizeof(index_buf) - off,
                            "<li><a href=\"%s\">%s</a> [%s] - %s</li>",
                            ep->path, ep->path, ep->method, ep->description);
        }

        off += snprintf(index_buf + off, sizeof(index_buf) - off,
                        "</ul></body></html>");

        send_html_response(response_ctx, HTTP_200_OK, index_buf, off);
    }

    return 0;
}

static struct http_resource_detail_dynamic index_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/index") - 1,
    },
    .cb = index_handler,
};

HTTP_RESOURCE_DEFINE(index_res, icb_api, "/index", &index_detail);

ENDPOINT_ENTRY_DEFINE(ep_index, "/index", "GET",
    "Returns an HTML page listing all available paths.");

/* ==========================================================================
 * Thread entry point
 * ========================================================================== */

/**
 * @brief REST API module thread entry point.
 *
 * Starts the Zephyr HTTP server. The server runs in its own internal
 * thread — this thread just starts it and then sleeps to keep the
 * stack alive.
 */
void rest_api_thread_entry(void *p1, void *p2, void *p3)
{
    int ret = http_server_start();

    if (ret < 0) {
        LOG_ERR("Failed to start HTTP server: %d", ret);
        return;
    }

    size_t ep_count;
    STRUCT_SECTION_COUNT(endpoint_entry, &ep_count);

    ota_report_module_ready(OTA_MODULE_REST_API);

    LOG_INF("HTTP server started on port %u", http_port);
    LOG_INF("Available endpoints (%zu):", ep_count);
    STRUCT_SECTION_FOREACH(endpoint_entry, ep) {
        LOG_INF("  %s %s", ep->method, ep->path);
    }


    /* The server runs in its own thread; keep this thread alive */
    while (1) {
        k_sleep(K_FOREVER);
    }
}
