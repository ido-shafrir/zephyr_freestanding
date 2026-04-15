#ifndef REST_API_ENDPOINTS_H
#define REST_API_ENDPOINTS_H

/**
 * @file rest_api_endpoints.h
 * @brief Example REST API endpoint declarations.
 *
 * All application-specific HTTP endpoints live here and in
 * rest_api_endpoints.c.  Each endpoint registers itself with
 * HTTP_RESOURCE_DEFINE against the shared rest_api_svc service
 * (declared in rest_api.h).
 *
 * Example endpoints implemented:
 *   POST /api/echo            - echo the JSON payload
 *   GET  /api/ion/<name>/test - return the ion name (wildcard path)
 */

#endif /* REST_API_ENDPOINTS_H */
