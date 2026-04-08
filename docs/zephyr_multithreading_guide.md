# Zephyr Multi-Threading Guide: Module-Per-Thread Pattern

A guide to structuring Zephyr applications using one thread per module, with `main()` as the orchestrator.

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  main.c  (orchestrator)                                  │
│                                                          │
│  - Defines shared state (volatile globals + mutex)       │
│  - Creates all threads via k_thread_create()             │
│  - Sets initial values, then sleeps (k_sleep(K_FOREVER)) │
└──────────┬──────────────────────┬────────────────────────┘
           │                      │
           ▼                      ▼
┌─────────────────────┐  ┌─────────────────────┐
│  blink_thread.c     │  │  switch_thread.c    │
│  (module A)         │  │  (module B)         │
│                     │  │                     │
│  - Own stack        │  │  - Own stack        │
│  - Own init         │  │  - Own init         │
│  - Own loop logic   │  │  - ISR + handler    │
│  - Priority 3       │  │  - Priority 2       │
└─────────┬───────────┘  └──────────┬──────────┘
          │                         │
          ▼                         ▼
   ┌─────────────────────────────────────────┐
   │  state_machines.h                       │
   │                                         │
   │  - extern volatile shared state         │
   │  - extern k_mutex (thread-safe access)  │
   │  - Included by all modules that need    │
   │    access to shared state               │
   └─────────────────────────────────────────┘
```

**Why this pattern?**
- Each hardware concern (LEDs, buttons, sensors, comms) lives in its own module
- Threads run concurrently — a blocking LED toggle doesn't freeze button detection
- `main()` is clean — it only wires modules together and sets initial state
- Easy to add/remove modules without touching other code

---

## File Structure

```
project/
├── CMakeLists.txt
├── prj.conf
├── include/
│   ├── state_machines.h    # Shared state: extern volatile vars + extern mutex
│   ├── blink_thread.h      # Thread A: API, stack/thread externs, config
│   └── switch_thread.h     # Thread B: API, stack/thread externs, config
└── src/
    ├── main.c              # Orchestrator: defines shared state, mutex, thread creation
    ├── blink_thread.c      # Thread A: implementation
    └── switch_thread.c     # Thread B: implementation
```

---

## Step 1: Module Header

Each module exposes its thread entry point, stack, thread struct, and configuration constants via a header.

```c
/* include/blink_thread.h */
#ifndef BLINK_THREAD_H
#define BLINK_THREAD_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* ---------- Thread configuration ---------- */
#define BLINK_STACK_SIZE 1024
#define BLINK_PRIORITY   3      /* Lower number = higher priority in Zephyr */

/* ---------- Thread resources (defined in blink_thread.c) ---------- */
extern struct k_thread blink_thread_data;
extern k_thread_stack_t blink_stack[];

/* ---------- Module API ---------- */
void blink_thread_entry(void *p1, void *p2, void *p3);

/* ---------- Shared hardware (defined in blink_thread.c) ---------- */
extern const struct gpio_dt_spec led0;
extern const struct gpio_dt_spec led1;

#endif
```

**Key rules:**
- `extern` for anything defined in the `.c` but needed by other modules or `main.c`
- Stack size and priority as `#define` so `main.c` can pass them to `k_thread_create()`
- The thread entry function signature **must** be `void (*)(void *, void *, void *)` — that's what `k_thread_create()` expects

---

## Step 2: Module Implementation

Each module defines its own stack, thread struct, init logic, and loop.

```c
/* src/blink_thread.c */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "blink_thread.h"

/* ---------- Hardware definitions ---------- */
const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(blink_stack, BLINK_STACK_SIZE);
struct k_thread blink_thread_data;

/* ---------- Module-private init ---------- */
static int led_init(void)
{
    const struct gpio_dt_spec leds[] = {led0, led1};

    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        if (!gpio_is_ready_dt(&leds[i])) {
            return -ENODEV;
        }
        int ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

/* ---------- Thread entry point ---------- */
void blink_thread_entry(void *p1, void *p2, void *p3)
{
    if (led_init() < 0) {
        printk("blink: init failed\n");
        return;  /* thread exits — k_thread_create'd thread just dies */
    }

    while (1) {
        /* Access shared state set by main or other threads */
        gpio_pin_toggle_dt(&led0);
        k_msleep(500);
    }
}
```

**Key points:**
- `K_THREAD_STACK_DEFINE` allocates the stack at compile time — the size must match the header's `#define`
- `struct k_thread` is the thread control block — **not** `static`, so `main.c` can reference it
- The entry function inits its own hardware, then loops forever
- When the entry function returns, the thread is terminated

---

## Step 3: Orchestrator (`main.c`)

`main()` owns shared state, creates threads, and then sleeps.

```c
/* src/main.c */
#include <zephyr/kernel.h>
#include "blink_thread.h"
#include "switch_thread.h"

/* ---------- Shared state ----------
 * Owned by main, accessed by threads.
 * volatile because modified by one thread and read by another.
 */
static volatile bool blink_enabled = true;
static volatile struct gpio_dt_spec blinking_led;

int main(void)
{
    /* Set initial state before any thread runs */
    blinking_led = led0;

    /* Create threads — they won't run until main yields or sleeps,
     * because main runs at priority 0 (cooperative) by default.
     *
     * k_thread_create() parameters:
     *   &thread_data  - thread control block (struct k_thread)
     *   stack         - stack array from K_THREAD_STACK_DEFINE
     *   stack_size    - must match K_THREAD_STACK_DEFINE size
     *   entry         - void (*)(void*, void*, void*) entry point
     *   p1, p2, p3    - args passed to entry (NULL if unused)
     *   priority      - thread priority
     *   options       - 0 for default
     *   delay         - K_NO_WAIT to start immediately
     */
    k_thread_create(&blink_thread_data, blink_stack, BLINK_STACK_SIZE,
                    blink_thread_entry, NULL, NULL, NULL,
                    BLINK_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&btn_thread_data, btn_stack, BTN_STACK_SIZE,
                    btn_thread_entry, NULL, NULL, NULL,
                    BTN_PRIORITY, 0, K_NO_WAIT);

    /* Main has nothing else to do — sleep forever.
     * The scheduler runs the other threads.
     */
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
```

---

## Thread Creation: `k_thread_create()` Reference

```c
k_tid_t k_thread_create(
    struct k_thread *new_thread,    // Thread control block
    k_thread_stack_t *stack,        // Stack memory
    size_t stack_size,              // Stack size in bytes
    k_thread_entry_t entry,         // Entry function
    void *p1, void *p2, void *p3,   // Arguments to entry
    int prio,                       // Priority
    uint32_t options,               // Options (0 = default)
    k_timeout_t delay               // Start delay
);
```

| Parameter    | Typical Value      | Notes                                         |
|--------------|--------------------|-----------------------------------------------|
| `new_thread` | `&blink_thread_data` | Must persist (global/static)                |
| `stack`      | `blink_stack`      | From `K_THREAD_STACK_DEFINE`                  |
| `stack_size` | `1024`             | Must match the `K_THREAD_STACK_DEFINE` size   |
| `entry`      | `blink_thread_entry` | Signature: `void (*)(void*, void*, void*)`  |
| `p1,p2,p3`   | `NULL`             | Pass config/context if needed                 |
| `prio`       | `3`                | Lower number = higher priority (preemptive)   |
| `options`    | `0`                | Use `K_ESSENTIAL` to reboot on thread crash   |
| `delay`      | `K_NO_WAIT`        | `K_MSEC(100)` to delay start                  |

---

## Priority Guidelines

Zephyr uses a **lower number = higher priority** scheme. The priority space is split into two classes:

### Cooperative Threads (Negative Priorities)

```
Priority -1  ─── highest cooperative priority
Priority -2  ─── ...
Priority -N  ─── lowest cooperative priority
```

- **Negative priority values** create **cooperative** threads
- A cooperative thread **cannot be preempted** by another thread — it runs until it explicitly yields (`k_yield()`), sleeps (`k_sleep()`, `k_msleep()`), or blocks on a kernel object (`k_sem_take()`, `k_mutex_lock()`, etc.)
- Even a higher-priority thread must wait until the cooperative thread voluntarily gives up the CPU
- **ISRs can still preempt cooperative threads** — only thread-to-thread preemption is disabled
- Use for: short critical sections, protocol state machines, init sequences that must not be interrupted

```c
/* This thread runs uninterrupted by other threads */
k_thread_create(&critical_thread, stack, STACK_SIZE,
                critical_entry, NULL, NULL, NULL,
                -1,   /* cooperative — cannot be preempted by threads */
                0, K_NO_WAIT);
```

### Preemptive Threads (Zero and Positive Priorities)

```
Priority 0   ─── highest preemptive priority (main thread default)
Priority 1   ─── critical real-time tasks
Priority 2   ─── button/event handlers (latency-sensitive)
Priority 3   ─── LED blinking, display updates (non-critical)
Priority 5+  ─── logging, telemetry, background tasks
```

- **Zero or positive** values create **preemptive** threads
- A preemptive thread **can be preempted at any time** by a higher-priority (lower number) thread
- When a higher-priority thread becomes runnable (e.g., semaphore given), it immediately takes the CPU

### Priority Comparison Table

| Priority | Type        | Can be preempted by threads? | Use case                        |
|----------|-------------|------------------------------|---------------------------------|
| `-1`     | Cooperative | No (only by ISRs)            | Critical sections, init         |
| `0`      | Preemptive  | Yes (by cooperative threads)  | main thread (default)           |
| `1`      | Preemptive  | Yes                          | Real-time control loops         |
| `2`      | Preemptive  | Yes                          | Event handlers                  |
| `3`      | Preemptive  | Yes                          | Periodic tasks (LEDs, display)  |
| `5+`     | Preemptive  | Yes                          | Background / low-priority work  |

### Key Rules

1. **Cooperative threads starve preemptive threads** — a cooperative thread that never yields will block all preemptive threads
2. **ISRs always preempt everything** — regardless of thread priority or cooperative mode
3. **Equal priority threads** are scheduled round-robin (if `CONFIG_TIMESLICING=y`) or FIFO
4. **`main()` runs at priority 0** by default (`CONFIG_MAIN_THREAD_PRIORITY`), which is preemptive

When a higher-priority thread becomes runnable (e.g., semaphore given), it **preempts** any lower-priority thread immediately.

---

## Sharing State Between Threads

Shared state must be accessed in a **thread-safe** manner. Use a mutex or message queue — both are kernel-provided and safe across threads.

### Option A: Mutex + Volatile (for shared variables)

The recommended pattern: declare shared state as `volatile` (prevents compiler optimization), protect access with a `k_mutex`, and snapshot into local variables.

**state_machines.h** — declare shared state and mutex:
```c
#ifndef STATE_MACHINES_H
#define STATE_MACHINES_H

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

extern struct k_mutex state_mutex;
extern volatile bool blink_enabled;
extern volatile struct gpio_dt_spec blinking_led;

#endif
```

**main.c** — define (own) the shared state and mutex:
```c
#include "state_machines.h"

K_MUTEX_DEFINE(state_mutex);
volatile bool blink_enabled = true;
volatile struct gpio_dt_spec blinking_led;
```

**Writer** (e.g., switch_thread.c) — lock, modify, unlock:
```c
#include "state_machines.h"

void button_handler(void) {
    k_mutex_lock(&state_mutex, K_FOREVER);
    if (blinking_led.pin == led0.pin) {
        blinking_led = led1;
        gpio_pin_set_dt(&led0, 0);
    } else {
        blinking_led = led0;
        gpio_pin_set_dt(&led1, 0);
    }
    k_mutex_unlock(&state_mutex);
}
```

**Reader** (e.g., blink_thread.c) — lock, snapshot, unlock, use local copy:
```c
#include "state_machines.h"

while (1) {
    k_mutex_lock(&state_mutex, K_FOREVER);
    bool local_enabled = blink_enabled;
    struct gpio_dt_spec local_led = blinking_led;
    k_mutex_unlock(&state_mutex);

    if (local_enabled) {
        gpio_pin_toggle_dt(&local_led);
    }
    k_msleep(500);
}
```

**Why snapshot?** You hold the lock only for the copy, then release it immediately. The GPIO toggle (which takes real time) runs lock-free, so other threads aren't blocked.

### Option B: Message Queue (for event/command passing)

Use when threads communicate via discrete events rather than shared variables.

```c
struct command {
    uint8_t type;
    struct gpio_dt_spec led;
};

K_MSGQ_DEFINE(cmd_queue, sizeof(struct command), 4, 4);

/* Producer (button thread) */
struct command cmd = {.type = CMD_SWITCH_LED, .led = led1};
k_msgq_put(&cmd_queue, &cmd, K_NO_WAIT);

/* Consumer (blink thread) */
struct command cmd;
if (k_msgq_get(&cmd_queue, &cmd, K_NO_WAIT) == 0) {
    handle_command(&cmd);
}
```

**Use when:** threads send discrete commands/events to each other, no shared mutable state needed.

### Comparison Table

| Method          | Thread-safe? | Blocking? | Best for                                 |
|-----------------|-------------|-----------|------------------------------------------|
| `k_mutex`       | Yes         | Yes       | Shared mutable state (variables, structs) |
| `k_msgq`        | Yes         | Optional  | Event/command passing between threads     |
| `k_sem`         | Yes         | Optional  | Signaling (ISR → thread)                  |
| `volatile` alone| **No**      | No        | **Not sufficient** — use with mutex       |

> **Note:** `volatile` alone is **not thread-safe**. It only prevents the compiler from caching values in registers. It does not guarantee atomicity for multi-byte structs or prevent interleaved reads/writes. Always pair `volatile` with a `k_mutex` for shared state.

---

## Adding a New Module

To add a new module (e.g., `sensor_thread`):

### 1. Create the header

```c
/* include/sensor_thread.h */
#ifndef SENSOR_THREAD_H
#define SENSOR_THREAD_H

#include <zephyr/kernel.h>

#define SENSOR_STACK_SIZE 1024
#define SENSOR_PRIORITY   4

extern struct k_thread sensor_thread_data;
extern k_thread_stack_t sensor_stack[];

void sensor_thread_entry(void *p1, void *p2, void *p3);

#endif
```

### 2. Create the implementation

```c
/* src/sensor_thread.c */
#include <zephyr/kernel.h>
#include "sensor_thread.h"

K_THREAD_STACK_DEFINE(sensor_stack, SENSOR_STACK_SIZE);
struct k_thread sensor_thread_data;

void sensor_thread_entry(void *p1, void *p2, void *p3)
{
    /* init sensor hardware */

    while (1) {
        /* read sensor, process data */
        k_msleep(1000);
    }
}
```

### 3. Add to CMakeLists.txt

```cmake
target_sources(
    app
    PRIVATE
    src/main.c
    src/blink_thread.c
    src/switch_thread.c
    src/sensor_thread.c        # ← add here
)
```

### 4. Create the thread in main.c

```c
#include "sensor_thread.h"

/* In main(): */
k_thread_create(&sensor_thread_data, sensor_stack, SENSOR_STACK_SIZE,
                sensor_thread_entry, NULL, NULL, NULL,
                SENSOR_PRIORITY, 0, K_NO_WAIT);
```

---

## Sequence Chart: Button Press Event

This shows the full flow when the user presses the button, from hardware interrupt to LED switch.

```
Time ──────────────────────────────────────────────────────────────────────►

  Hardware         ISR Context          btn_thread (P2)         blink_thread (P3)
  (GPIO pin)       (button_pressed)     (switch_thread.c)       (blink_thread.c)
     │                  │                     │                       │
     │  Button pressed  │                     │                       │
     ├─────────────────►│                     │                       │ toggling led0
     │  (rising edge)   │                     │  sleeping on          │ k_msleep(500)
     │                  │                     │  k_sem_take(FOREVER)  │
     │                  │  k_sem_give()       │                       │
     │                  ├────────────────────►│                       │
     │                  │                     │  ◄── wakes up         │
     │                  │  ISR returns        │                       │
     │                  │                     │                       │
     │                  │                     │  btn_thread now       │
     │                  │                     │  runnable at P2       │
     │                  │                     │                       │
     │                  │                     │  PREEMPTS             │
     │                  │                     │  blink_thread (P3)    │
     │                  │                     │  ──────────────────►X │ (suspended)
     │                  │                     │                       │
     │                  │                     │  button_handler():    │
     │                  │                     │  ┌─────────────────┐  │
     │                  │                     │  │ blinking_led =  │  │
     │                  │                     │  │   led1          │  │
     │                  │                     │  │ led0 → OFF      │  │
     │                  │                     │  │ printk("...")    │  │
     │                  │                     │  └─────────────────┘  │
     │                  │                     │                       │
     │                  │                     │  k_sem_take(FOREVER)  │
     │                  │                     │  ──► sleeping again   │
     │                  │                     │                       │
     │                  │                     │        RESUMES ──────►│
     │                  │                     │                       │ snapshot: led1
     │                  │                     │                       │ toggling led1
     │                  │                     │                       │ k_msleep(500)
     │                  │                     │                       │
```

### Step-by-step breakdown:

| Step | Where          | What happens                                                |
|------|----------------|-------------------------------------------------------------|
| 1    | Hardware       | GPIO pin detects rising edge on PC13 (button press)         |
| 2    | ISR            | `button_pressed()` runs — calls `k_sem_give(&btn_sem)`      |
| 3    | Scheduler      | `btn_sem` count goes 0→1, `btn_thread` becomes **runnable** |
| 4    | ISR            | ISR returns, scheduler runs                                 |
| 5    | Scheduler      | `btn_thread` (P2) preempts `blink_thread` (P3)              |
| 6    | btn_thread     | `k_sem_take()` returns 0, calls `button_handler()`          |
| 7    | btn_thread     | Handler sets `blinking_led = led1`, turns off `led0`        |
| 8    | btn_thread     | Loops back to `k_sem_take(K_FOREVER)` — blocks, sleeps      |
| 9    | Scheduler      | `blink_thread` (P3) resumes — it's the highest runnable     |
| 10   | blink_thread   | Snapshots new `blinking_led` (led1), starts toggling it     |

---

## Common Pitfalls

| Pitfall | Problem | Fix |
|---------|---------|-----|
| Stack too small | Hard fault / stack overflow | Increase `STACK_SIZE`, enable `CONFIG_THREAD_STACK_INFO` |
| `static` on thread struct/stack | `main.c` can't access them for `k_thread_create` | Remove `static`, add `extern` in header |
| `k_msleep(K_FOREVER)` | `k_msleep` takes `int32_t` ms, not `k_timeout_t` | Use `k_sleep(K_FOREVER)` instead |
| Shared struct without mutex | Race condition — partial reads | Use `k_mutex` or snapshot pattern |
| Thread entry returns accidentally | Thread dies silently | Always use `while (1)` loop |
| Forgetting to add `.c` to CMakeLists | Linker error: undefined reference | Add to `target_sources()` |
| `volatile` on multi-field struct | Not atomic — can read half-old/half-new | Use mutex or snapshot into local copy |

---

## Minimal `prj.conf` for Threading

```ini
CONFIG_GPIO=y          # GPIO driver
CONFIG_PRINTK=y        # Debug output
# Threading is enabled by default in Zephyr — no extra config needed.
# Optional:
# CONFIG_THREAD_NAME=y            # Name threads for debugging
# CONFIG_THREAD_STACK_INFO=y      # Stack usage monitoring
# CONFIG_THREAD_MONITOR=y         # Thread runtime stats
```
