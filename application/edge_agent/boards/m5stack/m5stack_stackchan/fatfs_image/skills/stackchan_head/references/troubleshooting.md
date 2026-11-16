# StackChan Head Troubleshooting

Report script errors to the user as they are. Do not retry a failed move with a different angle.

## No Servo Answered

The servo rail or the bus, not the angle. `VM_EN` on IOE1 pin 0 feeds the servos:

- `/system/skills/builtin_lua_modules/scripts/builtin/test/py32_ioe_basic.lua` shows and can cycle `VM_EN`
- `/system/scripts/stackchan_5v_check.lua` checks the 5 V rail
- Make sure no other script is holding UART 1 — it has a single owner

## One Servo Silent, The Other Answering

Prints a `WARNING` and keeps going: the rail and the bus are up, so it is that servo's own cable, its id, or an overload shutdown.

Actions that do not need the silent axis still work. With yaw silent you can still `nod`, `move` pitch, and `center`. Actions that need it fail with `cannot run` — say which axis is out and stop, do not retry.

## Retries In The Log

Every bus operation is tried twice, because this bus drops the occasional frame. `needed 2 attempts` or `answered on ping attempt 2` is normal in ones and twos. A burst of them, or alternating whole runs that work and runs where nothing answers, means the servo wiring is marginal: say so, and point at `/system/scripts/stackchan_bus_soak.lua`, which separates a flaky cable from a problem opening the port.

## An Axis Stops Short Of Its Target

**Yaw stops about 4° short of any goal and holds there.** Its joint carries the servo cable and the Touch board FPC, and that drag beats the servo's position loop before it arrives. The dead band is only 1 count, so this is drag, not servo tolerance. `move`, `center` and `jog` therefore run one correction pass by default (`trim`), which lands the axis exactly. Pass `trim: false` when a move should be as quick as possible; gestures never trim.

If an axis is still short **after** trimming, it is stalling against something real. Release torque with `torque` / `enabled: false` and tell the user.

## Yaw's Travel Is Not Mechanical

Yaw turns further than the encoder can address, so `calibrate_range` only ever finds the encoder ends there and refuses to set its travel from a sweep. Its real limit is cable and FPC twist through the joint, which nothing on the device can measure. The default is ±90° from zero, deliberately tighter than the ±128° the M5Stack BSP allows.

## Heat And Standing Load

Holding pitch near a travel end fights gravity continuously: one session read 37 °C there against 26 °C at rest. Idle yaw load reads about 180 even near zero, and more when turned away. For a long "keep looking that way" pose, release torque rather than holding it.
