# NMMiner LAN Discovery & Control

Quickly discover NMMiner / NMAxe / NMAxeGamma / NMQAxe++ Bitcoin solo miners on
the local network, read their live status, and control them. All work happens in
the on-board Lua scripts so the LLM only needs to choose a script and pass JSON
arguments — **no per-IP planning, no JSON parsing, no loop logic** is required
from the LLM.

## When to use

When the user asks anything about NMMiner / NMAxe / NMAxeGamma / NMQAxe++ /
"矿机" / "BitAxe" devices:

- "扫一下网内的矿机 / scan miners / list miners"
- "看看 192.168.x.y 这台机的算力 / status / hashrate / temperature"
- "重启那台矿机 / restart / 找一下哪台 / blink"
- "把频率调到 500 / vcore 1250"

## Discovery procedure (do not deviate)

NMMiner devices run an HTTP API on port 80. To scan accurately and fast:

1. **Just call `nmminer_scan` with no args** — `run_lua_script
   script="nmminer_scan" args={}`. The script will:
   - read this device's own STA IP (`system.ip()`),
   - derive the `/24` subnet (e.g. `192.168.124.0/24`),
   - fast-sweep `/alive` on every host with a 250 ms timeout; the **first**
     responder returns the full NMMiner IP list and the sweep stops,
   - fall back to a direct `/probe` sweep if no `/alive` responds,
   - then `/probe` each candidate, filter NM-compatible, and print a
     compact summary table.
2. (Optional) If the user already knows a NMMiner IP and wants the fastest
   possible scan, pass it as a seed:
   `args={"seed_ips":["192.168.x.y"]}`. This skips the subnet sweep
   (~1–2 s instead of ~5–60 s).
3. Read back the printed table and present it to the user in friendly form.
   Do not re-issue `/probe` requests yourself.

Optional args for `nmminer_scan`:

| Arg | Type | Default | Meaning |
|---|---|---|---|
| `seed_ips` | string[] | *auto* | Skip subnet sweep; treat each entry as a known NMMiner |
| `probe_timeout_ms` | int | `1500` | Per-IP `/probe` timeout (final classification step) |
| `alive_timeout_ms` | int | `2500` | Per-seed `/alive` timeout (seeded mode only) |
| `subnet_alive_timeout_ms` | int | `250` | Per-host `/alive` timeout during auto sweep |
| `subnet_probe_timeout_ms` | int | `250` | Per-host `/probe` timeout for fallback sweep |
| `subnet_host_start` | int | `1` | First host octet to sweep (.X) |
| `subnet_host_end` | int | `254` | Last host octet to sweep (.X) |
| `model_filter` | string | `""` | If set (e.g. `"NMQAxe++"`) only keep matching `model` |
| `max_targets` | int | `64` | Hard cap on probed IPs |

## Status / control / setting

Once you have an IP, use one of these scripts. Each takes `{"ip":"<addr>", ...}`
and prints either a one-line summary or a small JSON snippet.

### Read live status

`run_lua_script script="nmminer_info" args={"ip":"<addr>"}`

Optional `args.section` selects a sub-endpoint:

| Section | Endpoint | Use when |
|---|---|---|
| `"system"` (default) | `/api/system/info` | Hashrate, temps, power, pool, fans |
| `"realtime"` | `/api/dashboard/chart/realtime` | Most-recent telemetry sample |
| `"probe"` | `/probe` | Just identification |

### Control actions

`run_lua_script script="nmminer_control" args={"ip":"<addr>", "action":"<name>"}`

| `action` | Endpoint | Effect |
|---|---|---|
| `"restart"` | `POST /api/system/restart` | Reboot the device (~10–15 s offline) |
| `"clearhits"` | `POST /api/system/clearhits` | Reset block-hit counter |
| `"find"` | `POST /api/swarm/find` | Make the device blink (locate it physically) |
| `"wakeup"` | `GET  /api/wakeup` | Wake screen from screensaver |
| `"rescan"` | `POST /api/swarm/scan` | Trigger device-side ICMP rescan |

### Change mining settings

`run_lua_script script="nmminer_setting" args={"ip":"<addr>", "freq":500, "vcore":1200}`

At least one of `freq` (MHz, 400–600) or `vcore` (mV, 1100–1400) must be set.
The script PATCHes `/api/setting/mining` with `asicFreqReq` / `asicVcoreReq`.

> Frequency change requires a reboot to take effect. Vcore applies immediately.
> The script will print a one-line confirmation; chain `nmminer_control
> action="restart"` if the user wanted the freq change to take effect now.

## Examples

**User**: "扫描一下家里的矿机"
1. `run_lua_script script="nmminer_scan" args={}`
2. Forward the printed table; do not re-probe.

**User**: "扫一下，知道 192.168.124.167 是一台矿机"
1. `run_lua_script script="nmminer_scan" args={"seed_ips":["192.168.124.167"]}`
2. Same as above but faster.

**User**: "192.168.124.42 现在算力多少？"
1. `run_lua_script script="nmminer_info" args={"ip":"192.168.124.42"}`
2. Read `hashRate` / `asic.temp` / `power` from the printed JSON.

**User**: "找一下 .42 是哪一台 / 闪一下灯"
1. `run_lua_script script="nmminer_control" args={"ip":"192.168.124.42","action":"find"}`

**User**: "把 .42 频率调到 500MHz 立即生效"
1. `run_lua_script script="nmminer_setting" args={"ip":"192.168.124.42","freq":500}`
2. `run_lua_script script="nmminer_control" args={"ip":"192.168.124.42","action":"restart"}`

## Notes

- All endpoints are HTTP (not HTTPS), no authentication, port 80.
- Auto subnet mode (no `seed_ips`): worst case ~5–60 s on an empty /24
  (250 ms × 254 hosts), short-circuits the moment any `/alive` responder is
  found — typically **2–10 s** when at least one NMMiner is online.
- Seeded mode: **1–3 s** total. Pass `seed_ips` whenever a known NM IP is
  available (or the user asks for a re-scan after a recent successful one).
- Output of every script is bounded to fit the cap_lua 4 KB stdout buffer;
  very large fleets are summarized rather than dumped row-by-row.
