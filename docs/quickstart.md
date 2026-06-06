# Wave Rover MCP Quick Start

## 1. Build and flash

```bash
cd application/wave_rover
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pio run -t upload
.venv/bin/pio device monitor
```

## 2. Connect to rover Wi-Fi

Default AP: **`WR-ESP32`** / password **`12345678`**

Rover IP in AP mode: `192.168.4.1`

## 3. Test MCP endpoint

```bash
# List available tools
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'

# Get rover status
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"rover.get_status","arguments":{}}}'

# Stop motors
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"rover.stop","arguments":{"reason":"manual test"}}}'
```

## 4. Use rover CLI

```bash
python3 tools/rover_cli.py --host 192.168.4.1 status
python3 tools/rover_cli.py --host 192.168.4.1 power
python3 tools/rover_cli.py --host 192.168.4.1 imu
python3 tools/rover_cli.py --host 192.168.4.1 display-status
python3 tools/rover_cli.py --host 192.168.4.1 stop
# Motion requires --allow-motion flag (rover must be in safe position):
python3 tools/rover_cli.py --host 192.168.4.1 move --linear 0.2 --duration-ms 500 --allow-motion
```

## 5. Run smoke tests

```bash
python3 tools/mcp_smoke_test.py --host 192.168.4.1
# With motion:
python3 tools/mcp_smoke_test.py --host 192.168.4.1 --allow-motion
```

## 6. Switch to STA mode (via curl)

```bash
# WARNING: never share this command with the password visible in logs or shell history
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"rover.set_wifi","arguments":{"mode":"sta","ssid":"YourSSID","password":"YourPass","save":true}}}'
# Reboot rover to apply
```

## 7. Enable dry-run → real hardware

The firmware starts with `dry_run = true`. Motor and sensor calls are no-ops until you change this.
Use `rover.set_wifi` with NVS save and a future NVS config tool, or flash `sdkconfig.defaults`
with `CONFIG_DRY_RUN=n` once hardware is verified.
