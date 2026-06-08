# Wave Rover Prometheus Metrics — Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Expose wave_rover's existing telemetry (power, motors, IMU, system health) as a Prometheus-format `/metrics` HTTP endpoint, and advertise its path via mDNS for discovery — mirroring the service-discovery convention already used in `~/repos/ai-rover`.

**Architecture:** A new `wave_rover_mcp_metrics` source pair registers a `GET /metrics` handler on the existing httpd server. It reads from HAL getters that are already polled (`wr_power_get_status`, `wr_motor_get_state`, `wr_rover_state_get`, `wr_imu_get_sample`) plus ESP-IDF system APIs, formats Prometheus text-exposition output into a heap buffer, and sends it in one response. mDNS TXT records on the existing `_http._tcp` service advertise `/metrics` (and other API paths) for discovery.

**Tech Stack:** ESP-IDF `esp_http_server`, Prometheus text exposition format 0.0.4, ESP-IDF `mdns` component.

---

## Context

`~/repos/ai-rover` derives its "metrics" from structured JSON log lines forwarded to Loki (a `heartbeat` event with typed fields, queried via LogQL `unwrap`). It also registers a single `_http._tcp` mDNS service with TXT records pointing at its API paths (`api_status`, `api_cmd`, etc. — see `start_mdns()` in `ai-rover/src/main_idf.cpp`).

wave_rover already has a working syslog→Loki pipeline (verified 2026-06-08), but its forwarder wraps generic ESP_LOG text lines into `{"ts","level","tag","msg"}` — not domain-structured numeric fields, so Loki `unwrap` queries aren't practical on it. Per explicit direction, this design instead adds a **Prometheus `/metrics` endpoint** — more idiomatic for numeric time-series metrics and Grafana/Prometheus alerting — rather than following ai-rover's log-derived approach.

wave_rover's mDNS registration (`app_main.c`) currently calls `mdns_service_add(NULL, "_http", "_tcp", s_cfg.mcp_port, NULL, 0)` with **no TXT records**, unlike ai-rover's TXT-record convention.

## Components

### 1. `wave_rover_mcp_metrics.h` / `.c` (new files in `components/wave_rover_mcp/`)

Public API:

```c
/* Registers GET /metrics on the given httpd server. Mirrors
 * wr_mcp_web_register's signature/ownership model — caller owns the
 * server handle and starts/stops it. */
esp_err_t wr_mcp_metrics_register(httpd_handle_t server);
```

Internally: one static `handle_metrics()` HTTP GET handler. Why a separate file rather than adding to `wave_rover_mcp_web.c` (already 829 lines, focused on web UI/settings/OTA): Prometheus text formatting is a distinct concern — numeric formatting and exposition-format conventions — that can be understood, tested, and changed independently of the web UI. This follows the project's "split files by responsibility" guidance.

Registration: `wave_rover_mcp.c` calls `wr_mcp_metrics_register(s_httpd)` alongside the existing `wr_mcp_web_register(s_httpd, cfg)` call (around line 416). This brings the registered URI handler count from 9 to 10 — well under the configured `hcfg.max_uri_handlers = 16`.

### 2. Metric catalog

All metrics are namespaced `wave_rover_*`, typed `gauge`, with units in the metric name and standard `# HELP` / `# TYPE` comment lines, per Prometheus naming conventions.

**Power/battery** — source: `wr_power_get_status(&ps)` → `wr_power_status_t`:

| Metric | Source field | Notes |
|---|---|---|
| `wave_rover_battery_voltage_volts` | `load_voltage_v` | |
| `wave_rover_power_bus_voltage_volts` | `bus_voltage_v` | |
| `wave_rover_power_shunt_voltage_millivolts` | `shunt_voltage_mv` | |
| `wave_rover_power_current_milliamps` | `current_ma` | |
| `wave_rover_power_draw_milliwatts` | `power_mw` | |
| `wave_rover_power_charging` | `charging` | 0/1 |
| `wave_rover_power_low_battery` | `low_battery` | 0/1 |
| `wave_rover_power_sensor_present` | `present` | 0/1 — INA219 may be physically absent; firmware already degrades gracefully for this case (see `wr_hal_init`) |

**Motors & rover state** — source: `wr_motor_get_state(&ms)` → `wr_motor_state_t`, `wr_rover_state_get()`:

| Metric | Source | Notes |
|---|---|---|
| `wave_rover_motor_speed_ratio{side="left"\|"right"}` | `ms.left` / `ms.right` | gauge, range -1.0..1.0 |
| `wave_rover_emergency_stop` | `ms.emergency_stop` | 0/1 |
| `wave_rover_state{state="idle"\|"driving"\|"nav_busy"\|"estop"}` | `wr_rover_state_get()` + `wr_rover_state_name()` | Prometheus "enum" pattern: emit one line per possible state value — `1` for the active state, `0` for the rest, so `sum by (state) (wave_rover_state)` works in PromQL |

**IMU/orientation** — source: `wr_imu_get_sample(&s)` → `wr_imu_sample_t`. Each group is gated on its presence flag — if the sensor (or sub-sensor) is absent, its lines are omitted entirely rather than emitted as zero (avoids misleading "0 g acceleration" readings):

| Metric | Source | Gate |
|---|---|---|
| `wave_rover_imu_present` | `s.present` | always emitted (0/1) |
| `wave_rover_imu_accel_g{axis="x"\|"y"\|"z"}` | `s.accel_x/y/z` | `s.present` |
| `wave_rover_imu_gyro_dps{axis="x"\|"y"\|"z"}` | `s.gyro_x/y/z` | `s.present` |
| `wave_rover_imu_mag_microtesla{axis="x"\|"y"\|"z"}` | `s.mag_x/y/z` | `s.present && s.mag_present` |
| `wave_rover_imu_temperature_celsius` | `s.temperature_c` | `s.present && s.has_temperature` |

**System health** — source: ESP-IDF system APIs + a new `wr_wifi_get_rssi()` getter:

| Metric | Source | Notes |
|---|---|---|
| `wave_rover_uptime_seconds` | `esp_timer_get_time() / 1000000` | seconds since boot |
| `wave_rover_free_heap_bytes` | `esp_get_free_heap_size()` | |
| `wave_rover_min_free_heap_bytes` | `esp_get_minimum_free_heap_size()` | low-water mark; catches slow leaks over uptime |
| `wave_rover_wifi_connected` | `wr_wifi_is_connected()` | 0/1 |
| `wave_rover_wifi_rssi_dbm` | `wr_wifi_get_rssi()` (new) | only emitted when connected |
| `wave_rover_build_info{version="0.1.0"}` | constant | standard Prometheus version-as-label pattern; value always `1` |

### 3. New HAL getter: `wr_wifi_get_rssi`

`wr_wifi.h`/`wr_wifi.c` currently expose `wr_wifi_is_connected()` and `wr_wifi_get_ip()` but no signal-strength accessor. Add:

```c
/* Returns the current STA AP's RSSI in dBm via esp_wifi_sta_get_ap_info().
 * Returns ESP_ERR_WIFI_NOT_CONNECT (or the underlying error) when not
 * connected to an AP — caller should skip emitting the metric in that case. */
esp_err_t wr_wifi_get_rssi(int8_t *out_rssi_dbm);
```

Implementation wraps `esp_wifi_sta_get_ap_info(&ap_info)` and copies `ap_info.rssi`.

### 4. Response generation

The handler allocates an 8192-byte heap buffer — the catalog above is ~21 distinct metric names (each contributing a `# HELP` and `# TYPE` comment line) plus up to ~30 value lines (some expand into multiple lines via labels, e.g. `motor_speed_ratio{side=...}` ×2, `wave_rover_state{state=...}` ×4, IMU axes ×3 each), roughly 75-90 lines total at up to ~90 bytes/line worst case (long metric names with multiple labels) — 8192 bytes leaves comfortable headroom. The handler builds the exposition text into this buffer via bounded `snprintf`-with-offset calls (truncating safely on overflow, matching the `json_escape` truncation pattern already used in `wr_syslog.c`), sends it once via `httpd_resp_send` with content type `text/plain; version=0.0.4`, then frees the buffer. This mirrors the existing `malloc`/`free`-per-request pattern in `handle_settings_post`/`handle_update`, and avoids large stack locals per the project's memory rules (no >128-byte locals on task stacks).

### 5. mDNS TXT records

`app_main.c` currently registers mDNS with no TXT items:

```c
mdns_service_add(NULL, "_http", "_tcp", s_cfg.mcp_port, NULL, 0);
```

Change to mirror ai-rover's discovery convention, adding `api_metrics` alongside the other API paths:

```c
mdns_txt_item_t txt[] = {
    { "path",         "/" },
    { "api_status",   "/status" },
    { "api_metrics",  "/metrics" },
    { "api_settings", "/settings" },
};
mdns_service_add(NULL, "_http", "_tcp", s_cfg.mcp_port, txt,
                 sizeof(txt) / sizeof(txt[0]));
```

This makes `/metrics` discoverable via `wave-rover.local` mDNS browsing without hardcoding the path anywhere outside the firmware (e.g. for a future mDNS→Prometheus `file_sd` bridge).

### 6. Auth

`/metrics` goes through the same `WEB_REQUIRE_AUTH(req)` macro as every other endpoint (`/status`, `/settings`, `/cmd`, ...) — no special-casing. If `auth_enabled` is off (today's default), `/metrics` is open like the rest of the local API; if it's on, a Prometheus scrape config needs the same credentials (HTTP basic auth, which `web_check_auth` already supports). This keeps the security model uniform instead of carving out a metrics-specific exception.

## Error Handling

- INA219 absent → `wr_power_get_status` returns `present = false`; the handler still emits `wave_rover_power_sensor_present 0` and omits the rest of the power block (consistent with the IMU gating approach — no misleading zero readings).
- IMU absent / sub-sensors absent → gated per the table above.
- Wi-Fi not connected → `wave_rover_wifi_connected 0`, `wave_rover_wifi_rssi_dbm` omitted.
- Buffer overflow during text generation → bounded `snprintf` truncates safely; never overruns or crashes (matches `json_escape` precedent).
- `malloc` failure → handler returns `500 Internal Server Error` with a JSON error body, matching `handle_settings_post`'s OOM handling.

## Testing

- This app builds via PlatformIO (`platformio.ini`), not `idf.py` — per project convention, run `pio run` from `application/wave_rover/` for the affected board config (see [[feedback_build_flash]] memory).
- Manual verification: `curl http://wave-rover.local/metrics` (or the rover's IP) and confirm valid Prometheus exposition-format output — correct `# HELP`/`# TYPE` lines, parseable metric lines, sane values against `/status` for cross-check (e.g. `bat_v` vs `wave_rover_battery_voltage_volts`).
- If `promtool` is available locally, `curl -s .../metrics | promtool check metrics` validates exposition-format correctness.
- Confirm mDNS TXT records via `dns-sd -B _http._tcp` / `avahi-browse -r _http._tcp` (or equivalent) showing `api_metrics=/metrics`.

### Example Prometheus scrape config (reference only — lives on the Prometheus server, not in this repo)

```yaml
scrape_configs:
  - job_name: wave-rover
    static_configs:
      - targets: ["wave-rover.local:80"]
    metrics_path: /metrics
```
