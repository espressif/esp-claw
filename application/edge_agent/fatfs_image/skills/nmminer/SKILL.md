---
{
  "name": "nmminer",
  "description": "Discover, classify, inspect, locate, restart, and configure NMMiner low-power devices and AxeOS/NMAxe high-ASIC devices through bundled on-device Lua scripts.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# NMMiner LAN Discovery And Settings

Use this skill when the user asks about NMMiner, NMAxe, NMAxeGamma, NMQAxe++, AxeOS, BitAxe, LAN Bitcoin solo miners, miner discovery, hashrate, temperature, pool settings, display preferences, market coins, ASIC voltage, ASIC frequency, locating, or restarting miners.

All scripts perform HTTP requests locally from the ESP device. Always answer the user in English. Do not manually loop through IPs in the LLM and do not hand-edit JSON from remote APIs. Run the most specific Lua script and treat its `SUMMARY:` or `RESULT:` lines as the source of truth.

## Mandatory Discovery Rule

For any request that does not name a specific IP address, first discover devices through `/alive`, then `/probe`:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_scan.lua","args":{}}
```

The scan script reads the local subnet, calls `/alive` to collect the LAN `ips` list, probes each candidate with `/probe`, and prints each device's `model`, `family`, `class`, and hashrate. `class=low` means `/probe.hr < 1 GH/s`; `class=high` means `/probe.hr >= 1 GH/s`. Use `model` and `family` for API shape decisions.

If the user gives a known miner IP only as a seed for faster discovery, pass it as `seed_ips`:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_scan.lua","args":{"seed_ips":["192.168.124.167"]}}
```

Useful scan args: `seed_ips`, `model_filter`, `max_targets`, `probe_timeout_ms`, `alive_timeout_ms`, `subnet_alive_timeout_ms`, `subnet_probe_timeout_ms`.

## Direct Status And Control

Read one miner status:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_info.lua","args":{"ip":"192.168.124.42"}}
```

Optional `section`: `system`, `realtime`, or `probe`.

Control one miner:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_control.lua","args":{"ip":"192.168.124.42","action":"find"}}
```

Actions: `restart`, `clearhits`, `find`, `wakeup`, `rescan`.

## Configure Devices

Use `nmminer_setting.lua` for mining pool settings, high-ASIC voltage/frequency, display preferences, LED state, screensaver duration, and market coin settings. The script always probes the target model first, GETs current settings from the endpoint, then PATCHes only the requested fields. For older NMMiner firmware that rejects PATCH, the script falls back to POST after the GET.

If the user provides an IP, modify that IP directly after `/probe` classification:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","brightness":60}}
```

If the user does not provide an IP, let the setting script scan with `/alive`, probe models, apply `model` / `family` / `hash_class` filters, then modify matching devices:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"model":"NMMiner","pool":"stratum+tcp://public-pool.io:21496","address":"bc1q...worker"}}
```

Filter args: `model`, `model_filter`, `models`, `family` (`nmminer` or `axeos`), `hash_class` or `power_class` (`low` or `high`).

### Pool Settings

For pool changes, use `/api/setting/mining`. If the user does not specify primary or secondary, change only the primary pool/address. If they explicitly ask for backup, secondary, or fallback, set `secondary=true`.

Primary example:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","pool":"stratum+tcp://pool.example.com:3333","address":"wallet.worker"}}
```

Secondary example:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","secondary":true,"pool":"stratum+tcp://backup.example.com:3333","address":"wallet.worker"}}
```

Accepted explicit fields: `primary_pool`, `primary_address`, `primary_password`, `secondary_pool`, `secondary_address`, `secondary_password`. For `family=nmminer`, these map to `PrimaryPool`, `PrimaryAddress`, `SecondaryPool`, and `SecondaryAddress`. For `family=axeos`, they map to nested `stratum.primary` and `stratum.fallback`.

### High-ASIC Settings

Only change high-ASIC tuning through `asicVcoreReq` and `asicFreqReq` on `/api/setting/mining`. Use this for AxeOS models such as `NMAxe`, `NMAxeGamma`, and `NMQAxe++`:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.18","freq":500,"vcore":1200}}
```

`freq` must be `400..600` MHz. `vcore` must be `1100..1400` mV. Frequency changes usually require `nmminer_control.lua` with `action=restart` to take effect.

### Preference Settings

Use `/api/setting/preference` for display brightness, screen rotation, LED enable, and screensaver duration:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","Brightness":80,"RotateScreen":90,"LedEnable":1,"ScreenSaver":"5m"}}
```

Preferred lowercase aliases are `brightness`, `rotate_screen`, `led_enable`, and `screen_saver`. For AxeOS models, `brightness` maps to `Brightness`, `led_enable` maps to `ledIndicator`, `screen_saver` maps to `screensaverEnable` / `screensaverTimeout`, and `rotate_screen` supports only `0` or `180` via `screenFlip`.

### Market Settings

Use `/api/setting/market` for digital currency display settings:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nmminer_setting.lua","args":{"ip":"192.168.124.42","main_coin":"BTC","watch_coins":"BTC,ETH,SOL","kline_rotate":"30s","price_page_mode":"ticker"}}
```

For NMMiner this maps to `MainCoin`, `WatchCoins`, `KlineRotate`, and `PricePageMode`. For AxeOS this maps to `mainprice` and `coinWatchlist`.

## Response Rules

Run exactly the relevant `lua_run_script` call and summarize the script output in English. For configuration, report which IPs changed, which endpoint was used, and any skipped model/filter mismatch. If a script fails, report the error directly and stop unless an earlier step already completed successfully and the user asked for a multi-step operation.