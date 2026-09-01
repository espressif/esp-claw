# StackChan Head Calibration

Zero points and travel limits are mechanical properties of one assembled unit, so they cannot be constants. They live in `<DATA>/config/stackchan_head.json` and are written by the calibration actions.

Run this once per unit, and again after re-assembly or a servo swap. Every step needs the user at the device, so tell them what to do and wait. **Ask before starting: these actions overwrite stored per-unit data.**

1. `status` — note whether `calibrated` is already true. If it is, do not recalibrate without being asked.
2. `torque` with `enabled: false` — the head becomes free to move by hand.
3. `calibrate_range` with `axis: "pitch"` — ask the user to tilt the head slowly to its upper stop and its lower stop during the window, pausing at each end. It releases torque itself.
4. `calibrate_zero` with `axis: "both"` and `mid_travel: true` — takes the middle of the measured travel as 0°. This needs no hands and always gives a two-sided range.

   To make some other pose the neutral instead, have the user hold the head there and run `calibrate_zero` without `mid_travel`. Hold pitch **mid-travel, not resting on a stop**: rest is wherever gravity left it, and zeroing there puts every pitch angle on one side of 0°. The script says so when it happens.
5. `center`, then `move` a few small angles to confirm the result matches what the user sees.

Yaw needs no `calibrate_range`: a sweep can only find the encoder ends there, and the script refuses to set its travel from one. See `references/troubleshooting.md`.

## Why A Resting Position Is Not A Zero

The same unit read pitch 683 in one session and 582 in another, because rest is wherever gravity and friction left the head. Only step 4 establishes zero.

## Repeatability

Three hand sweeps of the same unit measured 578..864, 579..861 and 585..860 counts — spans of 85.9° to 89.4°, and stored zeros two counts apart. That scatter is inherent to hand calibration. Do not re-run calibration chasing ±1–2°.

## Direction

**Positive pitch is up, positive yaw is left**, from the robot's point of view, confirmed on hardware. `set_direction` records `-1` for a unit assembled the other way round; you should not need it. It only rewrites the file, so it works even when a servo is not answering.

## Output Meaning

- `... deg` — angle from the stored zero for that axis.
- `counts N (min..max)` — raw encoder position and the travel currently allowed. One count is 0.3125°.
- `zero N`, `dir +1/-1` — the calibration in force.
- `dead band N counts` — how close to a goal that servo will bother to get. `load` — what it is fighting to hold the current pose.
- `calibration source:` — the file the values came from, or which fallback was used and why.
- `SILENT` — that axis did not answer; the other one still works.
- `clamped` — the requested angle was outside the allowed range and was reduced.
- `trimming <axis> by N counts` — the correction pass, normal on yaw.
- `needed 2 attempts` — one bus frame was lost and the retry succeeded.
