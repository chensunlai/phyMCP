# phyMCP LED Control Example

This ESP-IDF example contains both sides of the current phyMCP test firmware:

- `device`: exposes an `led.set` tool over ESP-NOW, drives the ESP32-C3
  SuperMini LED on GPIO8.
- `host`: exposes a plain-text serial bridge for scan, tools/list, tool/call,
  and ping commands.

The example references the component at the repository root with
`EXTRA_COMPONENT_DIRS "../../"`.

## Build Device

```powershell
idf.py set-target esp32c3
idf.py build flash monitor
```

## Build Host

```powershell
idf.py set-target esp32c3
idf.py -B build_host -D SDKCONFIG=sdkconfig.host -D SDKCONFIG_DEFAULTS=sdkconfig.host.defaults build flash monitor
```

## Host Serial Commands

```text
help
scan [name_prefix] [window_ms]
tools <mac> [limit]
call <mac> <tool_name> <json_arguments>
ping <mac>
```

Example:

```text
scan
tools aa:bb:cc:dd:ee:ff
call aa:bb:cc:dd:ee:ff led.set {"toggle":true}
```
