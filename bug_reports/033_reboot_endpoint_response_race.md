# Bug Report #033: POST /api/mcu/reboot — MCU Resets Before HTTP Response Is Sent

**Date:** 2026-05-06
**Severity:** High (every caller of a reboot endpoint hangs with a
              broken connection — no response ever received)
**Status:** Resolved
**Component:** `src/rest_api_endpoints.c` (reboot handler)
**Board:** All
**Zephyr Version:** 4.4.0+

---

## Summary

A `POST /api/mcu/reboot` endpoint that calls `sys_reboot()` synchronously
inside the HTTP handler resets the MCU before the TCP stack can flush the
response. The client receives a connection-reset error instead of the
expected 200 OK.

---

## Symptoms

* Client sends `POST http://<ip>/api/mcu/reboot`
* Expected: `200 OK` with `{"message":"MCU reboot initiated"}`, then device
  reboots ~200 ms later
* Actual: TCP connection drops immediately with no response; device reboots
  normally but caller never receives confirmation

---

## Root Cause

```c
static int api_mcu_reboot_handler(...)
{
    static const char body[] = "{\"message\":\"MCU rebooting\"}";
    send_json(response_ctx, HTTP_200_OK, body, sizeof(body) - 1);

    sys_reboot(SYS_REBOOT_WARM);   /* ← resets before TCP flush */
    return 0;
}
```

`send_json()` queues the response into the HTTP framework's internal
buffer. The actual TCP `send()` happens **after** the handler returns,
in the HTTP server worker thread. `sys_reboot()` is synchronous and
preempts all threads immediately, so the TX buffer is never flushed.

The same race occurs with any synchronous system call that terminates
execution before the HTTP worker has a chance to run.

---

## Fix

Replace the synchronous `sys_reboot()` with a delayable work item
scheduled 200 ms into the future:

```c
static void reboot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    sys_reboot(SYS_REBOOT_WARM);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static int api_mcu_reboot_handler(...)
{
    if (status != HTTP_SERVER_REQUEST_DATA_FINAL) return 0;

    static const char body[] = "{\"message\":\"MCU reboot initiated\"}";
    send_json(response_ctx, HTTP_200_OK, body, sizeof(body) - 1);

    k_work_schedule(&reboot_work, K_MSEC(200));
    return 0;
}
```

The 200 ms delay accounts for:
- TCP ACK round-trip (~5–10 ms on LAN)
- HTTP server worker scheduling latency (~10–20 ms)
- Any last-minute log flushes or NVS commits in flight

---

## Lessons Learned

- **Never call `sys_reboot()` directly from an HTTP handler.** The
  response will never reach the client.
- **`k_work_schedule()` with a short delay is the standard Zephyr
  pattern** for "respond then act" scenarios.
- The same pattern applies to any endpoint that needs to perform a
  destructive action after responding (shutdown, sleep, factory reset).

---

## References

- Zephyr `sys_reboot()`: `zephyr/include/zephyr/sys/reboot.h`
- `K_WORK_DELAYABLE_DEFINE`: `zephyr/include/zephyr/kernel.h`
- Zephyr HTTP server: `zephyr/subsys/net/lib/http/`
