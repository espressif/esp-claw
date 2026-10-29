---
{
  "name": "stackchan_head",
  "description": "Move the StackChan robot's head by angle: look up/down/left/right, nod yes, shake no, centre, or report where the head is now. Also calibrates and stores this unit's zero point and travel limits. StackChan board only; angles are only meaningful after calibration.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_files"
    ],
    "manage_mode": "readonly"
  }
}
---

# StackChan Head

Move the StackChan head in degrees, and calibrate the zero that degrees are measured from.

Run exactly one script with `lua_run_script`: `{CUR_SKILL_DIR}/scripts/head.lua`. Every call takes an `action`.

Two axes, each a Feetech SCS serial servo on UART 1: **yaw** (id 1, left/right) and **pitch** (id 2, up/down). Do not open the servo UART from another script at the same time.

- Angles are degrees from the stored **zero**, not encoder positions. **Positive pitch is up, positive yaw is left.**
- Until `calibrate_zero` has run, the script uses factory guesses and says so in `status`. Read `{CUR_SKILL_DIR}/references/calibration.md` before running any `calibrate_*` action, and ask the user first — those actions overwrite stored per-unit data. Moving the head needs no confirmation.
- A move reports where the head actually settled. Yaw normally needs a correction pass to arrive, which `trim` does by default.
- On any error, read `{CUR_SKILL_DIR}/references/troubleshooting.md` and report what it says. Do not retry with a different angle.

## Safety Boundaries

Enforced by the script, not left to the caller:

- **Pitch has real mechanical stops** near 576..863 counts, varying by unit. Commands stay inside the calibrated travel, which keeps 8 counts clear of the stops it measured. Driving into a stop stalls the servo and heats it.
- **Yaw's limit is not mechanical** but cable and FPC twist through the joint, so it defaults to ±90° from zero and cannot be widened by a sweep.
- Angles outside the allowed range are clamped and the clamp is reported. Do not retry with a bigger number.
- A configuration naming a travel no real unit could have is discarded in favour of known-good defaults.

## Actions

| `action` | Effect |
|----------|--------|
| `status` | Both axes: angle, allowed range, counts, zero, direction, dead band, load, plus bus voltage and temperature. No motion. Use this first. |
| `move` | Go to `yaw_deg` and/or `pitch_deg`; an omitted axis holds still. `relative: true` treats them as offsets. |
| `center` | Both axes back to the stored zero. |
| `nod` | Pitch up and down `times` times around the current pose, then back. Reads as "yes". |
| `shake` | Same on yaw. Reads as "no". |
| `torque` | `enabled: false` releases the servos so the head can be moved by hand; `true` holds again. |
| `jog` | Step one axis by raw `counts` (max 40). |
| `calibrate_zero` | Take the current pose as 0°, or the middle of the travel with `mid_travel: true`. |
| `calibrate_range` | Release torque, watch an axis for `seconds` while the user sweeps it by hand, store the travel. |
| `set_direction` | Record `direction` (`1` or `-1`) for an axis. |

`axis` selects `yaw`, `pitch`, or `both` (default) for `torque`, `calibrate_zero`, `calibrate_range`, and `set_direction`. `jog` requires a single axis.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "action": {"type": "string", "default": "status",
               "enum": ["status", "move", "center", "nod", "shake", "torque", "jog",
                        "calibrate_zero", "calibrate_range", "set_direction"]},
    "axis": {"type": "string", "enum": ["yaw", "pitch", "both"], "default": "both"},
    "yaw_deg": {"type": "number", "description": "Target yaw angle, or offset when relative."},
    "pitch_deg": {"type": "number", "description": "Target pitch angle, or offset when relative."},
    "relative": {"type": "boolean", "default": false},
    "trim": {"type": "boolean", "default": true,
             "description": "move/center/jog: one correction pass if an axis settles short. Set false for the quickest possible move."},
    "mid_travel": {"type": "boolean", "default": false, "description": "calibrate_zero only."},
    "duration_ms": {"type": "integer", "default": 600, "minimum": 0, "maximum": 3000},
    "times": {"type": "integer", "default": 2, "minimum": 1, "maximum": 10},
    "amplitude_deg": {"type": "integer", "default": 12, "minimum": 1, "maximum": 60},
    "enabled": {"type": "boolean", "default": true, "description": "torque only."},
    "counts": {"type": "integer", "minimum": -40, "maximum": 40, "description": "jog only."},
    "seconds": {"type": "integer", "default": 15, "minimum": 1, "maximum": 120,
                "description": "calibrate_range sweep window."},
    "direction": {"type": "integer", "enum": [-1, 1], "default": 1,
                  "description": "set_direction only."}
  }
}
```

## Tool Call Inputs

Where is the head now:

```json
{"path":"{CUR_SKILL_DIR}/scripts/head.lua","args":{"action":"status"}}
```

Look up 30 degrees, keeping yaw where it is:

```json
{"path":"{CUR_SKILL_DIR}/scripts/head.lua","args":{"action":"move","pitch_deg":30}}
```

Look at someone to the left while levelling the head, over one second:

```json
{"path":"{CUR_SKILL_DIR}/scripts/head.lua","args":{"action":"move","yaw_deg":-25,"pitch_deg":0,"duration_ms":1000}}
```

Nod yes twice:

```json
{"path":"{CUR_SKILL_DIR}/scripts/head.lua","args":{"action":"nod","times":2,"amplitude_deg":10}}
```

Release the servos so the user can pose the head by hand:

```json
{"path":"{CUR_SKILL_DIR}/scripts/head.lua","args":{"action":"torque","enabled":false}}
```
