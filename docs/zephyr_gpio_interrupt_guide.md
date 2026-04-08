# Zephyr GPIO Interrupt Guide

A step-by-step guide to setting up GPIO interrupts in Zephyr using the semaphore signaling pattern.

## Architecture Overview

```
┌─────────────┐      k_sem_give()       ┌─────────────┐     k_sem_take()     ┌─────────────┐
│  Hardwar    │  ───────────────────►   │  Semaphore  │ ───────────────────► │  Main Loop  │
│  Interrupt  │                         │  (btn_sem)  │                      │  (thread)   │
│             │                         │             │                      │             │
│  button_pressed()                     │  count: 0|1 │                      │  button_handler()
│  (ISR context)                        └─────────────┘                      │  (thread context)
└─────────────┘                                                              └─────────────┘
```

**Why this pattern?**  
ISRs run in interrupt context where most kernel APIs and heavy logic are unsafe. The ISR should only signal that an event occurred. The actual work happens in thread context via a handler.

---

## Step 1: GPIO DT Spec and Callback Struct

```c
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* Get the button GPIO spec from device tree */
#define SWITCH0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(SWITCH0_NODE, gpios);

/*
 * Callback metadata struct.
 * Registered with the GPIO driver and linked into an internal list.
 * MUST be static/global — if it goes out of scope the driver holds
 * a dangling pointer → crash.
 */
static struct gpio_callback btn_cb_data;
```

- `GPIO_DT_SPEC_GET` extracts the GPIO controller, pin number, and flags from the device tree at **compile time**.
- `gpio_callback` is the struct the GPIO driver uses to track your callback. It **must** persist for the lifetime of the interrupt.

---

## Step 2: Define the Semaphore

```c
/*
 * K_SEM_DEFINE(name, initial_count, count_limit)
 *
 * - name:          variable name for the semaphore
 * - initial_count: starts at 0 (nothing to process yet)
 * - count_limit:   max count is 1 (binary semaphore)
 *
 * A binary semaphore (0 or 1) acts as a simple signal flag.
 * The ISR "gives" it (sets to 1), the main loop "takes" it (sets back to 0).
 * If the button is pressed multiple times before the main loop processes,
 * the count stays at 1 — no pileup.
 */
K_SEM_DEFINE(btn_sem, 0, 1);
```

| Parameter       | Value | Meaning                                        |
|-----------------|-------|------------------------------------------------|
| `initial_count` | `0`   | No pending events at startup                   |
| `count_limit`   | `1`   | Binary semaphore — at most one pending signal  |

> **Tip:** Use `count_limit > 1` if you need to queue multiple events (e.g., counting encoder pulses).

---

## Step 3: ISR Callback (Interrupt Context)

```c
/*
 * This runs in ISR context — keep it minimal.
 * Do NOT use:
 *   - printk (unless CONFIG_PRINTK_SYNC=y, and even then avoid it)
 *   - k_malloc / k_free
 *   - k_msleep or any blocking call
 *   - heavy computation
 *
 * DO:
 *   - k_sem_give()
 *   - set a flag
 *   - k_msgq_put() (for passing data)
 */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&btn_sem);
}
```

**Parameters passed by the GPIO driver:**
| Parameter | Type                   | Description                              |
|-----------|------------------------|------------------------------------------|
| `dev`     | `const struct device *`| The GPIO controller that fired           |
| `cb`      | `struct gpio_callback *`| Pointer to your registered callback struct |
| `pins`    | `uint32_t`             | Bitmask of pins that triggered           |

---

## Step 4: Handler Function (Thread Context)

```c
/*
 * This runs in thread context — safe to use any API.
 * Called from the main loop after k_sem_take() succeeds.
 */
void button_handler(void)
{
    printk("Button pressed! Doing work...\n");

    /* Your logic here — toggle LEDs, update state, send data, etc. */
}
```

The handler is just a normal function. It runs in the main thread so you can safely call `printk`, `gpio_pin_set_dt`, `k_msleep`, or any other Zephyr API.

---

## Step 5: Interrupt Configuration and Setup

```c
int setup_button_interrupt(void)
{
    int ret;

    /* 1. Check if the GPIO device is ready */
    if (!gpio_is_ready_dt(&button0)) {
        printk("Error: GPIO device %s is not ready\n", button0.port->name);
        return -1;
    }

    /* 2. Configure the pin as input */
    ret = gpio_pin_configure_dt(&button0, GPIO_INPUT);
    if (ret < 0) {
        printk("Error %d: failed to configure button pin\n", ret);
        return ret;
    }

    /* 3. Configure the interrupt trigger
     *
     * Common trigger options:
     *   GPIO_INT_EDGE_TO_ACTIVE   — rising edge (if ACTIVE_HIGH) or falling (if ACTIVE_LOW)
     *   GPIO_INT_EDGE_TO_INACTIVE — falling edge (if ACTIVE_HIGH) or rising (if ACTIVE_LOW)
     *   GPIO_INT_EDGE_BOTH        — both edges
     *   GPIO_INT_LEVEL_ACTIVE     — level-triggered while active
     */
    ret = gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        printk("Error %d: failed to configure interrupt\n", ret);
        return ret;
    }

    /* 4. Initialize the callback struct
     *   - btn_cb_data:     the static callback struct
     *   - button_pressed:  your ISR function
     *   - BIT(button0.pin): bitmask of the pin(s) this callback handles
     */
    gpio_init_callback(&btn_cb_data, button_pressed, BIT(button0.pin));

    /* 5. Register the callback with the GPIO port */
    ret = gpio_add_callback(button0.port, &btn_cb_data);
    if (ret < 0) {
        printk("Error %d: failed to add callback\n", ret);
        return ret;
    }

    return 0;
}
```

**Setup order matters:**
1. `gpio_pin_configure_dt` → configure pin direction
2. `gpio_pin_interrupt_configure_dt` → set trigger type
3. `gpio_init_callback` → link your function to the callback struct
4. `gpio_add_callback` → register with the driver

---

## Step 6: Main Loop with `k_sem_take`

### Option A: Non-blocking (`K_NO_WAIT`)

Use when the main loop has other work to do (e.g., toggling LEDs) and should just **check** for button presses each iteration.

```c
int main(void)
{
    setup_button_interrupt();

    while (1) {
        /* Check if button was pressed — returns immediately */
        if (k_sem_take(&btn_sem, K_NO_WAIT) == 0) {
            button_handler();  /* semaphore was available → handle event */
        }

        /* Other main loop work */
        gpio_pin_toggle_dt(&some_led);
        k_msleep(500);
    }
    return 0;
}
```

- `K_NO_WAIT` → returns `0` if semaphore available, `-EBUSY` if not. **Never blocks.**
- Button presses that arrive between checks are held in the semaphore until the next iteration.

### Option B: Blocking (`K_FOREVER`)

Use when the thread should **sleep** until a button press occurs. No polling, no CPU usage while waiting.

```c
int main(void)
{
    setup_button_interrupt();

    while (1) {
        /* Block until button is pressed — thread sleeps, uses no CPU */
        k_sem_take(&btn_sem, K_FOREVER);
        button_handler();
    }
    return 0;
}
```

- `K_FOREVER` → thread is suspended until `k_sem_give()` is called from the ISR. **Blocks indefinitely.**
- Most power-efficient option — the CPU can enter low-power state while waiting.

### Option C: Timeout (`K_MSEC(n)`)

Use when you want to wait up to a certain time, then do something else if no event arrived.

```c
while (1) {
    /* Wait up to 2 seconds for a button press */
    if (k_sem_take(&btn_sem, K_MSEC(2000)) == 0) {
        button_handler();
    } else {
        printk("No button press in 2 seconds — doing idle work\n");
    }
}
```

---

## Summary: Choosing `K_NO_WAIT` vs `K_FOREVER`

| Mode         | Blocks? | CPU usage        | Best for                                    |
|--------------|---------|------------------|---------------------------------------------|
| `K_NO_WAIT`  | No      | Continuous (loop) | Main loop has other periodic work to do     |
| `K_FOREVER`  | Yes     | Zero while waiting | Thread only reacts to events, nothing else  |
| `K_MSEC(n)`  | Up to n ms | Zero while waiting | Timeout-based fallback behavior           |

---

## Complete Example

```c
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define SWITCH0_NODE DT_ALIAS(sw0)
#define LED0_NODE    DT_ALIAS(led0)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SWITCH0_NODE, gpios);
static const struct gpio_dt_spec led    = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static struct gpio_callback btn_cb_data;

K_SEM_DEFINE(btn_sem, 0, 1);

/* ISR — minimal, just signal */
void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&btn_sem);
}

/* Handler — does the real work in thread context */
void button_handler(void)
{
    printk("Button pressed! Toggling LED.\n");
    gpio_pin_toggle_dt(&led);
}

int main(void)
{
    /* Init LED */
    gpio_is_ready_dt(&led);
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    /* Init button + interrupt */
    gpio_is_ready_dt(&button);
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&btn_cb_data, button_isr, BIT(button.pin));
    gpio_add_callback(button.port, &btn_cb_data);

    while (1) {
        k_sem_take(&btn_sem, K_FOREVER);  /* sleep until button press */
        button_handler();
    }
    return 0;
}
```

---

## Common Pitfalls

| Pitfall | Problem | Fix |
|---------|---------|-----|
| `gpio_callback` on stack | Driver holds dangling pointer → crash | Make it `static` or global |
| Heavy logic in ISR | Blocks other interrupts, undefined behavior | Move to handler, signal with semaphore |
| `printk` in ISR | Can cause deadlock or data corruption | Only use in thread context |
| Forgetting `gpio_pin_interrupt_configure_dt` | Interrupt never fires | Must be called after `gpio_pin_configure_dt` |
| Wrong trigger type | Fires on wrong edge or fires continuously | Match trigger to your button's active level |
