# Rover Ops

Use this skill for trajectories that need more than one `rover_move` or
`rover_turn` call.

## Square Loop

To trace a square, repeat this four times:

1. `rover_move(x=0, y=60, duration_ms=1000)`
2. `rover_turn(direction="left", angle_deg=90, speed_percent=50)`

Observe each result. If any tool returns `emergency_stop`, abort and report.

## Approach And Grab

1. Use `unitv_scan` to confirm target presence.
2. Move toward the target in 500-1000 ms hops.
3. Re-check the scene between hops.
4. Open the gripper, nudge forward, close the gripper, then back up.

## Error Handling

Do not retry after `emergency_stop`. For `rover_turn` timeout, try a smaller
angle or use `rover_move(z=...)` when IMU is unavailable.
