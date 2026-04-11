#ifndef REST_API_ENDPOINTS_H
#define REST_API_ENDPOINTS_H

/**
 * @file rest_api_endpoints.h
 * @brief Business-logic REST API endpoint declarations.
 *
 * All application-specific HTTP endpoints live here and in
 * rest_api_endpoints.c.  Each endpoint registers itself with
 * HTTP_RESOURCE_DEFINE against the shared icb_api service
 * (declared in rest_api.h).
 *
 * Endpoints implemented:
 *   POST /api/echo           – echo the JSON payload
 *   GET  /api/ion/<name>/test – return the ion name
 *   POST /api/set_ip_dhcp    – switch to DHCP
 *   POST /api/set_ip         – set a static IPv4 address
 */

#endif /* REST_API_ENDPOINTS_H */
