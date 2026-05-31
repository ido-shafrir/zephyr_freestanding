# Bug Report #035: STM32 EXTI Line Conflicts — One Port Per Pin Number Causes Silent Interrupt Loss

**Date:** 2026-05-11
**Severity:** High
**Status:** Resolved (replaced interrupt-based detection with polling)
**Component:** GPIO interrupt configuration, STM32 EXTI mux
**Board:** Any STM32F4/L4/H7 board using multiple GPIO interrupts across
           ports on the same pin number
**Zephyr Version:** 4.4.0+

---

## Summary

GPIO interrupt-based detection is unreliable when multiple pins across
different GPIO ports share the same EXTI line number. The STM32 EXTI
controller maps each pin number (0–15) to exactly **one** GPIO port at
a time via `SYSCFG_EXTICR`. Only the last-configured port receives
interrupts — the rest are silently lost.

---

## Symptoms

1. Interrupts on certain pins (those sharing an EXTI line with another
   port's pin) are never delivered.
2. Only a polling path catches state changes on conflicting pins.
3. The subset of conflict-free EXTI lines works correctly — creating
   inconsistent detection latency.
4. No build error, no runtime warning — the failure is completely
   silent.

---

## Root Cause

The STM32 EXTI multiplexer (`SYSCFG_EXTICR` registers) allows only
**one port per pin number**. Example: if you configure interrupts on
both `PA8` and `PD8`, the Zephyr STM32 GPIO driver unconditionally
overwrites `SYSCFG_EXTICR[2]` for EXTI8 — the last
`gpio_pin_interrupt_configure_dt()` call wins, and the other pin's
interrupt is silently disabled.

This is a hardware limitation, not a Zephyr bug. The driver does not
warn about conflicts.

Example conflict table:

| EXTI | Pin users (port.pin) | Result |
|------|----------------------|--------|
| EXTI8 | PD8, PC8 | Only last-configured works |
| EXTI12 | PB12, PD12, PE12 | Only last-configured works |

---

## Fix

Two approaches:

### Option A: Polling (recommended when many conflicts exist)

Replace interrupt-based detection with periodic GPIO reads in an
existing thread. Use a per-pin state machine to detect transitions:

```c
static bool prev_state[NUM_PINS];

void poll_gpio_inputs(void) {
    for (int i = 0; i < NUM_PINS; i++) {
        bool cur = gpio_pin_get_dt(&pins[i]);
        if (cur != prev_state[i]) {
            prev_state[i] = cur;
            handle_transition(i, cur);
        }
    }
}
```

Benefits:
- No EXTI conflicts
- Consistent detection latency across all pins
- Simpler code (no ISR context, no deferred work)

### Option B: Pin assignment redesign (hardware change)

Reassign pins so that no two interrupt-capable inputs share the same
pin number across ports. This requires PCB changes but preserves
low-latency interrupt-driven detection.

---

## Lessons Learned

1. **On STM32, only ONE GPIO port can use interrupts for any given
   pin number (0–15).** Plan pin assignments accordingly during
   hardware design.
2. **The Zephyr STM32 GPIO driver does not warn about EXTI conflicts.**
   The last `gpio_pin_interrupt_configure_dt()` call silently wins.
3. **Polling with state-machine transition detection** is often more
   reliable than interrupts for multi-pin designs, especially with
   floating inputs or electrically noisy environments.
4. **Always verify interrupt delivery on ALL configured pins** during
   hardware bring-up — not just a subset.

---

## References

- STM32F4 Reference Manual (RM0090), Section 12.2: EXTI block diagram
  showing the SYSCFG_EXTICR one-port-per-line mux.
- Zephyr STM32 GPIO driver: `drivers/gpio/gpio_stm32.c` — see
  `stm32_exti_set_callback()`.
- Any STM32 family (F1/F2/F4/F7/L4/H7/U5) has this same EXTI
  architecture.
