#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "command_uart.h"
#include "command_parse.h"
#include "config_store.h"
#include "event_log.h"
#include "time_service.h"

LOG_MODULE_REGISTER(uart, LOG_LEVEL_DBG);

/* ---------- Thread resources ---------- */
K_THREAD_STACK_DEFINE(uart_stack, UART_STACK_SIZE);
struct k_thread uart_thread_data;

/* ---------- UART device ----------
 * Bound to the `command-uart` devicetree alias so each board can pick
 * its own physical UART. Falls back to zephyr,console if no alias defined.
 */
#if DT_NODE_EXISTS(DT_ALIAS(command_uart))
static const struct device *const uart_dev = DEVICE_DT_GET(DT_ALIAS(command_uart));
#else
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#endif

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
    uart_send("  help, h, ?          - Show this help\r\n");
    uart_send("  ping                - Respond with 'pong'\r\n");
    uart_send("  ip_get              - Show current IP configuration\r\n");
    uart_send("  ip_set <ip> <m> <g> - Set static IP (addr mask gateway)\r\n");
    uart_send("  ip_set_dhcp         - Switch to DHCP\r\n");
    uart_send("  config_dump         - Dump persistent config\r\n");
    uart_send("  config_factory_reset- Restore factory defaults\r\n");
    uart_send("  event_log [seconds] - Dump recent events\r\n");
    uart_send("  log_level_set <lvl> - Set verbosity (error/warn/info/debug)\r\n");
    uart_send("  log_drop            - Erase all events (debug verbosity only)\r\n");
    uart_send("  time_get            - Show current wall-clock time\r\n");
    uart_send("  time_set <ISO8601>  - Set wall clock (YYYY-MM-DDTHH:MM:SSZ)\r\n");
    uart_send("  time_sync           - Trigger immediate SNTP sync\r\n");
}


/**
 * @brief Walk callback for event_log_read — prints each entry over UART.
 */
static bool uart_event_walk_cb(const event_entry_t *entry, void *user_data)
{
    ARG_UNUSED(user_data);
    char line[160];
    int len;

    if (entry->wall_clock != 0) {
        char iso[21];
        time_service_format_iso8601((int64_t)entry->wall_clock, iso, sizeof(iso));
        len = snprintf(line, sizeof(line), "[boot %u][%us %s] %s %s: %.*s\r\n",
                       entry->boot_id, entry->timestamp, iso,
                       event_severity_str(entry->severity),
                       event_type_str(entry->event_type),
                       EVENT_LOG_MSG_MAX_LEN, entry->message);
    } else {
        len = snprintf(line, sizeof(line), "[boot %u][%us] %s %s: %.*s\r\n",
                       entry->boot_id, entry->timestamp,
                       event_severity_str(entry->severity),
                       event_type_str(entry->event_type),
                       EVENT_LOG_MSG_MAX_LEN, entry->message);
    }

    if (len > 0) {
        uart_send(line);
    }
    return true; /* continue walking */
}

/**
 * @brief Handle 'event_log [seconds]' command with uptime fallback.
 */
static void cmd_event_log_dump(const char *seconds_str)
{
    uint32_t seconds = 0;
    if (seconds_str[0] != '\0') {
        seconds = (uint32_t)strtoul(seconds_str, NULL, 10);
    }

    uint32_t boot_id      = event_log_get_boot_id();
    uint32_t since_wall   = 0;
    uint32_t since_uptime = 0;
    bool     strict       = false;

    if (seconds != 0) {
        if (time_service_is_synced()) {
            uint32_t now_wall = (uint32_t)time_service_get();
            since_wall = (seconds > now_wall) ? 0 : now_wall - seconds;
        } else {
            /* No wall clock — fall back to uptime window, strict boot_id */
            uint32_t now_uptime = (uint32_t)(k_uptime_get() / 1000);
            since_uptime = (seconds > now_uptime) ? 0 : now_uptime - seconds;
            strict = true;
        }
    }

    int count = event_log_read(since_wall, since_uptime, boot_id,
                               strict, uart_event_walk_cb, NULL);
    char footer[96];
    snprintf(footer, sizeof(footer),
             "--- %d event(s) (this boot=%u; %s window %us) ---\r\n",
             count, boot_id,
             time_service_is_synced() ? "wall" : "uptime",
             seconds);
    uart_send(footer);
}

/**
 * @brief Handle 'log_level_set <level>' command.
 */
static void cmd_log_level_set(const char *level_str)
{
    if (level_str[0] == '\0') {
        uart_send("Usage: log_level_set <error|warn|info|debug>\r\n");
        return;
    }

    enum mcu_log_verbosity v;
    if (strcmp(level_str, "error") == 0) {
        v = MCU_LOG_ERROR;
    } else if (strcmp(level_str, "warn") == 0) {
        v = MCU_LOG_WARN;
    } else if (strcmp(level_str, "info") == 0) {
        v = MCU_LOG_INFO;
    } else if (strcmp(level_str, "debug") == 0) {
        v = MCU_LOG_DEBUG;
    } else {
        uart_send("error: invalid level (use error/warn/info/debug)\r\n");
        return;
    }

    config_store_set_log_verbosity(v);
    event_log_set_level((enum event_severity)v);

    char buf[48];
    snprintf(buf, sizeof(buf), "ok: log level set to %s\r\n", level_str);
    uart_send(buf);
}

/**
 * @brief Handle 'log_drop' command — erase all events (debug-only).
 */
static void cmd_log_drop(void)
{
    if (config_store_get_log_verbosity() != MCU_LOG_DEBUG) {
        uart_send("error: log_drop requires debug verbosity "
                  "(run 'log_level_set debug' first)\r\n");
        return;
    }

    int rc = event_log_drop_all();
    if (rc == 0) {
        uart_send("ok: event log erased\r\n");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "error: erase failed rc=%d\r\n", rc);
        uart_send(buf);
    }
}

/**
 * @brief Handle 'time_get' command.
 */
static void cmd_time_get(void)
{
    int64_t now = time_service_get();
    char iso[21];
    time_service_format_iso8601(now, iso, sizeof(iso));
    char buf[64];
    snprintf(buf, sizeof(buf), "time: %s (%s)\r\n", iso,
             time_service_is_synced() ? "synced" : "unsynced");
    uart_send(buf);
}

/**
 * @brief Handle 'time_set <ISO8601>' command.
 */
static void cmd_time_set(const char *iso_str)
{
    if (iso_str[0] == '\0') {
        uart_send("Usage: time_set YYYY-MM-DDTHH:MM:SSZ\r\n");
        return;
    }

    int64_t epoch;
    int rc = time_service_parse_iso8601(iso_str, &epoch);
    if (rc != 0) {
        uart_send("error: invalid ISO 8601 format\r\n");
        return;
    }

    rc = time_service_set(epoch);
    if (rc != 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "error: set failed rc=%d\r\n", rc);
        uart_send(buf);
        return;
    }

    uart_send("ok: wall clock set\r\n");
}

/**
 * @brief Handle 'time_sync' command — trigger immediate SNTP sync.
 */
static void cmd_time_sync(void)
{
    uart_send("triggering SNTP sync...\r\n");
    time_service_sync();
    uart_send("ok: sync requested\r\n");
}

/**
 * @brief Dispatch a received command string to the appropriate handler.
 *
 * Delegates all string parsing to parse_command() (command_parse.h),
 * then routes via switch on the command ID.
 *
 * @param cmd Null-terminated command string from the RX buffer.
 */
static void dispatch_command(const char *cmd)
{
    struct parsed_command pc;

    if (parse_command(cmd, &pc) != 0) {
        LOG_ERR("parse_command failed");
        return;
    }

    switch (pc.id) {
    case CMD_HELP:
        print_help();
        break;
    case CMD_PING:
        pong();
        break;
    case CMD_IP_GET:
        /* TODO: implement — call net_get_ip() when networking is enabled */
        uart_send("ip_get: not yet implemented\r\n");
        break;
    case CMD_IP_SET:
        if (pc.arg1[0] == '\0') {
            uart_send("Usage: ip_set <address> <mask> <gateway>\r\n");
        } else {
            /* TODO: implement — call net_set_ip() when networking is enabled */
            uart_send("ip_set: not yet implemented\r\n");
        }
        break;
    case CMD_IP_SET_DHCP:
        /* TODO: implement — call net_set_dhcp() when networking is enabled */
        uart_send("ip_set_dhcp: not yet implemented\r\n");
        break;
    case CMD_CONFIG_DUMP:
        config_store_dump();
        uart_send("ok: dumped to log\r\n");
        break;
    case CMD_CONFIG_FACTORY_RESET:
        config_store_factory_reset();
        uart_send("ok: factory defaults restored\r\n");
        break;
    case CMD_EVENT_LOG:
        cmd_event_log_dump(pc.arg1);
        break;
    case CMD_LOG_LEVEL_SET:
        cmd_log_level_set(pc.arg1);
        break;
    case CMD_LOG_DROP:
        cmd_log_drop();
        break;
    case CMD_TIME_GET:
        cmd_time_get();
        break;
    case CMD_TIME_SET:
        cmd_time_set(pc.arg1);
        break;
    case CMD_TIME_SYNC:
        cmd_time_sync();
        break;
    case CMD_UNKNOWN:
        uart_send("unknown command: ");
        uart_send(cmd);
        uart_send("\r\nType 'help' for available commands.\r\n");
        LOG_WRN("Unknown command: %s", cmd);
        break;
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
