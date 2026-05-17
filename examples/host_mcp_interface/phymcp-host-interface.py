#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import serial
from mcp.server.fastmcp import FastMCP
from serial.tools import list_ports


DEFAULT_MCP_HOST = "127.0.0.1"
DEFAULT_MCP_PORT = 11451
DEFAULT_SERIAL_BAUD = 115200
DEFAULT_COMMAND_TIMEOUT_S = 3.0
DEFAULT_SCAN_WINDOW_MS = 1500

MAC_RE = re.compile(r"^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$")


@dataclass
class ParsedLine:
    kind: str
    fields: dict[str, str] = field(default_factory=dict)
    json_text: str | None = None
    json_value: Any = None
    raw: str = ""


@dataclass
class DeviceEntry:
    mac: str
    name: str | None = None
    rssi: int | None = None
    info: dict[str, Any] = field(default_factory=dict)
    last_seen: float = 0.0

    def as_dict(self) -> dict[str, Any]:
        return {
            "id": self.name or self.mac,
            "mac": self.mac,
            "name": self.name,
            "rssi": self.rssi,
            "lastSeen": self.last_seen,
            "info": self.info,
        }


def _available_ports() -> list[dict[str, str]]:
    return [
        {
            "device": port.device,
            "description": port.description,
            "hwid": port.hwid,
        }
        for port in list_ports.comports()
    ]


def _parse_json(text: str | None) -> Any:
    if text is None or text == "":
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def _parse_line(line: str) -> ParsedLine | None:
    raw = line.strip()
    if not raw:
        return None

    prefix = raw
    json_text = None
    marker = " json="
    if marker in raw:
        prefix, json_text = raw.split(marker, 1)

    parts = prefix.split()
    if not parts:
        return None

    fields: dict[str, str] = {}
    for token in parts[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value

    return ParsedLine(
        kind=parts[0],
        fields=fields,
        json_text=json_text,
        json_value=_parse_json(json_text),
        raw=raw,
    )


def _int_field(fields: dict[str, str], name: str) -> int | None:
    value = fields.get(name)
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def _json_argument(arguments: dict[str, Any] | None,
                   arguments_json: str | None) -> str:
    if arguments_json:
        json.loads(arguments_json)
        return arguments_json
    return json.dumps(arguments or {}, separators=(",", ":"))


class PhyMCPHostInterface:
    def __init__(
        self,
        serial_port: str | None,
        baud: int = DEFAULT_SERIAL_BAUD,
        command_timeout_s: float = DEFAULT_COMMAND_TIMEOUT_S,
        scan_window_ms: int = DEFAULT_SCAN_WINDOW_MS,
    ) -> None:
        self.serial_port = serial_port
        self.baud = baud
        self.command_timeout_s = command_timeout_s
        self.scan_window_ms = scan_window_ms
        self._serial: serial.Serial | None = None
        self._lock = threading.RLock()
        self._devices: dict[str, DeviceEntry] = {}
        self._aliases: dict[str, str] = {}
        self._tool_cache: dict[str, dict[str, Any]] = {}

    def close(self) -> None:
        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.close()
            self._serial = None

    def serial_status(self) -> dict[str, Any]:
        ser = self._serial
        return {
            "configuredPort": self.serial_port,
            "baud": self.baud,
            "open": bool(ser and ser.is_open),
            "activePort": ser.port if ser else None,
            "availablePorts": _available_ports(),
        }

    def cached_devices(self) -> dict[str, Any]:
        with self._lock:
            return {
                "devices": [item.as_dict() for item in self._devices.values()],
                "aliases": dict(self._aliases),
            }

    def scan(self, name_prefix: str = "",
             window_ms: int | None = None) -> dict[str, Any]:
        window = window_ms or self.scan_window_ms
        if name_prefix:
            command = f"scan {name_prefix} {window}"
            effective_window = window
        else:
            command = "scan"
            effective_window = self.scan_window_ms

        with self._lock:
            self._write_command(command)
            xid, ok_line = self._wait_for_ok("scan", self.command_timeout_s)

            devices: list[dict[str, Any]] = []
            deadline = time.monotonic() + (effective_window / 1000.0) + self.command_timeout_s
            while time.monotonic() < deadline:
                parsed = self._read_parsed_until(deadline)
                if parsed is None:
                    break
                self._remember_line(parsed)
                if _int_field(parsed.fields, "xid") != xid:
                    continue
                if parsed.kind == "device":
                    devices.append(self._device_from_line(parsed))
                elif parsed.kind == "scanDone":
                    break
                elif parsed.kind == "error":
                    raise RuntimeError(parsed.raw)

            return {
                "xid": xid,
                "command": command,
                "ok": ok_line.raw,
                "devices": devices,
                "cache": self.cached_devices(),
            }

    def list_tools(self, device_id: str, limit: int = 8,
                   refresh: bool = True) -> dict[str, Any]:
        mac = self._resolve_device_id(device_id)
        if not refresh and mac in self._tool_cache:
            return {
                "device": self._device_dict(mac),
                "tools": self._tool_cache[mac],
                "cached": True,
            }

        with self._lock:
            self._write_command(f"tools {mac} {limit}")
            xid, ok_line = self._wait_for_ok("tools", self.command_timeout_s)
            parsed = self._wait_for_event({"tools", "error"}, xid,
                                          self.command_timeout_s)
            if parsed.kind == "error":
                raise RuntimeError(parsed.raw)
            self._remember_line(parsed)
            return {
                "xid": xid,
                "command": f"tools {mac} {limit}",
                "ok": ok_line.raw,
                "device": self._device_dict(mac),
                "tools": parsed.json_value,
                "raw": parsed.raw,
            }

    def call_tool(
        self,
        device_id: str,
        tool_name: str,
        arguments: dict[str, Any] | None = None,
        arguments_json: str | None = None,
    ) -> dict[str, Any]:
        mac = self._resolve_device_id(device_id)
        payload = _json_argument(arguments, arguments_json)
        command = f"call {mac} {tool_name} {payload}"

        with self._lock:
            self._write_command(command)
            xid, ok_line = self._wait_for_ok("call", self.command_timeout_s)
            parsed = self._wait_for_event({"result", "error"}, xid,
                                          self.command_timeout_s)
            self._remember_line(parsed)
            if parsed.kind == "error":
                raise RuntimeError(parsed.raw)
            return {
                "xid": xid,
                "command": command,
                "ok": ok_line.raw,
                "device": self._device_dict(mac),
                "tool": tool_name,
                "result": parsed.json_value,
                "raw": parsed.raw,
            }

    def ping(self, device_id: str) -> dict[str, Any]:
        mac = self._resolve_device_id(device_id)
        command = f"ping {mac}"

        with self._lock:
            self._write_command(command)
            xid, ok_line = self._wait_for_ok("ping", self.command_timeout_s)
            parsed = self._wait_for_event({"pong", "error"}, xid,
                                          self.command_timeout_s)
            self._remember_line(parsed)
            if parsed.kind == "error":
                raise RuntimeError(parsed.raw)
            return {
                "xid": xid,
                "command": command,
                "ok": ok_line.raw,
                "device": self._device_dict(mac),
                "pong": parsed.json_value,
                "raw": parsed.raw,
            }

    def _ensure_open(self) -> serial.Serial:
        if self._serial and self._serial.is_open:
            return self._serial

        port = self.serial_port or self._guess_single_port()
        if not port:
            raise RuntimeError(
                "No serial port configured. Pass --serial-port or set "
                "PHYMCP_SERIAL_PORT. Available ports: "
                + json.dumps(_available_ports(), ensure_ascii=False)
            )

        self._serial = serial.Serial(
            port=port,
            baudrate=self.baud,
            timeout=0.05,
            write_timeout=1.0,
        )
        time.sleep(0.2)
        self._drain_input(0.3)
        return self._serial

    def _guess_single_port(self) -> str | None:
        ports = _available_ports()
        return ports[0]["device"] if len(ports) == 1 else None

    def _drain_input(self, duration_s: float) -> None:
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline and self._serial:
            raw = self._serial.readline()
            if not raw:
                continue
            parsed = _parse_line(raw.decode(errors="replace"))
            if parsed:
                self._remember_line(parsed)

    def _write_command(self, command: str) -> None:
        ser = self._ensure_open()
        ser.write((command.rstrip() + "\n").encode("utf-8"))
        ser.flush()

    def _read_parsed_until(self, deadline: float) -> ParsedLine | None:
        ser = self._ensure_open()
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            parsed = _parse_line(raw.decode("utf-8", errors="replace"))
            if parsed:
                return parsed
        return None

    def _wait_for_ok(self, cmd: str,
                     timeout_s: float) -> tuple[int, ParsedLine]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            parsed = self._read_parsed_until(deadline)
            if parsed is None:
                break
            self._remember_line(parsed)
            if parsed.kind == "ok" and parsed.fields.get("cmd") == cmd:
                xid = _int_field(parsed.fields, "xid")
                if xid is None:
                    raise RuntimeError(f"ok line has no xid: {parsed.raw}")
                return xid, parsed
            if parsed.kind == "error" and (
                parsed.fields.get("cmd") == cmd or "reason" in parsed.fields
            ):
                raise RuntimeError(parsed.raw)
        raise TimeoutError(f"Timed out waiting for ok cmd={cmd}")

    def _wait_for_event(self, kinds: set[str], xid: int,
                        timeout_s: float) -> ParsedLine:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            parsed = self._read_parsed_until(deadline)
            if parsed is None:
                break
            self._remember_line(parsed)
            if _int_field(parsed.fields, "xid") == xid and parsed.kind in kinds:
                return parsed
        raise TimeoutError(f"Timed out waiting for {sorted(kinds)} xid={xid}")

    def _remember_line(self, parsed: ParsedLine) -> None:
        if parsed.kind == "device":
            self._device_from_line(parsed)
            return

        mac = parsed.fields.get("mac")
        if not mac:
            return
        mac = mac.lower()
        entry = self._devices.get(mac)
        if entry:
            rssi = _int_field(parsed.fields, "rssi")
            if rssi is not None:
                entry.rssi = rssi
            entry.last_seen = time.time()
        if parsed.kind == "tools" and isinstance(parsed.json_value, dict):
            self._tool_cache[mac] = parsed.json_value

    def _device_from_line(self, parsed: ParsedLine) -> dict[str, Any]:
        mac = parsed.fields.get("mac", "").lower()
        if not MAC_RE.match(mac):
            return {"raw": parsed.raw, "json": parsed.json_value}

        info = parsed.json_value if isinstance(parsed.json_value, dict) else {}
        name = info.get("name") if isinstance(info.get("name"), str) else None
        rssi = _int_field(parsed.fields, "rssi")
        entry = self._devices.get(mac) or DeviceEntry(mac=mac)
        entry.name = name or entry.name
        entry.rssi = rssi
        entry.info = info
        entry.last_seen = time.time()
        self._devices[mac] = entry
        if entry.name:
            self._aliases[entry.name] = mac
        return entry.as_dict()

    def _resolve_device_id(self, device_id: str) -> str:
        item = device_id.strip()
        if MAC_RE.match(item):
            return item.lower()
        if item in self._aliases:
            return self._aliases[item]
        raise KeyError(
            f"Unknown device id '{device_id}'. Run phymcp_scan first or use a MAC address."
        )

    def _device_dict(self, mac: str) -> dict[str, Any]:
        entry = self._devices.get(mac.lower())
        return entry.as_dict() if entry else {"id": mac.lower(), "mac": mac.lower()}


def build_server(args: argparse.Namespace) -> FastMCP:
    interface = PhyMCPHostInterface(
        serial_port=args.serial_port,
        baud=args.baud,
        command_timeout_s=args.command_timeout,
        scan_window_ms=args.scan_window_ms,
    )

    mcp = FastMCP(
        "phyMCP Host Interface",
        instructions=(
            "Generic bridge from MCP to a phyMCP ESP-NOW host over serial. "
            "Discover devices first, inspect their advertised tool lists, "
            "then call a device tool by device id and tool name."
        ),
        host=args.host,
        port=args.http_port,
        streamable_http_path=args.path,
    )

    @mcp.tool()
    def phymcp_serial_status() -> dict[str, Any]:
        """Return serial configuration, open state, and available local ports."""
        return interface.serial_status()

    @mcp.tool()
    def phymcp_scan(name_prefix: str = "",
                    window_ms: int | None = None) -> dict[str, Any]:
        """Scan nearby phyMCP devices and update the device-name cache."""
        return interface.scan(name_prefix=name_prefix, window_ms=window_ms)

    @mcp.tool()
    def phymcp_devices() -> dict[str, Any]:
        """Return cached devices discovered by previous scan or command output."""
        return interface.cached_devices()

    @mcp.tool()
    def phymcp_list_tools(device_id: str,
                          limit: int = 8,
                          refresh: bool = True) -> dict[str, Any]:
        """Read the advertised MCP-like tool list from one device."""
        return interface.list_tools(device_id=device_id, limit=limit, refresh=refresh)

    @mcp.tool()
    def phymcp_call_tool(
        device_id: str,
        tool_name: str,
        arguments: dict[str, Any] | None = None,
        arguments_json: str | None = None,
    ) -> dict[str, Any]:
        """Call an advertised device tool without hardcoding any physical tool names."""
        return interface.call_tool(
            device_id=device_id,
            tool_name=tool_name,
            arguments=arguments,
            arguments_json=arguments_json,
        )

    @mcp.tool()
    def phymcp_ping(device_id: str) -> dict[str, Any]:
        """Send a lightweight ping to a cached device name or MAC address."""
        return interface.ping(device_id=device_id)

    return mcp


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="MCP server for a phyMCP ESP-NOW host serial bridge."
    )
    parser.add_argument(
        "--serial-port",
        default=os.environ.get("PHYMCP_SERIAL_PORT"),
        help="Serial port connected to the phyMCP host, for example COM5.",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=int(os.environ.get("PHYMCP_SERIAL_BAUD", DEFAULT_SERIAL_BAUD)),
        help="Serial baud rate for UART backends. USB CDC tools may ignore it.",
    )
    parser.add_argument(
        "--host",
        default=os.environ.get("PHYMCP_MCP_HOST", DEFAULT_MCP_HOST),
        help="HTTP bind host for the MCP server.",
    )
    parser.add_argument(
        "--http-port",
        type=int,
        default=int(os.environ.get("PHYMCP_MCP_PORT", DEFAULT_MCP_PORT)),
        help="HTTP port for the MCP server.",
    )
    parser.add_argument(
        "--path",
        default=os.environ.get("PHYMCP_MCP_PATH", "/mcp"),
        help="Streamable HTTP path.",
    )
    parser.add_argument(
        "--transport",
        choices=("streamable-http", "sse", "stdio"),
        default=os.environ.get("PHYMCP_MCP_TRANSPORT", "streamable-http"),
        help="MCP transport.",
    )
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=float(os.environ.get("PHYMCP_COMMAND_TIMEOUT", DEFAULT_COMMAND_TIMEOUT_S)),
        help="Seconds to wait for unicast command responses.",
    )
    parser.add_argument(
        "--scan-window-ms",
        type=int,
        default=int(os.environ.get("PHYMCP_SCAN_WINDOW_MS", DEFAULT_SCAN_WINDOW_MS)),
        help="Default scan collection window used by the host firmware.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = build_server(args)
    print(
        f"phyMCP host-interface on {args.host}:{args.http_port}{args.path} "
        f"transport={args.transport} serial={args.serial_port or 'auto'} "
        f"baud={args.baud}",
        flush=True,
    )
    server.run(transport=args.transport)


if __name__ == "__main__":
    main()
