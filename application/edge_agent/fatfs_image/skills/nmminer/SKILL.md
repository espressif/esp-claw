---
{
  "name": "nmminer",
  "description": "Discover, list, inspect, locate, restart, and configure NMMiner, NMAxe, NMAxeGamma, NMQAxe++, BitAxe, or LAN Bitcoin solo miner devices through bundled on-device Lua scripts.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# NMMiner LAN Discovery And Control

Use this skill when the user asks about NMMiner, NMAxe, NMAxeGamma, NMQAxe++, BitAxe, Bitcoin solo miners, LAN miners, mining hashrate, miner temperature, miner IP discovery, locating a miner, restarting a miner, or changing miner frequency/vcore.

The scripts perform HTTP requests locally from the device. Do not plan per-IP loops in the LLM and do not parse remote APIs manually. Choose one script and forward its printed summary.

## Scan Or List Miners

For broad discovery, call:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_scan.lua","args":{}}
```

If the user provides known miner IPs or seed IPs, pass them as `seed_ips` for faster scanning:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_scan.lua","args":{"seed_ips":["192.168.124.167"]}}
```

Useful optional args:

- `seed_ips`: known NMMiner IPs.
- `model_filter`: exact model filter such as `NMQAxe++`.
- `max_targets`: cap candidates.
- `probe_timeout_ms`, `alive_timeout_ms`, `subnet_alive_timeout_ms`, `subnet_probe_timeout_ms`.

## Read One Miner Status

Use when the user asks for hashrate, temperature, power, uptime, firmware, pool, or details for a known IP:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_info.lua","args":{"ip":"192.168.124.42"}}
```

Optional `section`: `system`, `realtime`, or `probe`.

## Control One Miner

Use when the user asks to restart, locate, wake, rescan, or clear hits on a known IP:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_control.lua","args":{"ip":"192.168.124.42","action":"find"}}
```

Actions:

- `restart`: reboot the miner.
- `clearhits`: reset block-hit counter.
- `find`: blink/locate the physical device.
- `wakeup`: wake the miner screen.
- `rescan`: trigger device-side swarm scan.

## Change Mining Settings

Use for frequency or vcore changes:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","freq":500,"vcore":1200}}
```

At least one of `freq` or `vcore` is required. `freq` range is `400..600` MHz. `vcore` range is `1100..1400` mV. Frequency changes require restart to take effect. If the user asks to apply a frequency change immediately, call `nmminer_setting.lua` first and then `nmminer_control.lua` with `action=restart`.

## Response Rules

Run the relevant `lua_run_script` call and summarize the script output. If the script prints a `SUMMARY:` line, use it as the source of truth. If the script fails, report the error directly and stop unless the user asked for a multi-step operation where the previous step succeeded.