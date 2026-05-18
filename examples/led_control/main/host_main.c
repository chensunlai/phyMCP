#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "phymcp.h"
#include "sdkconfig.h"

#define LINE_BUF_LEN 768

static void serial_write_raw(const char *data, size_t len)
{
#if CONFIG_PHYMCP_HOST_SERIAL_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
#else
    uart_write_bytes(CONFIG_PHYMCP_HOST_UART_PORT, data, len);
#endif
}

static void serial_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(buf)) {
        n = sizeof(buf);
    }
    serial_write_raw(buf, (size_t)n);
}

static int serial_read_byte(void)
{
    uint8_t ch = 0;
#if CONFIG_PHYMCP_HOST_SERIAL_USB_SERIAL_JTAG
    int n = usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY);
#else
    int n = uart_read_bytes(CONFIG_PHYMCP_HOST_UART_PORT, &ch, 1, portMAX_DELAY);
#endif
    return n == 1 ? ch : -1;
}

static esp_err_t serial_init(void)
{
#if CONFIG_PHYMCP_HOST_SERIAL_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = CONFIG_PHYMCP_HOST_USB_RX_BUFFER_SIZE,
        .tx_buffer_size = CONFIG_PHYMCP_HOST_USB_TX_BUFFER_SIZE,
    };
    return usb_serial_jtag_driver_install(&cfg);
#else
    uart_config_t cfg = {
        .baud_rate = CONFIG_PHYMCP_HOST_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CONFIG_PHYMCP_HOST_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_PHYMCP_HOST_UART_PORT,
                                 CONFIG_PHYMCP_HOST_UART_TX_GPIO,
                                 CONFIG_PHYMCP_HOST_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    return uart_driver_install(CONFIG_PHYMCP_HOST_UART_PORT,
                               CONFIG_PHYMCP_HOST_UART_RX_BUFFER_SIZE,
                               CONFIG_PHYMCP_HOST_UART_TX_BUFFER_SIZE,
                               0, NULL, 0);
#endif
}

static void trim_line(char *line)
{
    char *start = line;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }

    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
}

static char *next_token(char **cursor)
{
    char *p = *cursor;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return NULL;
    }

    char *token = p;
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    if (*p) {
        *p++ = '\0';
    }
    *cursor = p;
    return token;
}

static bool is_uint_token(const char *token)
{
    if (!token || *token == '\0') {
        return false;
    }
    while (*token) {
        if (!isdigit((unsigned char)*token)) {
            return false;
        }
        token++;
    }
    return true;
}

static void host_event_cb(const phymcp_host_event_t *event, void *ctx)
{
    (void)ctx;

    char mac[18];
    phymcp_mac_to_str(event->mac, mac);

    const char *kind = "raw";
    switch (event->type) {
    case PHYMCP_HOST_EVENT_DEVICE_FOUND:
        kind = "device";
        break;
    case PHYMCP_HOST_EVENT_TOOLS_LIST:
        kind = "tools";
        break;
    case PHYMCP_HOST_EVENT_TOOL_RESULT:
        kind = "result";
        break;
    case PHYMCP_HOST_EVENT_PONG:
        kind = "pong";
        break;
    case PHYMCP_HOST_EVENT_ERROR:
        kind = "error";
        break;
    default:
        break;
    }

    serial_printf("%s xid=%u mac=%s rssi=%d json=%.*s\r\n",
                  kind,
                  event->xid,
                  mac,
                  event->rssi,
                  (int)event->json_len,
                  event->json ? event->json : "");
}

static void print_help(void)
{
    serial_write_raw(
        "commands:\r\n"
        "  scan [name_prefix] [window_ms]\r\n"
        "  scan [window_ms]\r\n"
        "  tools <mac> [limit]\r\n"
        "  call <mac> <tool_name> <json_arguments>\r\n"
        "  ping <mac>\r\n"
        "  help\r\n",
        strlen(
            "commands:\r\n"
            "  scan [name_prefix] [window_ms]\r\n"
            "  scan [window_ms]\r\n"
            "  tools <mac> [limit]\r\n"
            "  call <mac> <tool_name> <json_arguments>\r\n"
            "  ping <mac>\r\n"
            "  help\r\n"));
}

static void handle_scan(char *args)
{
    char *first = next_token(&args);
    char *second = next_token(&args);
    char *prefix = first;
    char *window_s = second;
    uint32_t window_ms = CONFIG_PHYMCP_HOST_SCAN_WINDOW_MS;

    if (first && !second && is_uint_token(first)) {
        prefix = NULL;
        window_s = first;
    }
    if (window_s) {
        window_ms = (uint32_t)strtoul(window_s, NULL, 10);
    }

    phymcp_discover_options_t opts = {
        .name_prefix = prefix,
    };
    uint16_t xid = 0;
    esp_err_t ret = phymcp_host_discover(&opts, &xid);
    if (ret != ESP_OK) {
        serial_printf("error cmd=scan err=0x%x\r\n", ret);
        return;
    }

    serial_printf("ok cmd=scan xid=%u window=%lu prefix=%s\r\n",
                  xid, (unsigned long)window_ms, prefix ? prefix : "");
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    serial_printf("scanDone xid=%u\r\n", xid);
}

static bool parse_mac_arg(char **args, uint8_t mac[PHYMCP_MAC_LEN])
{
    char *mac_s = next_token(args);
    if (!mac_s || !phymcp_mac_from_str(mac_s, mac)) {
        serial_printf("error reason=bad_mac\r\n");
        return false;
    }
    return true;
}

static void handle_tools(char *args)
{
    uint8_t mac[PHYMCP_MAC_LEN];
    if (!parse_mac_arg(&args, mac)) {
        return;
    }

    char *limit_s = next_token(&args);
    phymcp_tools_list_options_t opts = {0};
    if (limit_s) {
        opts.limit = (uint8_t)strtoul(limit_s, NULL, 10);
    }

    uint16_t xid = 0;
    esp_err_t ret = phymcp_host_list_tools(mac, &opts, &xid);
    serial_printf(ret == ESP_OK ? "ok cmd=tools xid=%u\r\n" :
                                  "error cmd=tools err=0x%x\r\n",
                  ret == ESP_OK ? xid : ret);
}

static void handle_call(char *args)
{
    uint8_t mac[PHYMCP_MAC_LEN];
    if (!parse_mac_arg(&args, mac)) {
        return;
    }

    char *tool = next_token(&args);
    if (!tool) {
        serial_printf("error reason=missing_tool\r\n");
        return;
    }
    while (*args && isspace((unsigned char)*args)) {
        args++;
    }
    if (*args == '\0') {
        args = "{}";
    }

    phymcp_tool_call_options_t opts = {
        .arguments_json = args,
        .timeout_ms = CONFIG_PHYMCP_HOST_COMMAND_TIMEOUT_MS,
    };
    uint16_t xid = 0;
    esp_err_t ret = phymcp_host_call_tool(mac, tool, &opts, &xid);
    serial_printf(ret == ESP_OK ? "ok cmd=call xid=%u\r\n" :
                                  "error cmd=call err=0x%x\r\n",
                  ret == ESP_OK ? xid : ret);
}

static void handle_ping(char *args)
{
    uint8_t mac[PHYMCP_MAC_LEN];
    if (!parse_mac_arg(&args, mac)) {
        return;
    }

    phymcp_ping_options_t opts = {
        .nonce = "host",
    };
    uint16_t xid = 0;
    esp_err_t ret = phymcp_host_ping(mac, &opts, &xid);
    serial_printf(ret == ESP_OK ? "ok cmd=ping xid=%u\r\n" :
                                  "error cmd=ping err=0x%x\r\n",
                  ret == ESP_OK ? xid : ret);
}

static void handle_line(char *line)
{
    trim_line(line);
    if (line[0] == '\0') {
        return;
    }

    char *cursor = line;
    char *cmd = next_token(&cursor);
    if (!cmd) {
        return;
    }

    if (strcmp(cmd, "scan") == 0) {
        handle_scan(cursor);
    } else if (strcmp(cmd, "tools") == 0) {
        handle_tools(cursor);
    } else if (strcmp(cmd, "call") == 0) {
        handle_call(cursor);
    } else if (strcmp(cmd, "ping") == 0) {
        handle_ping(cursor);
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
    } else {
        serial_printf("error reason=unknown_command cmd=%s\r\n", cmd);
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_ERROR_CHECK(serial_init());

    phymcp_host_config_t config;
    phymcp_host_default_config(&config);
    config.event_cb = host_event_cb;
    config.transport.channel = CONFIG_PHYMCP_HOST_ESPNOW_CHANNEL;
    ESP_ERROR_CHECK(phymcp_host_init(&config));

#if CONFIG_PHYMCP_HOST_SERIAL_USB_SERIAL_JTAG
    const char *backend = "usb_serial_jtag";
#else
    const char *backend = "uart";
#endif
    serial_printf("ready role=host backend=%s baud=%d channel=%d\r\n",
                  backend,
#if CONFIG_PHYMCP_HOST_SERIAL_UART
                  CONFIG_PHYMCP_HOST_UART_BAUD_RATE,
#else
                  0,
#endif
                  CONFIG_PHYMCP_HOST_ESPNOW_CHANNEL);
    print_help();

    char line[LINE_BUF_LEN];
    size_t pos = 0;
    while (true) {
        int ch = serial_read_byte();
        if (ch < 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                handle_line(line);
                pos = 0;
            }
            continue;
        }
        if (ch == 0x08 || ch == 0x7f) {
            if (pos > 0) {
                pos--;
            }
            continue;
        }
        if (pos + 1 < sizeof(line)) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;
            serial_printf("error reason=line_too_long\r\n");
        }
    }
}
