# Zephyr UART Command Interface Guide

A step-by-step guide to building an interrupt-driven UART command interface in Zephyr using a dedicated thread.

## Architecture Overview

```
┌──────────────┐     uart_fifo_read()    ┌─────────────┐     k_sem_take()     ┌────────────────┐
│  UART HW     │  ────────────────────►  │  Semaphore  │ ───────────────────► │  UART Thread   │
│  Interrupt   │                         │  (rx_sem)   │                      │                │
│              │                         │             │                      │  dispatch_     │
│  uart_isr_cb()                         │  count: 0|1 │                      │  command()     │
│  (ISR context)                         └─────────────┘                      │  (thread ctx)  │
└──────────────┘                                                              └────────────────┘
```

**Why this pattern?**  
The UART ISR fires for every received byte and runs in interrupt context where most kernel APIs are unsafe. Characters are accumulated into a line buffer inside the ISR. When a newline arrives, the ISR signals a semaphore to wake a dedicated thread that dispatches the complete command.

---

## Step 1: Kconfig – Enable the UART Driver with Interrupts

In `prj.conf`, enable these options:

```ini
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
```

`CONFIG_SERIAL` enables the UART driver subsystem.  
`CONFIG_UART_INTERRUPT_DRIVEN` enables the interrupt-driven API (`uart_irq_*` functions). Without it, only the polling API (`uart_poll_in/out`) is available.

---

## Step 2: Get the UART Device from Device Tree

```c
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

/*
 * Get the console UART device at compile time via DT chosen node.
 * The 'zephyr,console' chosen node is defined in the board's .dts file.
 *
 * Alternatives:
 *   DEVICE_DT_GET(DT_NODELABEL(usart2))   – by node label
 *   DEVICE_DT_GET(DT_ALIAS(my_uart))      – by alias
 */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
```

- `DEVICE_DT_GET` resolves the device pointer at **compile time** — no runtime lookup needed.
- Using `DT_CHOSEN(zephyr_console)` makes the code board-agnostic: it always picks whatever UART the board designates as its console.

---

## Step 3: RX Buffer and Semaphore

```c
#include <zephyr/kernel.h>

#define RX_BUF_SIZE 128

static char rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_pos;

/*
 * K_SEM_DEFINE(name, initial_count, count_limit)
 *
 * initial_count = 0 : thread blocks immediately on k_sem_take()
 * count_limit   = 1 : binary semaphore (only one event queued)
 */
static K_SEM_DEFINE(rx_sem, 0, 1);
```

- `rx_buf` accumulates characters from the ISR until a newline is received.
- `rx_pos` is `volatile` because it is written in the ISR and read/reset in the thread.
- The semaphore is binary (limit 1) — if new commands arrive while one is being processed, at most one will be queued.

---

## Step 4: ISR Callback – Accumulate Characters

```c
/**
 * @brief UART interrupt service routine callback.
 *
 * Reads characters from the UART FIFO into rx_buf. On newline (\r or \n),
 * null-terminates the buffer and signals rx_sem for the thread to process.
 * Runs in ISR context — must not block.
 *
 * @param dev Pointer to the UART device structure.
 * @param user_data User data pointer (unused).
 */
static void uart_isr_cb(const struct device *dev, void *user_data)
{
    if (!uart_irq_update(dev)) {
        return;
    }

    if (!uart_irq_rx_ready(dev)) {
        return;
    }

    uint8_t c;

    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (rx_pos > 0) {          /* ignore empty lines */
                rx_buf[rx_pos] = '\0';
                k_sem_give(&rx_sem);   /* wake the thread */
            }
        } else if (rx_pos < RX_BUF_SIZE - 1) {
            rx_buf[rx_pos++] = c;
        }
    }
}
```

Key points:
- `uart_irq_update()` must be called first — it acknowledges the interrupt and returns whether data is pending.
- `uart_irq_rx_ready()` checks that the interrupt was specifically an RX event (not TX-complete, error, etc.).
- `uart_fifo_read()` drains the hardware FIFO one byte at a time.
- When a newline is detected, the buffer is null-terminated and the semaphore is given.

---

## Step 5: Sending Data – Polling Output

```c
/**
 * @brief Send a null-terminated string over UART using polling output.
 *
 * @param str The string to transmit.
 */
static void uart_send(const char *str)
{
    while (*str) {
        uart_poll_out(uart_dev, *str++);
    }
}
```

`uart_poll_out()` is the simplest way to send — it blocks until the TX holding register is free, then writes one byte. This is fine for short responses. For high-throughput TX, consider interrupt-driven or async TX.

---

## Step 6: Command Handlers

Define handler functions for each command:

```c
static void pong(void)
{
    uart_send("pong\r\n");
}

static void print_help(void)
{
    uart_send("Available commands:\r\n");
    uart_send("  help, h, ?  - Show this help\r\n");
    uart_send("  ping        - Respond with 'pong'\r\n");
}
```

---

## Step 7: Command Dispatcher

```c
/**
 * @brief Dispatch a received command string to the appropriate handler.
 *
 * Matches the command against known commands and calls the corresponding
 * function. Sends "unknown command" response if no match.
 *
 * @param cmd Null-terminated command string from the RX buffer.
 */
static void dispatch_command(const char *cmd)
{
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "ping") == 0) {
        pong();
    } else {
        uart_send("unknown command: ");
        uart_send(cmd);
        uart_send("\r\nType 'help' for available commands.\r\n");
    }
}
```

To add a new command with arguments, use `strncmp` to match the prefix:

```c
} else if (strncmp(cmd, "set_value ", 10) == 0) {
    handle_set_value(cmd + 10);   /* pass the argument portion */
}
```

---

## Step 8: Thread Entry Point

```c
#define UART_STACK_SIZE 1024
#define UART_PRIORITY   4

K_THREAD_STACK_DEFINE(uart_stack, UART_STACK_SIZE);
struct k_thread uart_thread_data;

/**
 * @brief UART module thread entry point.
 *
 * Initializes the UART device with interrupt-driven RX, then loops
 * waiting for complete commands via semaphore and dispatching them.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
void command_uart_thread_entry(void *p1, void *p2, void *p3)
{
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return;
    }

    /* Register the ISR callback and enable RX interrupts */
    uart_irq_callback_user_data_set(uart_dev, uart_isr_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    uart_send("Ready. Type 'help' for available commands.\r\n");

    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);   /* block until ISR signals */

        dispatch_command(rx_buf);
        rx_pos = 0;                        /* reset buffer for next command */
    }
}
```

- `device_is_ready()` verifies the UART peripheral was initialized by the driver at boot.
- `uart_irq_callback_user_data_set()` registers our ISR callback with the driver.
- `uart_irq_rx_enable()` enables RX interrupts — from this point on, `uart_isr_cb` fires for each received byte.
- The infinite loop blocks on the semaphore, processes the buffered command, then resets `rx_pos` for the next line.

---

## Step 9: Create the Thread from main()

```c
#include "command_uart.h"

void main(void)
{
    k_thread_create(&uart_thread_data, uart_stack,
                    K_THREAD_STACK_SIZEOF(uart_stack),
                    command_uart_thread_entry, NULL, NULL, NULL,
                    UART_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&uart_thread_data, "uart_cmd");
}
```

- `K_NO_WAIT` starts the thread immediately.
- `k_thread_name_set` is optional but helps when debugging with `thread analyze` in the Zephyr shell or a debugger.

---

## File Layout

```
project/
├── include/
│   └── command_uart.h   # Thread config, stack/thread externs, entry prototype
├── src/
│   ├── main.c           # Creates the UART thread
│   └── command_uart.c   # ISR, buffer, dispatcher, thread entry
├── prj.conf            # CONFIG_SERIAL=y, CONFIG_UART_INTERRUPT_DRIVEN=y
└── CMakeLists.txt
```

---

## Adding New Commands

1. Write a handler function (e.g. `cmd_status()`).
2. Add an `else if` branch in `dispatch_command()`.
3. Add a line in `print_help()`.

For commands with arguments, use `strncmp` to match the prefix and pass the remainder to the handler:

```c
} else if (strncmp(cmd, "mycommand ", 10) == 0) {
    cmd_mycommand(cmd + 10);
}
```

---

## Common Pitfalls

| Issue | Cause | Fix |
|---|---|---|
| No characters received | `CONFIG_UART_INTERRUPT_DRIVEN=n` | Enable it in `prj.conf` |
| ISR never fires | `uart_irq_rx_enable()` not called | Call it in the thread init |
| Buffer overflow / garbled | `rx_pos` not reset after dispatch | Reset `rx_pos = 0` after processing |
| Crash in ISR | Calling blocking APIs (`printk`, `k_sleep`) | Use only ISR-safe APIs; signal semaphore instead |
| Console output mixed with commands | Using same UART for console logging and commands | Use a separate UART or disable `CONFIG_LOG` on console |
