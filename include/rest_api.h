#ifndef REST_API_H
#define REST_API_H

/**
 * @file rest_api.h
 * @brief HTTP REST API module interface.
 *
 * Provides an HTTP server on port 80 using Zephyr's built-in HTTP
 * server subsystem. Endpoints are registered declaratively using
 * HTTP_RESOURCE_DEFINE macros, making it easy to add new routes.
 *
 * The endpoint registry is a shared data source used by /index (HTML)
 * and /api/help (JSON) to auto-list every endpoint. Other modules
 * (e.g. rest_api_endpoints.c) add their HTTP_RESOURCE_DEFINE entries
 * and append to the registry so both pages stay in sync.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/sys/iterable_sections.h>

/* ---------- Thread configuration ---------- */
#define REST_API_STACK_SIZE 4096
#define REST_API_PRIORITY   7

/* ---------- Endpoint registry ---------- */

/**
 * @brief Describes a single registered API endpoint for /index and /api/help.
 *
 * Collected automatically by the linker via iterable sections.
 * Use ENDPOINT_ENTRY_DEFINE() to register an entry alongside its handler.
 */
struct endpoint_entry {
    const char *path;        /**< URL path, e.g. "/api/ping" */
    const char *method;      /**< HTTP method, e.g. "GET" */
    const char *description; /**< Human-readable description. */
};

/**
 * @brief Register an endpoint entry in the linker-collected registry.
 *
 * Place this macro next to the matching HTTP_RESOURCE_DEFINE so the
 * entry and handler stay together. /index and /api/help iterate
 * the section automatically.
 *
 * @param _name  Unique C identifier for this entry.
 * @param _path  URL path string.
 * @param _method HTTP method string ("GET", "POST", etc.).
 * @param _desc  Human-readable description string.
 */
#define ENDPOINT_ENTRY_DEFINE(_name, _path, _method, _desc) \
    const STRUCT_SECTION_ITERABLE(endpoint_entry, _name) = { \
        .path = _path, \
        .method = _method, \
        .description = _desc, \
    }

/**
 * @brief HTTP service instance shared by all endpoints.
 *
 * Defined in rest_api.c via HTTP_SERVICE_DEFINE. Other modules
 * reference this symbol in their HTTP_RESOURCE_DEFINE macros.
 */
extern const struct http_service_desc icb_api;

/* ---------- Thread resources (defined in rest_api.c) ---------- */
extern struct k_thread rest_api_thread_data;
extern k_thread_stack_t rest_api_stack[];

/* ---------- Module API ---------- */

/**
 * @brief REST API module thread entry point.
 *
 * Starts the Zephyr HTTP server which listens on port 80.
 * All registered endpoints are served automatically by the
 * server's internal thread. This thread starts the server
 * and then sleeps, keeping the stack alive.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void rest_api_thread_entry(void *p1, void *p2, void *p3);

#endif /* REST_API_H */
