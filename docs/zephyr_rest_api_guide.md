# Zephyr HTTP REST API Guide

A step-by-step guide to building an HTTP REST API on Zephyr using the built-in HTTP server subsystem, with a self-documenting endpoint registry powered by iterable linker sections.

## Architecture Overview

```
┌──────────────┐  HTTP_RESOURCE_DEFINE   ┌──────────────┐   HTTP/TCP    ┌─────────┐
│  rest_api_   │ ──────────────────────► │  rest_api.c  │ ◄───────────► │ Client  │
│  endpoints.c │                         │  (server +   │               │ (curl,  │
│  (handlers)  │  ENDPOINT_ENTRY_DEFINE  │   /ping,     │               │  browser│
│              │ ──────────────────────► │   /help,     │               │  etc.)  │
└──────────────┘                         │   /index)    │               └─────────┘
                                         └──────────────┘
                                               │
                                         sections-rom.ld
                                         (linker collects
                                          iterable sections)
```

The framework has three layers:
1. **rest_api.c** — HTTP server, core endpoints (`/api/ping`, `/api/help`, `/index`), and the `send_json_response` helper.
2. **rest_api_endpoints.c** — Application-specific endpoint handlers. Each registers itself via macros.
3. **sections-rom.ld** — Custom linker script fragment that places the iterable sections in ROM.

---

## Step 1: Kconfig — Enable the HTTP Server

In `prj.conf`:

```ini
# ─── Networking (required) ───
CONFIG_NETWORKING=y
CONFIG_NET_L2_ETHERNET=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y

# ─── HTTP Server ───
CONFIG_HTTP_SERVER=y
CONFIG_HTTP_SERVER_RESOURCE_WILDCARD=y   # for wildcard path endpoints

# ─── Buffer Configuration ───
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16
CONFIG_NET_BUF_RX_COUNT=32
CONFIG_NET_BUF_TX_COUNT=32

# ─── Logging ───
CONFIG_LOG=y
```

`CONFIG_HTTP_SERVER_RESOURCE_WILDCARD=y` is required if any endpoint uses `*` in its path (e.g. `/api/ion/*/test`).

---

## Step 2: The `sections-rom.ld` Linker Fragment

Zephyr's `HTTP_RESOURCE_DEFINE` and our custom `ENDPOINT_ENTRY_DEFINE` both use **iterable sections** — the linker collects all instances into a contiguous array in ROM. For this to work, you must tell the linker about these sections.

Create `sections-rom.ld` in your project root:

```ld
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_rest_api_svc, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(endpoint_entry, Z_LINK_ITERABLE_SUBALIGN)
```

And register it in `CMakeLists.txt`:

```cmake
zephyr_linker_sources(SECTIONS sections-rom.ld)
```

### How it works

- `ITERABLE_SECTION_ROM(name, align)` creates a named ROM section that the linker populates with all `STRUCT_SECTION_ITERABLE(name, ...)` instances.
- `http_resource_desc_rest_api_svc` is auto-generated from your `HTTP_SERVICE_DEFINE(rest_api_svc, ...)` — the naming convention is `http_resource_desc_<service_name>`.
- `endpoint_entry` collects all `ENDPOINT_ENTRY_DEFINE(...)` instances for `/api/help` and `/index` to enumerate.

### Why is this needed?

Without `sections-rom.ld`, the linker has no section definitions for these iterable arrays. The result:
- `HTTP_RESOURCE_DEFINE` entries are silently discarded — endpoints return 404.
- `STRUCT_SECTION_FOREACH(endpoint_entry, ...)` iterates zero entries — `/api/help` returns an empty list.

> **Important:** If you rename the HTTP service (the first argument to `HTTP_SERVICE_DEFINE`), you must also update the section name in `sections-rom.ld` to match: `http_resource_desc_<new_service_name>`.

---

## Step 3: HTTP Service and Server Thread

In `rest_api.c`, define the HTTP service and start the server:

```c
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

static uint16_t http_port = 80;

/*
 * HTTP_SERVICE_DEFINE(name, host, port, max_clients, backlog, ...)
 *
 * - name: C identifier — used by HTTP_RESOURCE_DEFINE to bind endpoints
 * - host: NULL = bind to all interfaces (INADDR_ANY)
 * - port: pointer to uint16_t
 * - max_clients: concurrent connections (2 is conservative)
 * - backlog: TCP listen backlog
 */
HTTP_SERVICE_DEFINE(rest_api_svc, NULL, &http_port, 2, 1, NULL, NULL, NULL);
```

Start the server from a dedicated thread:

```c
void rest_api_thread_entry(void *p1, void *p2, void *p3)
{
    int ret = http_server_start();
    if (ret < 0) {
        LOG_ERR("Failed to start HTTP server: %d", ret);
        return;
    }
    LOG_INF("HTTP server started on port %u", http_port);

    /* Server runs in its own internal thread; keep this one alive */
    while (1) {
        k_sleep(K_FOREVER);
    }
}
```

---

## Step 4: The Endpoint Registry

The endpoint registry provides self-documentation. Every endpoint registers a `struct endpoint_entry` that `/api/help` (JSON) and `/index` (HTML) enumerate automatically:

```c
/* In rest_api.h */
struct endpoint_entry {
    const char *path;
    const char *method;
    const char *description;
};

#define ENDPOINT_ENTRY_DEFINE(_name, _path, _method, _desc) \
    const STRUCT_SECTION_ITERABLE(endpoint_entry, _name) = { \
        .path = _path, \
        .method = _method, \
        .description = _desc, \
    }
```

Place `ENDPOINT_ENTRY_DEFINE` right next to the matching `HTTP_RESOURCE_DEFINE` so they stay in sync:

```c
HTTP_RESOURCE_DEFINE(api_ping_res, rest_api_svc, "/api/ping", &api_ping_detail);

ENDPOINT_ENTRY_DEFINE(ep_ping, "/api/ping", "GET",
    "Returns {\"result\":\"pong\"}. Use for connectivity testing.");
```

---

## Step 5: The Handler Callback Signature

Every dynamic endpoint handler has this signature:

```c
static int my_handler(struct http_client_ctx *client,
                      enum http_transaction_status status,
                      const struct http_request_ctx *request_ctx,
                      struct http_response_ctx *response_ctx,
                      void *user_data)
```

Key parameters:
- `status` — the transaction lifecycle event:
  - `HTTP_SERVER_REQUEST_DATA_MORE` — body data arrived (may be partial)
  - `HTTP_SERVER_REQUEST_DATA_FINAL` — end of request
- `request_ctx->data` / `request_ctx->data_len` — request body chunk
- `response_ctx` — populate this to send a response
- `client->method` — HTTP method enum
- `client->url_buffer` — the requested URL path

### CRITICAL: Multi-chunk body delivery

> **Bug lesson (see [bug_reports/003](../bug_reports/003_http_post_body_missing.md)):**
> Zephyr's HTTP server may deliver the request body across **two** callback
> invocations: `DATA_MORE` (with body data) then `DATA_FINAL` (with `data_len == 0`).
> **Never assume the full body arrives in a single `DATA_FINAL` callback.**
>
> All POST/PUT handlers must accumulate data from `DATA_MORE` into a buffer,
> then process on `DATA_FINAL`.

---

## How-To: Add a Simple GET Endpoint

A GET endpoint with no request body and a static JSON response:

```c
/* 1. Define the static response */
static const char status_json[] = "{\"status\":\"ok\",\"uptime_ms\":0}";

/* 2. Write the handler */
static int api_status_handler(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *request_ctx,
                              struct http_response_ctx *response_ctx,
                              void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        send_json_response(response_ctx, HTTP_200_OK,
                           status_json, sizeof(status_json) - 1);
    }
    return 0;
}

/* 3. Define the resource detail */
static struct http_resource_detail_dynamic api_status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/api/status") - 1,
    },
    .cb = api_status_handler,
};

/* 4. Register the route */
HTTP_RESOURCE_DEFINE(api_status_res, rest_api_svc, "/api/status",
                     &api_status_detail);

/* 5. Register in the endpoint registry */
ENDPOINT_ENTRY_DEFINE(ep_status, "/api/status", "GET",
    "Returns system status.");
```

---

## How-To: Add a GET with Dynamic Payload

A GET endpoint that builds a JSON response dynamically (e.g. reading a sensor):

```c
static char sensor_buf[256];

static int api_sensor_handler(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *request_ctx,
                              struct http_response_ctx *response_ctx,
                              void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        /* Build dynamic response */
        int temp = read_temperature();  /* your app logic */
        int len = snprintf(sensor_buf, sizeof(sensor_buf),
                           "{\"temperature_c\":%d}", temp);

        send_json_response(response_ctx, HTTP_200_OK, sensor_buf, len);
    }
    return 0;
}

static struct http_resource_detail_dynamic api_sensor_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .path_len = sizeof("/api/sensor") - 1,
    },
    .cb = api_sensor_handler,
};

HTTP_RESOURCE_DEFINE(api_sensor_res, rest_api_svc, "/api/sensor",
                     &api_sensor_detail);

ENDPOINT_ENTRY_DEFINE(ep_sensor, "/api/sensor", "GET",
    "Returns current sensor reading.");
```

**Note:** The scratch buffer (`sensor_buf`) is static because the response must remain valid until Zephyr's HTTP server finishes sending it. Do not use stack-allocated buffers.

---

## How-To: Add a Simple POST Endpoint

A POST endpoint that performs an action but ignores the request body:

```c
static int api_reboot_handler(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *request_ctx,
                              struct http_response_ctx *response_ctx,
                              void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        /* Perform the action */
        LOG_INF("Reboot requested via API");

        static const char ok[] = "{\"result\":\"rebooting\"}";
        send_json_response(response_ctx, HTTP_200_OK, ok, sizeof(ok) - 1);

        /* Schedule reboot after response is sent */
        /* k_work_schedule(&reboot_work, K_MSEC(500)); */
    }
    return 0;
}

static struct http_resource_detail_dynamic api_reboot_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .path_len = sizeof("/api/reboot") - 1,
    },
    .cb = api_reboot_handler,
};

HTTP_RESOURCE_DEFINE(api_reboot_res, rest_api_svc, "/api/reboot",
                     &api_reboot_detail);

ENDPOINT_ENTRY_DEFINE(ep_reboot, "/api/reboot", "POST",
    "Triggers a device reboot.");
```

---

## How-To: Add a POST with JSON Payload

A POST endpoint that parses a JSON request body. This is the most complex pattern and **must handle multi-chunk body delivery:**

```c
/* Scratch buffers — must be static */
static char config_resp_buf[256];
static char config_recv_buf[256];
static size_t config_recv_len;

/**
 * @brief Minimal JSON string extractor.
 *
 * Finds "key": "value" in the JSON buffer and copies value into dst.
 * Handles optional whitespace after the colon (bug_reports/003 lesson).
 */
static int json_get_string(const char *json, size_t json_len,
                           const char *key, char *dst, size_t dst_size)
{
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) {
        return -1;
    }

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

static int api_config_handler(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *request_ctx,
                              struct http_response_ctx *response_ctx,
                              void *user_data)
{
    /* --- Accumulate body across callbacks --- */
    if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
        size_t copy = request_ctx->data_len;
        if (copy > sizeof(config_recv_buf)) {
            copy = sizeof(config_recv_buf);
        }
        memcpy(config_recv_buf, request_ctx->data, copy);
        config_recv_len = copy;
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        /* Append any remaining data */
        if (request_ctx->data_len > 0) {
            size_t avail = sizeof(config_recv_buf) - config_recv_len;
            size_t copy = (request_ctx->data_len < avail)
                          ? request_ctx->data_len : avail;
            memcpy(config_recv_buf + config_recv_len, request_ctx->data, copy);
            config_recv_len += copy;
        }

        /* Parse JSON fields */
        char name[32], value[32];
        if (json_get_string(config_recv_buf, config_recv_len,
                            "name", name, sizeof(name)) ||
            json_get_string(config_recv_buf, config_recv_len,
                            "value", value, sizeof(value))) {
            static const char err[] = "{\"error\":\"missing name or value\"}";
            send_json_response(response_ctx, HTTP_400_BAD_REQUEST,
                               err, sizeof(err) - 1);
            config_recv_len = 0;
            return 0;
        }

        /* Process the data ... */
        int len = snprintf(config_resp_buf, sizeof(config_resp_buf),
                           "{\"result\":\"set %s=%s\"}", name, value);

        send_json_response(response_ctx, HTTP_200_OK, config_resp_buf, len);
        config_recv_len = 0;
    }

    return 0;
}

static struct http_resource_detail_dynamic api_config_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .path_len = sizeof("/api/config") - 1,
    },
    .cb = api_config_handler,
};

HTTP_RESOURCE_DEFINE(api_config_res, rest_api_svc, "/api/config",
                     &api_config_detail);

ENDPOINT_ENTRY_DEFINE(ep_config, "/api/config", "POST",
    "Set a config value. Payload: {\"name\":\"\", \"value\":\"\"}.");
```

**Key points from bug #003:**
- Always handle `DATA_MORE` to accumulate the body.
- Always skip whitespace after `:` when parsing JSON keys.
- Always reset `recv_len = 0` after processing (even on error paths).

---

## How-To: Add a Wildcard Path Endpoint

A GET endpoint with a variable path segment (e.g. `/api/device/<id>/info`):

```c
static char device_buf[256];

static int api_device_info_handler(struct http_client_ctx *client,
                                   enum http_transaction_status status,
                                   const struct http_request_ctx *request_ctx,
                                   struct http_response_ctx *response_ctx,
                                   void *user_data)
{
    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        const char *url = (const char *)client->url_buffer;
        size_t url_len = strlen(url);

        /* Parse: /api/device/<id>/info
         * Prefix: "/api/device/" (12 chars)
         * Suffix: "/info"        (5 chars)  */
        const char prefix[] = "/api/device/";
        const size_t prefix_len = sizeof(prefix) - 1;
        const char suffix[] = "/info";
        const size_t suffix_len = sizeof(suffix) - 1;

        if (url_len <= prefix_len + suffix_len ||
            memcmp(url, prefix, prefix_len) != 0 ||
            memcmp(url + url_len - suffix_len, suffix, suffix_len) != 0) {
            static const char err[] =
                "{\"error\":\"URL must be /api/device/<id>/info\"}";
            send_json_response(response_ctx, HTTP_400_BAD_REQUEST,
                               err, sizeof(err) - 1);
            return 0;
        }

        const char *id_start = url + prefix_len;
        size_t id_len = url_len - prefix_len - suffix_len;

        int len = snprintf(device_buf, sizeof(device_buf),
                           "{\"device_id\":\"%.*s\",\"status\":\"online\"}",
                           (int)id_len, id_start);

        send_json_response(response_ctx, HTTP_200_OK, device_buf, len);
    }
    return 0;
}

static struct http_resource_detail_dynamic api_device_info_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        /* path_len must match the PATTERN including the wildcard */
        .path_len = sizeof("/api/device/*/info") - 1,
    },
    .cb = api_device_info_handler,
};

/* Register with * as the wildcard segment */
HTTP_RESOURCE_DEFINE(api_device_info_res, rest_api_svc,
                     "/api/device/*/info", &api_device_info_detail);

/* In the registry, use {id} for human readability */
ENDPOINT_ENTRY_DEFINE(ep_device_info, "/api/device/{id}/info", "GET",
    "Returns device info for the given ID.");
```

**Wildcard rules:**
- Use `*` in the path passed to `HTTP_RESOURCE_DEFINE` — this matches any single path segment.
- Requires `CONFIG_HTTP_SERVER_RESOURCE_WILDCARD=y` in `prj.conf`.
- The handler must manually parse `client->url_buffer` to extract the variable segment.
- `path_len` in the detail struct must match the **pattern** length (including `*`), not the actual URL length.

---

## File Layout

```
project/
├── include/
│   ├── rest_api.h               # Service extern, endpoint_entry struct, macros
│   └── rest_api_endpoints.h     # (optional) endpoint declarations
├── src/
│   ├── main.c                   # Creates the REST API thread
│   ├── rest_api.c               # HTTP service, server thread, /ping, /help, /index
│   └── rest_api_endpoints.c     # Application-specific endpoint handlers
├── sections-rom.ld              # Linker fragment for iterable sections
├── prj.conf                     # HTTP server Kconfig
└── CMakeLists.txt               # Must include zephyr_linker_sources(SECTIONS sections-rom.ld)
```

---

## CMakeLists.txt Integration

```cmake
target_sources(app PRIVATE
    src/rest_api.c
    src/rest_api_endpoints.c
)

target_include_directories(app PRIVATE include)

# REQUIRED: register the linker fragment for iterable sections
zephyr_linker_sources(SECTIONS sections-rom.ld)
```

---

## Adding a New Endpoint — Checklist

1. Write the handler function (follow the `http_resource_dynamic_cb_t` signature).
2. Create a `static struct http_resource_detail_dynamic` with the method bitmask and path_len.
3. Call `HTTP_RESOURCE_DEFINE(name, rest_api_svc, "/path", &detail)`.
4. Call `ENDPOINT_ENTRY_DEFINE(name, "/path", "METHOD", "description")`.
5. If this is a POST/PUT handler, add `DATA_MORE` body accumulation (see how-to above).
6. If the path contains a wildcard, ensure `CONFIG_HTTP_SERVER_RESOURCE_WILDCARD=y`.

---

## Known Issues and Bug Fixes

These issues were discovered in downstream projects and are addressed in this code:

### 1. POST Body Always Empty (Bug #003)

**Symptom:** POST endpoints always received empty body data on `DATA_FINAL`.

**Cause:** Zephyr delivers POST bodies across two callbacks: `DATA_MORE` (body) then `DATA_FINAL` (empty). Handlers that only checked `DATA_FINAL` missed the body entirely. Additionally, a minimal JSON parser failed on whitespace after `:`.

**Fix:** Accumulate body from `DATA_MORE` into a static buffer, process on `DATA_FINAL`. Skip whitespace in JSON parsing.

See [bug_reports/003_http_post_body_missing.md](../bug_reports/003_http_post_body_missing.md).

### 2. Server Unreachable After IP Change (Bug #004)

**Symptom:** After changing the device IP via an API endpoint, HTTP became unreachable (ping worked, TCP didn't).

**Cause:** Zephyr's HTTP listening socket's `net_context` becomes stale after the bound IP changes. The socket remains "bound" but can't accept new connections.

**Fix:** Schedule a delayed `http_server_stop()` + `http_server_start()` via `K_WORK_DELAYABLE_DEFINE`, with a 1-second delay between stop and start to let the server thread fully shut down.

See [bug_reports/004_http_unreachable_after_ip_change.md](../bug_reports/004_http_unreachable_after_ip_change.md).

---

## Debugging Checklist

| Issue | What to Check |
|---|---|
| All endpoints return 404 | `sections-rom.ld` registered? Section name matches service name? |
| `/api/help` returns empty list | `ITERABLE_SECTION_ROM(endpoint_entry, ...)` in `sections-rom.ld`? |
| POST body always empty | Handling `DATA_MORE` to accumulate body? (bug #003) |
| JSON parsing fails on valid JSON | Skipping whitespace after `:` in parser? (bug #003) |
| Server unreachable after IP change | Restarting HTTP server after `net_set_ip()`? (bug #004) |
| `http_server_start()` returns error | Port already in use? Previous server not fully stopped? |
| Wildcard path returns 404 | `CONFIG_HTTP_SERVER_RESOURCE_WILDCARD=y` in prj.conf? |
| Response buffer garbled | Using `static` buffer? Stack buffers are freed before send completes |
