# Port ai-rover features into wave_rover

**Date:** 2026-06-07
**Status:** approved, scope revised after user feedback (see Revision note)

## Revision note (2026-06-07, post-review)

Original plan proposed 4 features. After presenting it, the user (via
Telegram) said:

> "Давай sta-ap пока не будем трогать совсем, а логи хотелось бы все видеть
> в json, чтобы в Loki их красиво укладывать"
> ("Let's not touch STA-AP at all for now, and I'd like to see ALL logs in
> JSON, so they fit nicely into Loki.")

This **drops feature 4 (Wi-Fi STA→AP fallback) entirely** — out of scope, not
just reordered — and **expands feature 3 from "scoped key-event logging" to
"every log line in JSON"**. The expansion turns out to have a much cleaner
implementation than either ai-rover's pervasive `rover_log()` rewrite or this
doc's original scoped-helper idea: see revised Feature 3 below.

## Background

`~/repos/ai-rover` (M5StickC Plus + RoverC Pro, Mecanum) had three things
worth porting into `wave_rover` (ESP32 tank-drive rover, IDF + MCP), after the
above revision:

1. mDNS hostname advertisement
2. An FSM with named states, surfaced as a color-coded status pill in the web UI
3. All log output in structured JSON (for Loki ingestion over the existing
   UDP syslog forwarder)

wave_rover already has the dark-theme web UI, joystick, Wi-Fi settings page,
and a UDP syslog forwarder (`wr_syslog.c`) from prior sessions — those are not
touched here. Wi-Fi STA→AP fallback is explicitly out of scope per the user's
"не будем трогать совсем" (let's not touch it at all).

## Ordering

No risk-ordering constraint remains once Wi-Fi is out of scope — all three
remaining features are additive (new component dep, new state module, log
formatting inside an existing forwarder) and none can sever connectivity.
Build in dependency order: mDNS first (simplest, validates the OTA-flash loop
for this round of changes), then FSM/status (feeds the `state` field into both
`/status` JSON and `wr_display_status`), then JSON logging (independent of the
other two, can be done in any order — placed last because it touches the most
performance-sensitive path, the log hook that fires on every log line).

Each step is flashed and verified over OTA before starting the next.

## Feature: mDNS hostname

- Add `mdns` to `PRIV_REQUIRES` in `main/CMakeLists.txt`.
- In `app_main.c`, after `wr_wifi_init()`, call `mdns_init()` /
  `mdns_hostname_set(cfg->hostname)` / `mdns_instance_name_set(...)`, and
  register an `_http._tcp` service on `cfg->mcp_port` so `<hostname>.local`
  resolves on the LAN (config already has a `hostname` field, defaulting to
  `"wave-rover"` — no new config needed).
- Skip mDNS setup entirely in pure-AP mode (no upstream LAN to advertise to).

## Feature: FSM + color-coded web status pill

ai-rover's FSM tracks AI/chat states (`AI_THINKING`, `AI_EXECUTING`,
`WEB_CONTROL`) that have no equivalent here — wave_rover runs no local agent
loop. The states that actually occur on wave_rover are derivable almost
entirely from values the code already polls every status refresh:

- `WR_STATE_ESTOP`    — `wr_motor_emergency_stop_active()` is true (red)
- `WR_STATE_NAV_BUSY` — a blocking nav command (`rover.drive_cm`/
  `rover.rotate_deg`) is in progress (amber)
- `WR_STATE_DRIVING`  — motors are moving: `wr_motor_get_state().left/right != 0`
  (blue)
- `WR_STATE_IDLE`     — none of the above (green)

This is a **derived** status, not a transition-tracked FSM with history —
simpler, and the only piece that can't be derived from already-polled values
is "nav command in progress" (the blocking `wr_nav_drive_cm`/`wr_nav_rotate_deg`
calls don't expose a busy flag). That needs exactly one new piece of shared
state, set/cleared around those two call sites in `wave_rover_mcp_tools.c`.

Implementation, confined to the `wave_rover_mcp` component (where both the
setter — tool handlers — and the only consumer — `/status` — already live, so
no new cross-component dependency is introduced):

- New `wave_rover_mcp_state.h/.c` (private, added to the component's `SRCS`,
  not under `include/`): `wr_rover_state_t` enum, `wr_rover_state_get(void)`
  (derives from motor state + estop + the nav-busy flag),
  `wr_rover_state_set_nav_busy(bool)`, `wr_rover_state_name(wr_rover_state_t)`.
- `wave_rover_mcp_tools.c`: bracket the (synchronous, blocking)
  `wr_nav_drive_cm`/`wr_nav_rotate_deg` call expressions in `tool_drive_cm`
  (`:561`), `tool_rotate_deg` (`:521`), and `tool_nav_to` (`:606`, `:610`,
  wrapping both calls in one busy span) with `wr_rover_state_set_nav_busy(true)`
  before and `..._set_nav_busy(false)` after. Since these calls are
  synchronous and always return before the wrapper resumes, a single
  set/clear pair around each call site is correct regardless of how many
  internal early-return paths the nav functions have — no restructuring of
  `wave_rover_nav.c` needed.
- `wave_rover_mcp_web.c`: extend `handle_status()`'s JSON with a `"state"`
  string field (`handle_status` at `wave_rover_mcp_web.c:387`); extend the
  page's JS with a `stColors`-style `{idle:'#2d8b2d', driving:'#2563eb',
  nav_busy:'#d97706', estop:'#dc2626'}` map (mirrors ai-rover's
  `state_color()` hex values) and use it in `rf()` (currently
  `wave_rover_mcp_web.c:273-278`) to set both `ePill`'s text and background.

OLED (`wave_rover_display.c`) is monochrome SSD1306 — color coding is not
physically possible there, and it already shows the most safety-critical bit
(`ESTOP` in the MCP line). Adding FSM-state text would require either a new
polling task or pushing transitions across a `wave_rover_mcp` →
`wave_rover_hal` dependency that doesn't currently exist, for marginal benefit
over what's already shown. **Left untouched** — out of scope for this pass.

## Feature: all logs as structured JSON (for Loki)

ai-rover's `rover_log()` wraps every *call site* in a JSON envelope — a
pervasive rewrite touching dozens of files. The user wants the *output* in
JSON (so Loki can parse fields/labels cleanly), not a particular call-site
API, and `wr_syslog.c` already gives us a single chokepoint where every log
line in the firmware passes through exactly once:

- `wr_syslog_init()` installs `syslog_vprintf()` via `esp_log_set_vprintf()`
  — this hook receives the fully-rendered line for *every* `ESP_LOGx` call in
  the firmware (confirmed: `CONFIG_LOG_COLORS` is **not** set in
  `sdkconfig.wave_rover`, so lines are plain text, no ANSI codes:
  `"%c (%lu) %s: %s\n"` → e.g. `I (12345) wr_wifi: STA connected, IP=...`).
- `syslog_task()` currently wraps the stripped line in an RFC 3164 envelope
  (`"<%d>wave-rover: %s"`) and sends it over UDP.

**Plan: parse the rendered line into level/timestamp/tag/message once, in
`syslog_task()`, and replace the RFC 3164 plain-text body with a JSON object**
— keeping the `<PRI>` envelope (Promtail/syslog receivers expect it; the JSON
becomes the message body, which a Loki pipeline `json` stage then parses into
labels):

```
<PRI>wave-rover: {"ts":12345,"level":"info","tag":"wr_wifi","msg":"STA connected, IP=192.168.1.5"}
```

Parsing is a single small function: the line always starts with `"%c (%lu) %s: "`
(level char, space, `(`, decimal ms timestamp, `)`, space, tag, `: `, message)
because that's ESP-IDF's fixed `LOG_FORMAT` with colors disabled. Map the level
char (`E/W/I/D/V`) to a lowercase word (`error/warn/info/debug/verbose`) for
clean Loki label values. JSON-escape `tag` and `msg` (quotes, backslashes,
control characters) — both can in principle contain arbitrary bytes.

This is strictly additive to one already-private file:
- **Zero changes to any of the ~hundreds of `ESP_LOGx` call sites** — no
  refactor risk, no new chance of a call site leaking a secret.
- **UART/serial output stays human-readable** — only the UDP-forwarded copy
  changes shape (the existing `vprintf(fmt, args)` call for UART is untouched).
- Reuses the existing queue/socket/broadcast machinery as-is; only the framing
  in `syslog_task()` changes.

No new secret-logging surface is introduced: the formatter only restructures
bytes that `wr_syslog` already forwards verbatim today (and that already pass
the project's "never log password fields" discipline at the `ESP_LOGx` call
sites themselves).

## Build / verification

- `idf.py build` for the wave_rover board config after each feature.
- Flash via the existing OTA path (`POST /update`).
- After each flash: verify over the MCP web UI / `/status` endpoint —
  `<hostname>.local` resolves, state pill renders and changes color with FSM
  transitions, and the `wr_syslog` UDP stream carries well-formed JSON lines
  (spot-check with `nc -ul 5514` or similar) with no secret fields and no
  parse/escaping artifacts.

## Out of scope

- Wi-Fi STA→AP fallback — explicitly declined by the user for now
  ("давай sta-ap пока не будем трогать совсем")
- Deep sleep / RTC GPIO wake (ai-rover is M5StickC-battery-specific; wave_rover
  has a UPS — not applicable)
- Mecanum/gripper/AI-chat states from ai-rover's FSM — different hardware,
  not present on wave_rover
- Rewriting `ESP_LOGx` call sites — the JSON conversion happens centrally in
  `wr_syslog.c`, not at call sites
