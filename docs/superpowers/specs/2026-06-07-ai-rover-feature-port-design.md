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

- New small header/source (e.g. `wr_state.h/.c` in `main/`) defining
  `wr_state_t` (`IDLE, DRIVING, NAV_BUSY, AP_FALLBACK, ESTOP`) and
  `wr_state_set()/wr_state_get()/wr_state_name()`, mirroring ai-rover's
  `transition_to()`/`state_name()` but trimmed to states that actually occur
  on wave_rover (no AI/chat states — that's not part of this firmware).
- `wave_rover_mcp_web.c`: extend the `/status` JSON with a `"state"` field;
  extend the JS `stColors`-style map so the pill's background color reflects
  state (mirrors ai-rover's `state_color()` → hex map, e.g. green=IDLE,
  blue=DRIVING, red=ESTOP/AP_FALLBACK, amber=NAV_BUSY).
- OLED (`wave_rover_display.c`) is monochrome SSD1306 — color is not
  physically possible there. `wr_display_status()` keeps showing text; we
  only add the state name as a short label (e.g. replacing/augmenting the
  `MCP:ON ESTOP` line with the FSM state name) so the two surfaces stay
  consistent without inventing a color scheme that can't render.
- State transitions are wired at the existing call sites that already know
  about drive/estop/nav events (motor/nav/estop handlers in `wave_rover_hal`
  and `wave_rover_mcp_tools.c`), not via new polling.

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
