# phyMCP Architecture

phyMCP is a small ESP-IDF component for exposing nearby physical devices as
MCP-like tool providers over ESP-NOW.

## Goals

- Discover nearby devices without Wi-Fi association.
- Let each device advertise its own tool list.
- Call tools with JSON arguments and JSON results.
- Keep embedded state, retry logic, and memory use small.
- Leave rich behavior such as aliases, caching, policy, and MCP server
  wrapping to the host-interface or application layer.

## Layers

Frame layer:

- Packs and unpacks a 12-byte binary header.
- Treats payloads as opaque UTF-8 JSON strings.
- Provides MAC string helpers and small MCP-style result/error helpers.

Transport layer:

- Initializes NVS, netif, Wi-Fi STA mode, and ESP-NOW when requested.
- Registers broadcast and unicast peers.
- Receives ESP-NOW packets in the ESP-NOW callback and moves them into a
  FreeRTOS queue.
- Unpacks phyMCP frames in a worker task before invoking the application
  callback.

Device layer:

- Owns the device identity and tool registry.
- Handles `discover`, `tools/list`, `tool/call`, and `ping`.
- Adds 0-1000 ms random jitter to discovery responses.
- Keeps a small duplicate cache for recent tool calls.

Host layer:

- Provides C functions for discover, list tools, call tool, and ping.
- Converts received frames into typed host events.
- Does not cache devices or tools.

## Frame Format

```text
offset  size  name
0       2     magic "PM"
2       1     version = 1
3       1     message type
4       1     flags
5       1     header length = 12
6       2     seq, little-endian
8       2     xid, little-endian
10      2     JSON payload length, little-endian
12      n     JSON payload
```

Message types:

- `1`: discover
- `2`: tools/list
- `3`: tool/call
- `4`: ping

Flags:

- `PHYMCP_FLAG_ACK`: reserved for explicit ACK use
- `PHYMCP_FLAG_ERR`: payload is an error object

## Reliability Model

The component intentionally avoids a heavy session protocol:

- Broadcast scan: send once, optionally retry once above this component.
- Device discovery response: random delay from 0 to 1000 ms.
- Unicast request: retry up to two times above this component using the same
  `xid`.
- Tool call duplicate handling: device cache prevents repeated side effects
  during the duplicate window.
- Timeout: fixed per command, usually 1000-1500 ms at the host-interface
  layer.
- Virtual connection: send `ping` periodically while a device is selected.

RSSI is reported as metadata only. The component does not filter scan results
by signal strength.

## Extension Points

To add a device tool, register a `phymcp_tool_t` with:

- `name`: MCP-style tool name.
- `description`: human-readable summary.
- `input_schema_json`: JSON schema string.
- `handler`: function that receives argument JSON and writes result JSON.

The host should never hardcode device-specific tool names. It should discover
tools at runtime with `phymcp_host_list_tools()`.
