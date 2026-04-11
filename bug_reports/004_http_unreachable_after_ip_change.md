# Bug Report #004: HTTP Server Unreachable After IP Change

**Date:** 2026-04-11  
**Severity:** High  
**Status:** Resolved  
**Component:** REST API endpoints (src/rest_api_endpoints.c)  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

After changing the device IP via `/api/set_ip` or `/api/set_ip_dhcp`,
the HTTP server became unreachable on the new IP address. ICMP ping
worked on the new IP, but all HTTP requests timed out.

---

## Symptoms

1. Send `POST /api/set_ip` with a new address — returns `{"result":"success"}`
2. Update the client to use the new IP
3. `ping <new_ip>` succeeds
4. `GET /api/ping` on the new IP times out — no TCP connection established

---

## Root Cause

Zephyr's HTTP server creates a TCP listening socket at startup via
`zsock_bind()` + `zsock_listen()`. The socket's internal `net_context`
is associated with the network interface's routing state at bind time.

When `net_set_ip()` removes the old address and adds a new one, the
existing listening socket's internal state becomes stale. It remains
bound to `INADDR_ANY` but can no longer accept new TCP connections
because the underlying net_context references are invalidated.

ICMP ping works because it bypasses the TCP socket layer entirely —
it's handled directly by the IP stack.

---

## Fix

Added a delayed HTTP server restart using `K_WORK_DELAYABLE_DEFINE`
that fires 500 ms after a successful IP change. The delay gives the
HTTP response time to complete on the old connection before teardown.

```c
static void http_restart_work_handler(struct k_work *work)
{
    http_server_stop();
    k_msleep(1000);    /* wait for server thread to fully shut down */
    http_server_start();
}

static K_WORK_DELAYABLE_DEFINE(http_restart_work, http_restart_work_handler);
```

Both `api_set_ip_handler` and `api_set_ip_dhcp_handler` schedule
this work item on success:

```c
send_json(response_ctx, HTTP_200_OK, ok, sizeof(ok) - 1);
k_work_schedule(&http_restart_work, K_MSEC(500));
```

### Initial fix attempt (failed)

The first version used `k_msleep(100)` between stop and start. This
was not enough — there was a race condition:

- `http_server_stop()` signals the eventfd and sets `server_running = false`
- The server thread needs to exit `http_server_run()`, call
  `close_all_sockets()`, and loop back to `k_sem_take(&server_start)`
- If `http_server_start()` gives the semaphore before the thread
  reaches `k_sem_take()`, the semaphore is consumed while the thread
  is still shutting down, and the new start is lost

Increasing the delay to 1000 ms gave the server thread enough time
to fully complete its shutdown cycle.

---

## Sequence Diagram

```
Client          Handler (workqueue)     HTTP Server Thread
  |                  |                        |
  |--POST /set_ip-->|                        |
  |                  |-- net_set_ip() ------>|
  |<--200 OK--------|                        |
  |                  |-- schedule(500ms) --->|
  |                  |                        |
  |            [500ms passes]                 |
  |                  |                        |
  |                  |-- http_server_stop()-->|
  |                  |                  [exit poll loop]
  |                  |                  [close sockets]
  |            [1000ms sleep]           [k_sem_take()]
  |                  |                        |
  |                  |-- http_server_start()->|
  |                  |                  [bind + listen]
  |                  |                  [poll loop]
  |                  |                        |
  |--GET /ping ---(new IP)------------------>|
  |<--200 OK---------------------------------|
```

---

## Files Changed

- `src/rest_api_endpoints.c` — added `http_restart_work_handler`,
  `K_WORK_DELAYABLE_DEFINE`, and `k_work_schedule()` calls in
  `api_set_ip_handler` and `api_set_ip_dhcp_handler`

---

## Lesson Learned

1. Zephyr's TCP listening sockets do not automatically adapt when the
   network interface's IP address changes. Any operation that modifies
   the IP configuration must restart services that hold open listening
   sockets.

2. `http_server_stop()` is asynchronous — the server thread needs time
   to process the stop signal, close sockets, and return to its idle
   state. A generous delay (1 s) between stop and start is needed.

3. `K_WORK_DELAYABLE_DEFINE` is the correct pattern for deferring
   operations that can't happen inside an HTTP handler callback (which
   runs on the server's own thread).
