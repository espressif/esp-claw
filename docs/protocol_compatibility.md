# Wave Rover Protocol Compatibility

## Reference JSON protocol vs MCP tools

| JSON cmd | T | Format | MCP equivalent |
|---|---|---|---|
| Speed ctrl | 1 | `{"T":1,"L":f,"R":f}` | `rover.drive_tank` with L/R ∈ [-1,1] |
| PWM input | 11 | `{"T":11,"L":i,"R":i}` | `rover.drive_tank` (scale to [-1,1]) |
| ROS ctrl | 13 | `{"T":13,"X":mps,"Z":radps}` | `rover.move` linear/angular |
| OLED ctrl | 3 | `{"T":3,"lineNum":n,"Text":"..."}` | `rover.display_text` |
| Get IMU | 126 | `{"T":126}` | `rover.get_imu` |
| Calibrate IMU | 127 | `{"T":127}` | (stub, not implemented in MVP) |
| Get IMU offset | 128 | `{"T":128}` | (not mapped in MVP) |
| Set IMU offset | 129 | `{"T":129,...}` | (not mapped in MVP) |

## Transport compatibility

Reference firmware transports (all still functional if running original firmware):
- UART0 @ 115200 baud (JSON over serial)
- USB-Serial (same as UART0)
- HTTP POST `/js?json=<cmd>` (Wi-Fi AP/STA)
- ESP-NOW broadcast

Our firmware replaces the above with:
- HTTP POST `/mcp` (JSON-RPC 2.0, port 8080)

The two firmwares are mutually exclusive (only one can run at a time).

## Motor control mapping

Reference: `setGoalSpeed(L, R)` where L/R ∈ [-1,1], WAVE ROVER mainType=1 → direct PWM = L×512×spd_rate.
Our HAL: `wr_motor_set(left, right)` where left/right ∈ [-1,1] → PWM = round(|v|×255).

Note: reference firmware multiplies by 512 (using 8-bit max 255 → effectively capped). Our formula
uses 255 directly, giving slightly lower max speed (same hardware limit, different scaling).
Adjust `max_speed` config (default 0.4) to match desired behaviour.

## Heartbeat / watchdog

Reference firmware: 3000 ms no-command → motors stop (heartBeatCtrl).
Our firmware: 10 000 ms HTTP idle → motors stop (keepalive timer in wave_rover_mcp.c).
