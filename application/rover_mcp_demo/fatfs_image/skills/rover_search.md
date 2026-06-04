# Rover Search

Use this skill when the user asks to find a named object.

## Procedure

1. Call `unitv_scan` once for a quick check.
2. If uncertain, call `unitv_capture` with a focused question about the target.
3. If not found, sweep by repeating:
   - `rover_turn(direction="left", angle_deg=45, speed_percent=40)`
   - `unitv_scan`
4. Stop at the first likely sighting and confirm with `unitv_capture`.

## Limits

Do not exceed 12 vision calls in one user turn. Do not combine search with
blind driving into unknown space. The camera is fixed; only rover movement
changes the view.
