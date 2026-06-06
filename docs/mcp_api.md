# Wave Rover MCP API Reference

Endpoint: `POST http://<rover-ip>:8080/mcp` (JSON-RPC 2.0)

## Protocol methods

| Method | Description |
|---|---|
| `initialize` | MCP handshake; returns `protocolVersion`, `capabilities` |
| `ping` | Liveness check |
| `tools/list` | List all available tools |
| `tools/call` | Invoke a tool: `{"name":"rover.X","arguments":{...}}` |
| `resources/list` | List available resource URIs |
| `resources/read` | Read a resource: `{"uri":"rover://..."}` |

## Resources

| URI | Content |
|---|---|
| `rover://status` | Motor state + emergency stop flag |
| `rover://power` | INA219 voltage, current |
| `rover://ups` | Battery state alias |
| `rover://imu` | IMU present flag |
| `rover://config` | Config stub (ok: true) |
| `rover://wifi` | Wi-Fi config stub |
| `rover://display` | Display stub |
| `rover://logs/recent` | Log ring buffer (not implemented in MVP) |

## Tools

### System

| Tool | Parameters | Returns |
|---|---|---|
| `rover.get_status` | — | firmware info, motor state, power, IMU presence, uptime, free heap |
| `rover.get_config` | — | hostname, mcp_port, auth_enabled, dry_run, max_speed, max_cmd_ms |

### Motor

| Tool | Parameters | Returns |
|---|---|---|
| `rover.move` | `linear` float [-1,1], `angular` float [-1,1], `duration_ms` int [1,3000] | `ok`, `action`, speeds, duration |
| `rover.drive_tank` | `left` float [-1,1], `right` float [-1,1], `duration_ms` int [1,3000] | `ok`, `action`, speeds |
| `rover.turn` | `direction` "left"\|"right", `speed` float [0,1], `duration_ms` int [1,3000] | `ok`, `action`, direction |
| `rover.stop` | `reason` string (optional) | `ok`, `action` |
| `rover.emergency_stop` | `reason` string (optional) | `ok`, `action` |
| `rover.clear_emergency_stop` | `confirm` bool (must be true) | `ok`, `action` |

### Sensors

| Tool | Parameters | Returns |
|---|---|---|
| `rover.get_power` | — | INA219 bus/shunt voltage, current, power, low_battery |
| `rover.get_ups` | — | battery voltage, current, charging, discharging, low_battery |
| `rover.get_imu` | — | accel, gyro, mag (if present), temperature |

### Display

| Tool | Parameters | Returns |
|---|---|---|
| `rover.display_text` | `text` string, `line` int [0,3], `clear` bool | `ok` |
| `rover.display_clear` | — | `ok` |
| `rover.display_status` | — | `ok` (renders FW/WiFi/Batt/MCP on OLED) |

### Wi-Fi

| Tool | Parameters | Returns |
|---|---|---|
| `rover.get_wifi` | — | `mode`, `ssid`, `hostname` (no password) |
| `rover.set_wifi` | `ssid`, `password`, `mode` ("ap"\|"sta"\|"ap_sta"), `save` bool | `ok`, `note` ("reboot_to_apply") |

## Error responses

All tool errors return `{"ok": false, "error": "<reason>"}` in the result.
JSON-RPC level errors use standard error codes (-32600 parse, -32601 method not found, -32603 internal).
