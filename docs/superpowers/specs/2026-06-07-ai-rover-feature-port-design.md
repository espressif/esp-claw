# Port 4 ai-rover features into wave_rover

**Date:** 2026-06-07
**Status:** approved (user: "Думаю, стоит всё перенести. Используй superpowers, спланируй портирование и приступай")

## Background

`~/repos/ai-rover` (M5StickC Plus + RoverC Pro, Mecanum) has four things worth
porting into `wave_rover` (ESP32 tank-drive rover, IDF + MCP):

1. Wi-Fi STA→AP fallback with a background reconnect task
2. mDNS hostname advertisement
3. Structured JSON event logging
4. An FSM with named states, surfaced as a color-coded status pill in the web UI

wave_rover already has the dark-theme web UI, joystick, Wi-Fi settings page,
and a UDP syslog forwarder (`wr_syslog.c`) from prior sessions — those are not
touched here.

## Ordering (risk-driven, not feature-priority-driven)

The Wi-Fi rewrite is the only change that can sever the rover's only remote
access channel (it's flashed via OTA, `POST /update`; no routine serial
access). So build order is reversed from feature-priority order:

1. **mDNS** — additive, can't break connectivity
2. **FSM + status pill** — additive, UI/state only
3. **Structured logging helper** — additive, rides on existing `wr_syslog`
4. **Wi-Fi STA→AP fallback** — last, flashed only with serial access confirmed

Each step is flashed and verified over OTA before starting the next, so a
regression in step N doesn't strand the device before step N+1 is even
written.

## Feature 1: mDNS hostname

- Add `mdns` to `PRIV_REQUIRES` in `main/CMakeLists.txt`.
- In `app_main.c`, after `wr_wifi_init()`, call `mdns_init()` /
  `mdns_hostname_set(cfg->hostname)` / `mdns_instance_name_set(...)`, and
  register an `_http._tcp` service on `cfg->mcp_port` so `<hostname>.local`
  resolves on the LAN (config already has a `hostname` field, defaulting to
  `"wave-rover"` — no new config needed).
- Skip mDNS setup entirely in pure-AP mode (no upstream LAN to advertise to).

## Feature 2: FSM + color-coded web status pill

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

## Feature 3: Structured JSON event logging (scoped, not a rewrite)

ai-rover's `rover_log()` wraps *every* log call in a JSON envelope. That's a
pervasive rewrite this codebase's "don't refactor beyond the task" guidance
argues against, and it would also multiply the chance of accidentally logging
a secret across dozens of call sites.

Scoped version: one helper, e.g. `wr_log_event(const char *event, const char
*k1, const char *v1, ...)` (or a small fixed-shape struct, following
ai-rover's field-array idea) that emits a single-line JSON object via
`ESP_LOGI` — which `wr_syslog` already forwards over UDP. Used only at a
handful of *significant* transition points that are currently just prose logs:

- Wi-Fi state changes (connected / disconnected / AP-fallback entered/exited)
- E-stop trigger / clear
- FSM state transitions (feature 2)
- Nav command start / complete / abort

Existing routine `ESP_LOGI/D` calls are untouched. The helper must never be
passed `wifi_password`/`auth_token` — only identifiers, states, and numeric
values, consistent with the existing "never log password fields" rule in
`wave_rover_config.c`.

## Feature 4: Wi-Fi STA→AP fallback + background reconnect (last, careful)

Current `wr_wifi_init()` (confirmed by reading `on_event()`): on
`WIFI_EVENT_STA_DISCONNECTED` it just clears `s_connected`/`s_ip` — there is
**no** existing retry/reconnect logic, so a new reconnect task will not race
with anything.

Plan:
- On initial STA connect timeout, instead of "continuing offline": switch to
  `WIFI_MODE_APSTA` (config already supports `wifi_mode == 2` AP+STA path) and
  bring up the configured fallback AP (`wifi_ap_ssid`/`wifi_ap_password`) so
  there is always a LAN-side lifeline — this directly avoids the failure mode
  where forcing a test of the fallback strands the device.
- Add `wr_wifi_reconnect_task()`: every ~15 s while not connected in STA mode,
  call `esp_wifi_connect()`; on success, log + transition state, and (if the
  fallback AP was brought up) tear it down and drop back to STA-only — mirrors
  ai-rover's `wifi_reconnect_task()` teardown-on-success pattern.
- Expose `wr_wifi_is_ap_fallback(void)` for the status JSON / FSM (an
  `AP_FALLBACK` state, feature 2) and the OLED line.
- **Flash protocol for this step only**: confirm serial/USB access to the
  board *before* flashing this change, and keep `wifi_mode == 2` as the
  effective runtime mode during bring-up testing so the AP is always live
  regardless of STA outcome — never test "force STA to fail" against a board
  that only has the new code's untested fallback as its recovery path.

## Build / verification

- `idf.py build` for the wave_rover board config after each feature.
- Flash via the existing OTA path (`POST /update`) for features 1–3; flash
  feature 4 only with physical serial access available.
- After each flash: verify over the MCP web UI / `/status` endpoint and
  (where applicable) `wr_syslog` UDP stream — no new ESP_LOG secret leakage,
  state pill renders and changes color, `<hostname>.local` resolves, and (for
  feature 4) the rover recovers from a forced STA outage without a manual
  power cycle.

## Out of scope

- Deep sleep / RTC GPIO wake (ai-rover is M5StickC-battery-specific; wave_rover
  has a UPS — not applicable)
- Mecanum/gripper/AI-chat states from ai-rover's FSM — different hardware,
  not present on wave_rover
- Rewriting all `ESP_LOG*` call sites to structured JSON
