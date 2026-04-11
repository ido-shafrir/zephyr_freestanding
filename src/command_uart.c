#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "command_uart.h"
#include "w5500_net.h"
#include "utils.h"

LOG_MODULE_REGISTER(command_uart, LOG_LEVEL_DBG);

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
 * @brief Handle the 'ip_set' command.
 *
 * Parses and validates three IPv4 address arguments, then applies
 * the new IP configuration to the default network interface.
 * Usage: ip_set <ip> <mask> <gateway>
 *
 * @param args The argument string after "ip_set ".
 */
static void cmd_ip_set(const char *args)
{
    char buf[48];
    char *ip_str, *mask_str, *gw_str, *saveptr;

    /* Copy args since strtok_r modifies the string */
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    ip_str = strtok_r(buf, " ", &saveptr);
    mask_str = strtok_r(NULL, " ", &saveptr);
    gw_str = strtok_r(NULL, " ", &saveptr);

    if (ip_str == NULL || mask_str == NULL || gw_str == NULL) {
        uart_send("usage: ip_set <ip> <mask> <gateway>\r\n");
        return;
    }

    if (!is_valid_ipv4(ip_str)) {
        uart_send("error: invalid IP address\r\n");
        return;
    }
    if (!is_valid_ipv4(mask_str)) {
        uart_send("error: invalid netmask format\r\n");
        return;
    }
    if (!is_valid_netmask(mask_str)) {
        uart_send("error: netmask must be contiguous (e.g. 255.255.255.0)\r\n");
        return;
    }
    if (!is_valid_ipv4(gw_str)) {
        uart_send("error: invalid gateway\r\n");
        return;
    }

    struct net_ipv4_config cfg;

    if (net_addr_pton(AF_INET, ip_str, &cfg.addr) < 0 ||
        net_addr_pton(AF_INET, mask_str, &cfg.netmask) < 0 ||
        net_addr_pton(AF_INET, gw_str, &cfg.gw) < 0) {
        uart_send("error: failed to parse address\r\n");
        return;
    }

    int ret = net_set_ip(&cfg);

    if (ret < 0) {
        uart_send("error: failed to set IP\r\n");
    } else {
        uart_send("ok\r\n");
    }
}

/**
 * @brief Handle the 'ip_get' command.
 *
 * Reads the current IPv4 configuration from the network interface
 * and sends it over UART as "ip=<addr> mask=<mask> gw=<gw>".
 */
static void cmd_ip_get(void)
{
    struct net_ipv4_config cfg;
    int ret = net_get_ip(&cfg);

    if (ret < 0) {
        uart_send("error: no IP address assigned\r\n");
        return;
    }

    char addr_str[NET_IPV4_ADDR_LEN];
    char mask_str[NET_IPV4_ADDR_LEN];
    char gw_str[NET_IPV4_ADDR_LEN];

    net_addr_ntop(AF_INET, &cfg.addr, addr_str, sizeof(addr_str));
    net_addr_ntop(AF_INET, &cfg.netmask, mask_str, sizeof(mask_str));
    net_addr_ntop(AF_INET, &cfg.gw, gw_str, sizeof(gw_str));

    uart_send("ip=");
    uart_send(addr_str);
    uart_send(" mask=");
    uart_send(mask_str);
    uart_send(" gw=");
    uart_send(gw_str);
    uart_send("\r\n");
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
    uart_send("  help, h, ?,                   - Show this help\r\n");
    uart_send("  ping                          - Respond with 'pong'\r\n");
    uart_send("  ip_get                        - Show current IPv4 configuration\r\n");
    uart_send("  ip_set <ip> <mask> <gateway>  - Set static IPv4 configuration\r\n");
    uart_send("  ip_set_dhcp                   - Switch to DHCP\r\n");
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
    } else if (strcmp(cmd, "ip_get") == 0) {
        cmd_ip_get();
    } else if (strncmp(cmd, "ip_set ", 7) == 0) {
        cmd_ip_set(cmd + 7);
    } else if (strcmp(cmd, "ip_set_dhcp") == 0) {
        int ret = net_set_dhcp();
        if (ret < 0) {
            uart_send("error: failed to start DHCP\r\n");
        } else {
            uart_send("ok: DHCP started\r\n");
        }
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
 */
void command_uart_thread_entry(void *p1, void *p2, void *p3)
{
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_isr_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("USB/UART command interface ready");
    uart_send("ICB-FW ready. Type 'help' to test.\r\n");

    while (1) {
        k_sem_take(&rx_sem, K_FOREVER);

        dispatch_command(rx_buf);
    }
}
