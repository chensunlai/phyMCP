#include <stdbool.h>
#include <stdio.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "phymcp.h"
#include "sdkconfig.h"

static bool s_led_on;
static uint32_t s_call_count;

static void led_apply(bool on)
{
    s_led_on = on;
#if CONFIG_PHYMCP_DEMO_LED_ACTIVE_HIGH
    const int level = on ? 1 : 0;
#else
    const int level = on ? 0 : 1;
#endif
    gpio_set_level(CONFIG_PHYMCP_DEMO_LED_GPIO, level);
}

static esp_err_t led_set_tool(const char *arguments_json,
                              char *response_json,
                              size_t response_json_len,
                              void *ctx)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(arguments_json ? arguments_json : "{}");
    if (!root) {
        return phymcp_make_tool_result_text_json("invalid arguments", true,
                                                response_json,
                                                response_json_len);
    }

    cJSON *toggle = cJSON_GetObjectItem(root, "toggle");
    cJSON *on = cJSON_GetObjectItem(root, "on");
    if (cJSON_IsTrue(toggle)) {
        led_apply(!s_led_on);
    } else if (cJSON_IsBool(on)) {
        led_apply(cJSON_IsTrue(on));
    } else {
        cJSON_Delete(root);
        return phymcp_make_tool_result_text_json("expected {\"on\":true} or {\"toggle\":true}",
                                                true, response_json,
                                                response_json_len);
    }
    cJSON_Delete(root);

    s_call_count++;

    char text[32];
    snprintf(text, sizeof(text), "%s calls=%lu",
             s_led_on ? "led on" : "led off",
             (unsigned long)s_call_count);
    return phymcp_make_tool_result_text_json(text, false,
                                            response_json,
                                            response_json_len);
}

static const phymcp_tool_t s_tools[] = {
    {
        .name = "led.set",
        .description = "Set or toggle the board LED.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"},\"toggle\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
        .destructive = false,
        .handler = led_set_tool,
    },
};

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_PHYMCP_DEMO_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    led_apply(false);

    phymcp_device_config_t config;
    phymcp_device_default_config(&config);
    config.name = "esp32c3_led";
    config.class_name = "light";
    config.model = "esp32c3-supermini";
    config.firmware = "0.1.0";
    config.tool_etag = "led-v1";
    config.tools = s_tools;
    config.tool_count = sizeof(s_tools) / sizeof(s_tools[0]);
    config.transport.channel = 6;

    ESP_ERROR_CHECK(phymcp_device_init(&config));
}
