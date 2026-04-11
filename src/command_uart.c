#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "command_uart.h"

LOG_MODULE_REGISTER(uart, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(uart_stack, UART_STACK_SIZE);
struct k_thread uart_thread_data;

/* ---------- UART device ----------
 * Currently uses the console UART (e.g. usart3 on Nucleo H753ZI, via ST-Link USB).
 * To use a different UART:
 *   - By DT chosen:  DEVICE_DT_GET(DT_CHOSEN(zephyr_console))
 *   - By node label: DEVICE_DT_GET(DT_NODELABEL(usart2))
 *   - By alias:      DEVICE_DT_GET(DT_ALIAS(my_uart))
 */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ---------- RX buffer and signaling ---------- */
#define RX_BUF_SIZE 128

static char rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_pos;
static K_SEM_DEFINE(rx_sem, 0, 1);


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
            if (rx_pos > 0) {
                rx_buf[rx_pos] = '\0';
                rx_pos = 0;
                k_sem_give(&rx_sem);
            }
        } else if (rx_pos < RX_BUF_SIZE - 1) {
            rx_buf[rx_pos++] = c;
        }
    }
}

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


/**
 * @brief Handle the 'ping' command by responding with "pong".
 */
static void pong(void)
{
    uart_send("pong\r\n");
    LOG_INF("Received 'ping', sent 'pong'");
}

/**
 * @brief Print the list of available UART commands.
 *
 * Sends a formatted help message over UART listing all
 * supported commands and their usage.
 */
static void print_help(void)
{
    uart_send("Available commands:\r\n");
    uart_send("  help, h, ?  - Show this help\r\n");
    uart_send("  ping        - Respond with 'pong'\r\n");
}


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
        LOG_WRN("Unknown command: %s", cmd);
    }
}


/**
 * @brief UART module thread entry point.
 *
 * Initializes the UART device with interrupt-driven RX, then loops
 * waiting for complete commands via semaphore and dispatching them.
 * Created by main() via k_thread_create().
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

    uart_irq_callback_user_data_set(uart_dev, uart_isr_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("UART command interface ready");
    uart_send("Ready. Type 'help' for available commands.\r\n");

    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);

        dispatch_command(rx_buf);
    }
}
