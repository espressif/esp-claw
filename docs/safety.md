# Wave Rover Safety Requirements

## Movement safety

1. All movement commands MUST include `duration_ms` (1–3000 ms).
2. After `duration_ms` elapses, motors stop automatically (worker task stops them).
3. If emergency stop is active, all movement commands return an error immediately.
4. If low battery is detected (`ps.low_battery`), a warning is logged.
5. At startup, motors are always stopped (`wr_motor_stop()` in app_main).
6. At MCP server idle >10 s, motors are stopped (keepalive timer in wave_rover_mcp.c).
7. `rover.clear_emergency_stop` does NOT start movement; it only clears the flag.
8. Motor commands go through a FreeRTOS queue; emergency stop is checked every 50 ms
   in the worker task and preempts any in-progress move command.

## Emergency stop

- Call `rover.emergency_stop` to latch motors off immediately.
- No movement is possible until `rover.clear_emergency_stop` with `confirm: true`.
- Emergency stop survives until explicitly cleared (not cleared by reboot — set at boot if `safe_mode` is enabled).

## Rate limiting

- `rover.move` / `rover.drive_tank` / `rover.turn` use the motor worker queue;
  the HTTP handler task is blocked until the move completes or is preempted.
- Maximum `duration_ms` = 3000 ms.
- Maximum speed = 0.4 (configurable via NVS `max_speed`).

## Wi-Fi credentials

- Credentials are never logged (not in NVS load, not in responses to `rover.get_wifi`).
- `rover.set_wifi` does not return the password in its response.
- Avoid entering Wi-Fi passwords in shell commands where they may appear in shell history.

## Dry-run mode

Default: `dry_run = true`. No hardware is accessed; all motor/sensor calls are no-ops.
Set `dry_run = false` in NVS config only after hardware connections are confirmed.
