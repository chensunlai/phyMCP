# phyMCP

phyMCP is a lightweight physical MCP-style protocol for nearby IoT devices.
It lets an agent-side host discover devices over ESP-NOW, read each device's
tool list, call tools, and send lightweight heartbeat requests.

This repository is an ESP-IDF component library, not a complete firmware
project. Applications provide their own `main/` firmware, serial bridge,
MCP server wrapper, device cache, display UI, and authorization policy.

## Status

Current version: `0.1.0`

The protocol and C API are still experimental. The current implementation is
small on purpose and favors predictable timeouts over a heavy session layer.

## Repository Layout

```text
include/
  phymcp.h                 umbrella include
  phymcp_frame.h           binary frame format and helpers
  phymcp_transport.h       ESP-NOW transport API
  phymcp_host.h            host-side request API
  phymcp_device.h          device-side tool registry API

src/
  phymcp_frame.c
  phymcp_transport_espnow.c
  phymcp_host.c
  phymcp_device.c

docs/
  architecture.md
  usage.md

examples/
  led_control/
  host_mcp_interface/
```

`examples/led_control/` contains the ESP-IDF host/device test firmware for an
LED device. `examples/host_mcp_interface/` contains the Python MCP host
interface.

## Install

As a local component, copy or submodule this repository into an ESP-IDF
application:

```text
your_app/
  components/
    phyMCP/
      CMakeLists.txt
      include/
      src/
```

After publishing to the ESP Component Registry, an application can depend on
it from its own `idf_component.yml`.

```yaml
dependencies:
  yourname/phymcp: "^0.1.0"
```

## Requirements

- ESP-IDF `>=5.5`
- ESP-NOW from ESP-IDF's built-in `esp_now.h`
- No compatibility layer for ESP-NOW v1.0
- Wi-Fi initialized and started in STA mode by default

## Protocol

phyMCP sends a compact binary header plus a UTF-8 JSON payload:

```text
magic[2] = "PM"
version  = 1
type     = 1 discover, 2 tools/list, 3 tool/call, 4 ping
flags    = bit0 ack, bit1 error
hlen     = 12
seq      = uint16 little-endian
xid      = uint16 little-endian transaction id
len      = uint16 little-endian JSON payload length
payload  = UTF-8 JSON
```

The ESP-NOW packet is not pure JSON. JSON is only the application payload
inside the phyMCP frame.

## Message Flow

The host side can send four request types:

- `discover`: broadcast scan for nearby devices.
- `tools/list`: request an MCP-like tool list from one device.
- `tool/call`: call a named tool with JSON arguments.
- `ping`: lightweight liveness and signal check.

The device side registers a list of tools and responds to those requests.
Tools are not predefined by phyMCP. A generic host must call `tools/list`
instead of assuming names such as `led.set`.

## Minimal Device

```c
#include "phymcp.h"

static esp_err_t led_set(const char *arguments_json,
                         char *response_json,
                         size_t response_json_len,
                         void *ctx)
{
    (void)arguments_json;
    (void)ctx;
    return phymcp_make_tool_result_text_json("ok", false,
                                             response_json,
                                             response_json_len);
}

static const phymcp_tool_t tools[] = {
    {
        .name = "led.set",
        .description = "Set LED state.",
        .input_schema_json = "{\"type\":\"object\"}",
        .handler = led_set,
    },
};

void app_main(void)
{
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
}
```

## Minimal Host

```c
#include "phymcp.h"

static void on_host_event(const phymcp_host_event_t *event, void *ctx)
{
    (void)ctx;
    char mac[18];
    phymcp_mac_to_str(event->mac, mac);
    printf("event=%d mac=%s xid=%u json=%.*s\n",
           event->type,
           mac,
           event->xid,
           (int)event->json_len,
           event->json ? event->json : "");
}

void app_main(void)
{
    phymcp_host_config_t cfg;
    phymcp_host_default_config(&cfg);
    cfg.event_cb = on_host_event;

    ESP_ERROR_CHECK(phymcp_host_init(&cfg));
    ESP_ERROR_CHECK(phymcp_host_discover(NULL, NULL));
}
```

## Reliability Model

- Broadcast discovery: devices respond after random 0-300 ms jitter.
- Unicast requests: upper layers may retry with the same `xid`.
- Duplicate tool calls: devices keep a small duplicate cache to avoid
  repeating side effects during the cache window.
- Timeouts and device caches belong in the host-interface or application
  layer, not inside this component.

## Documentation

- [Architecture](docs/architecture.md)
- [Usage](docs/usage.md)

## Firmware Example

`examples/led_control/` is a complete ESP-IDF app with two firmware roles:

- `device`: exposes an `led.set` demo tool for the board LED.
- `host`: provides the plain-text serial bridge used by the Python MCP
  interface.

Build the default device role:

```powershell
cd examples/led_control
idf.py set-target esp32c3
idf.py build flash monitor
```

Build the host role with its defaults:

```powershell
cd examples/led_control
idf.py -B build_host -D SDKCONFIG=sdkconfig.host -D SDKCONFIG_DEFAULTS=sdkconfig.host.defaults build flash monitor
```

## Host Interface

`examples/host_mcp_interface/phymcp-host-interface.py` is a generic MCP server
that talks to a phyMCP host firmware through the plain-text serial bridge. It
does not hardcode physical device tools; it exposes scan, device cache,
tool-list, generic tool-call, and ping operations.

```powershell
python examples/host_mcp_interface/phymcp-host-interface.py --serial-port COM5 --http-port 11451
```

Default MCP endpoint:

```text
http://127.0.0.1:11451/mcp
```
