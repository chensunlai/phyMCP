# phyMCP Usage

## Add To An ESP-IDF App

Use phyMCP as a local component:

```text
your_app/
  components/
    phyMCP/
      CMakeLists.txt
      include/
      src/
```

Then include the umbrella header:

```c
#include "phymcp.h"
```

## Kconfig

The component exposes one default option:

```text
Component config -> phyMCP -> Default ESP-NOW channel
```

Applications may also override the channel at runtime:

```c
phymcp_host_config_t cfg;
phymcp_host_default_config(&cfg);
cfg.transport.channel = 6;
```

The same pattern applies to `phymcp_device_config_t`.

## Device Side

Device firmware creates a tool table and starts the device responder:

```c
static const phymcp_tool_t tools[] = {
    {
        .name = "led.set",
        .description = "Set LED state.",
        .input_schema_json = "{\"type\":\"object\"}",
        .handler = led_set,
    },
};

phymcp_device_config_t cfg;
phymcp_device_default_config(&cfg);
cfg.name = "esp32c3_led";
cfg.class_name = "light";
cfg.model = "esp32c3";
cfg.firmware = "0.1.0";
cfg.tool_etag = "led-v1";
cfg.tools = tools;
cfg.tool_count = sizeof(tools) / sizeof(tools[0]);

ESP_ERROR_CHECK(phymcp_device_init(&cfg));
```

Handlers receive raw JSON arguments and write raw JSON results:

```c
static esp_err_t led_set(const char *arguments_json,
                         char *response_json,
                         size_t response_json_len,
                         void *ctx)
{
    return phymcp_make_tool_result_text_json("ok", false,
                                             response_json,
                                             response_json_len);
}
```

## Host Side

Host firmware registers an event callback:

```c
static void on_event(const phymcp_host_event_t *event, void *ctx)
{
    (void)ctx;
    printf("type=%d xid=%u json=%.*s\n",
           event->type,
           event->xid,
           (int)event->json_len,
           event->json ? event->json : "");
}

phymcp_host_config_t cfg;
phymcp_host_default_config(&cfg);
cfg.event_cb = on_event;

ESP_ERROR_CHECK(phymcp_host_init(&cfg));
```

Then send requests:

```c
uint16_t xid;
ESP_ERROR_CHECK(phymcp_host_discover(NULL, &xid));
ESP_ERROR_CHECK(phymcp_host_list_tools(mac, NULL, &xid));

phymcp_tool_call_options_t call = {
    .arguments_json = "{\"on\":true}",
    .timeout_ms = 1000,
};
ESP_ERROR_CHECK(phymcp_host_call_tool(mac, "led.set", &call, &xid));
ESP_ERROR_CHECK(phymcp_host_ping(mac, NULL, &xid));
```

## Notes

- ESP-NOW requires Wi-Fi to be initialized and started. The default transport
  configuration uses STA mode.
- Application payloads are JSON, but each ESP-NOW packet starts with a compact
  phyMCP binary header.
- The component does not reject scan results by RSSI.
- Serial bridges, displays, MCP servers, device name caches, and tool
  caches should live in applications or examples, not in this component.

## Python Host Interface

The repository includes `examples/host_mcp_interface/phymcp-host-interface.py`,
a generic MCP server for the host serial bridge. Use the `py311` conda
environment:

```powershell
conda activate py311
python examples/host_mcp_interface/phymcp-host-interface.py --serial-port COM5 --http-port 11451
```

It serves MCP over streamable HTTP at:

```text
http://127.0.0.1:11451/mcp
```

The exposed MCP tools are generic:

- `phymcp_serial_status`
- `phymcp_scan`
- `phymcp_devices`
- `phymcp_list_tools`
- `phymcp_call_tool`
- `phymcp_ping`

Use `phymcp_scan` first, then `phymcp_list_tools`, then call a discovered
device tool through `phymcp_call_tool`.

## LED Control Example

`examples/led_control` contains the copied host/device test firmware. It is a
normal ESP-IDF app that references this component from the repository root.

Build the default device firmware:

```powershell
cd examples/led_control
idf.py set-target esp32c3
idf.py build flash monitor
```

Build the host serial bridge firmware:

```powershell
cd examples/led_control
idf.py -B build_host -D SDKCONFIG=sdkconfig.host -D SDKCONFIG_DEFAULTS=sdkconfig.host.defaults build flash monitor
```

The device defaults target ESP32-C3 SuperMini style wiring:

- LED: GPIO8
