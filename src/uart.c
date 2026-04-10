#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include "uart.h"

LOG_MODULE_REGISTER(uart, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(uart_stack, UART_STACK_SIZE);
struct k_thread uart_thread_data;

/* ---------- UART device ----------
 * Currently uses the console UART (usart3 on Nucleo H753ZI, via ST-Link USB).
 * To use a different UART:
 *   - By DT chosen:  DEVICE_DT_GET(DT_CHOSEN(zephyr_console))
 *   - By node label: DEVICE_DT_GET(DT_NODELABEL(usart2))
 *   - By alias:      DEVICE_DT_GET(DT_ALIAS(my_uart))
 */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ---------- RX buffer and signaling ---------- */
#define RX_BUF_SIZE 64

static char rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_pos;
static K_SEM_DEFINE(rx_sem, 0, 1);


/* ---------- UART ISR callback ---------- */
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
                k_sem_give(&rx_sem);
            }
        } else if (rx_pos < RX_BUF_SIZE - 1) {
            rx_buf[rx_pos++] = c;
        }
    }
}

/* ---------- Send a string over UART ---------- */
static void uart_send(const char *str)
{
    while (*str) {
        uart_poll_out(uart_dev, *str++);
    }
}


/* ---------- Ping/Pong command ---------- */
void pong(void)
{
    uart_send("pong\r\n");
    LOG_INF("Received 'ping', sent 'pong'");
}



/* ---------- Command dispatch ---------- */
static void dispatch_command(const char *cmd)
{
    if (strcmp(cmd, "ping") == 0) {
        pong();
    } else {
        uart_send("unknown command: ");
        uart_send(cmd);
        uart_send("\r\n");
        LOG_WRN("Unknown command: %s", cmd);
    }
}






/* ---------- Thread entry point ---------- */
void uart_thread_entry(void *p1, void *p2, void *p3)
{
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_isr_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("USB/UART command interface ready");
    uart_send("ICB-FW ready. Type 'ping' to test.\r\n");

    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);

        dispatch_command(rx_buf);
        rx_pos = 0;
    }
}
