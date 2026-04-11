# Bug Report #002: UART Commands Dispatched Twice

**Date:** 2026-04-11  
**Severity:** Low  
**Status:** Resolved  
**Component:** UART command interface (src/uart.c)  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

Every UART command was executed and printed twice. Typing `help` produced
two identical help outputs; typing `ping` produced two `pong` responses.

---

## Symptoms

```
---- Sent utf8 encoded message: "help\r\n" ----
Available commands:
  help, h, ?,                   - Show this help
  ...
Available commands:
  help, h, ?,                   - Show this help
  ...
[00:00:11.158,000] <wrn> uart: Unknown command: help 
[00:00:11.163,000] <wrn> uart: Unknown command: help 
```

---

## Root Cause

The serial terminal sends `\r\n` (CR + LF) as a line ending. The UART ISR
treated both `\r` and `\n` as command terminators:

```c
if (c == '\n' || c == '\r') {
    if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        k_sem_give(&rx_sem);   // signal command ready
    }
}
```

The `rx_pos` variable was only reset in the thread (after `k_sem_take`),
not in the ISR. The sequence was:

1. `\r` received → `rx_pos > 0` → null-terminate, signal semaphore
2. `\n` received immediately after → `rx_pos` still > 0 (not yet reset
   by thread) → signal semaphore again
3. Thread wakes twice, dispatches the same command buffer both times

---

## Fix

Reset `rx_pos` in the ISR immediately after null-terminating the buffer,
before signaling the semaphore:

```c
if (c == '\n' || c == '\r') {
    if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        rx_pos = 0;            // ← reset here, not in thread
        k_sem_give(&rx_sem);
    }
}
```

This ensures `\n` arriving after `\r` sees `rx_pos == 0` and is
silently ignored. The redundant `rx_pos = 0` in the thread loop was
also removed.

---

## Files Changed

| File | Change |
|------|--------|
| `src/uart.c` | Moved `rx_pos = 0` from thread loop into ISR after null-terminate |

---

## Lessons Learned

1. When handling `\r\n` line endings from a terminal, reset ISR state
   *before* signaling the consumer thread — not after.
2. Semaphores with max count 1 (`K_SEM_DEFINE(rx_sem, 0, 1)`) would
   prevent double-wake, but the real fix is to not signal twice.
