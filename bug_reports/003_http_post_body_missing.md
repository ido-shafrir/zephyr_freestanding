# Bug Report #003: HTTP POST /api/set_ip Always Returns 400

**Date:** 2026-04-11  
**Severity:** High  
**Status:** Resolved  
**Component:** REST API endpoints (src/rest_api_endpoints.c)  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

POST endpoint `/api/set_ip` always returned 400 "missing required
field(s)" even when a valid JSON payload was sent. `/api/echo` worked
fine.

---

## Symptoms

Sending a well-formed request:

```http
POST /api/set_ip
Content-Type: application/json

{
    "address": "192.168.0.85",
    "mask": "255.255.255.0",
    "gateway": "192.168.0.1"
}
```

Always returned:

```json
HTTP/1.1 400
Content-Type: application/json

{"result": "missing required field(s): address, mask, gateway"}
```

---

## Root Cause

Two issues were found:

### Issue 1: Multi-chunk body delivery

Zephyr's HTTP server delivers the request body across **two** callback
invocations:

1. `HTTP_SERVER_REQUEST_DATA_MORE` — contains the body data
2. `HTTP_SERVER_REQUEST_DATA_FINAL` — signals end of request
   (often with `data_len == 0`)

The handlers only checked for `HTTP_SERVER_REQUEST_DATA_FINAL`:

```c
if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
    const char *body = (const char *)request_ctx->data;
    size_t body_len = request_ctx->data_len;
    // body_len is 0 here — the data already arrived in DATA_MORE
}
```

By the time `DATA_FINAL` fired, the body had already been delivered
in the `DATA_MORE` callback which was ignored.

### Issue 2: JSON parser didn't tolerate whitespace (actual root cause)

The `json_get_string()` helper searched for the pattern `"key":"` with
**no whitespace** between `:` and `"`. Standard JSON formatting includes
a space (`"key": "value"`), so the pattern never matched:

```c
/* Old — fails on "address": "..." */
int plen = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
```

`/api/echo` was unaffected because it copies the raw payload without
parsing keys.

---

## Fix

### Fix 1: Accumulate body across callbacks

Added a static receive buffer per POST endpoint to accumulate body data
across `DATA_MORE` and `DATA_FINAL` callbacks.

### Fix 2: Whitespace-tolerant JSON parser

Changed `json_get_string()` to search for `"key":` (without the quote),
then skip any whitespace before finding the opening `"` of the value:

```c
/* New — search for "key": then skip whitespace */
int plen = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
...
/* Skip whitespace between : and opening quote */
while (val_start < json_end && (*val_start == ' ' || *val_start == '\t' ||
       *val_start == '\n' || *val_start == '\r')) {
    val_start++;
}
if (val_start >= json_end || *val_start != '"') {
    return -1;
}
val_start++; /* skip opening quote */
```

---

## Files Changed

- `src/rest_api_endpoints.c` — `api_echo_handler` and
  `api_set_ip_handler` updated to accumulate body data;
  `json_get_string()` updated to tolerate whitespace after `:`

---

## Lesson Learned

1. Zephyr's HTTP server dynamic callbacks must handle multi-chunk body
   delivery. **Never assume the full body arrives in a single callback.**

2. When writing a minimal JSON parser, always handle optional whitespace
   around separators (`:` and `,`). Even "pretty-printed" JSON from
   tools like VS Code REST Client includes spaces after colons.

## Lesson Learned

Zephyr's HTTP server dynamic callbacks must handle multi-chunk body
delivery. **Never assume the full body arrives in a single callback.**
Any POST/PUT handler must either:

- Accumulate data from `DATA_MORE` into a buffer, then process on
  `DATA_FINAL`, or
- Process data incrementally on each callback

This applies to all future POST/PUT endpoints added to the project.
