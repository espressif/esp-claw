---
{
  "name": "lora_radio",
  "description": "Use the HPD16A/SX1262 LoRa module to send or receive data and show live LoRa TX/RX packets on the LCD. Requires the module frequency variant and a legal operating frequency.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# LoRa Radio

Use this skill for raw SX1262 LoRa packet communication. This is point-to-point
LoRa, not LoRaWAN.

Before running the monitor, obtain an explicit `frequency_hz` from the user.
The HPD16A is sold in different frequency variants, and the operating frequency
must both match the installed module and be legal in the deployment region. Do
not guess 433/470/868/915 MHz. Both peers must use the same frequency and all
other LoRa parameters.

For LilyGo T-Connect-Pro S3, use these fixed hardware values:

- Existing shared bus: `spi_host = 2` (`SPI3_HOST`), MOSI GPIO11, MISO GPIO13,
  SCLK GPIO12.
- LoRa: CS GPIO14, RESET GPIO42, DIO1 GPIO45, BUSY GPIO38.
- LCD and Ethernet remain on the same initialized SPI bus with their own chip
  selects. The script must not initialize or free that bus.

Run exactly one bundled monitor asynchronously. If execution returns an error,
report it directly; do not retry with a different frequency or GPIO assignment.

## Monitor Args

```json
{
  "type": "object",
  "required": ["frequency_hz"],
  "properties": {
    "frequency_hz": { "type": "integer", "minimum": 150000000, "maximum": 960000000 },
    "bandwidth_khz": { "type": "integer", "enum": [7, 10, 15, 20, 31, 41, 62, 125, 250, 500], "default": 125 },
    "spreading_factor": { "type": "integer", "minimum": 5, "maximum": 12, "default": 9 },
    "coding_rate": { "type": "integer", "enum": [5, 6, 7, 8], "default": 6 },
    "tx_power_dbm": { "type": "integer", "minimum": -9, "maximum": 22, "default": 14 },
    "current_limit_ma": { "type": "integer", "minimum": 60, "maximum": 140, "default": 140 },
    "preamble_length": { "type": "integer", "minimum": 1, "maximum": 65535, "default": 16 },
    "sync_word": { "type": "integer", "minimum": 0, "maximum": 255, "default": 171 },
    "send_text": { "type": "string", "maxLength": 255 },
    "send_hex": { "type": "string", "description": "Even-length hexadecimal bytes, optional spaces allowed" },
    "interval_ms": { "type": "integer", "minimum": 100 },
    "tx_timeout_ms": { "type": "integer", "minimum": 1, "maximum": 262143, "default": 5000 }
  }
}
```

Specify at most one of `send_text` and `send_hex`. With neither, the script only
receives. With data and no `interval_ms`, it sends once at startup and then
listens continuously. With `interval_ms`, it sends at that interval and resumes
continuous receive after every transmission. To send new data while a monitor
is running, replace the existing async job using the same tool fields below.

## Tool Call

Use `lua_run_script_async` with:

- Path: `{CUR_SKILL_DIR}/scripts/lora_lcd_monitor.lua`
- Timeout: `0`
- Name: `lora_lcd_monitor`
- Exclusive: `display_app`
- Replace: `true`

Example only after the user explicitly selected 868.6 MHz:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/lora_lcd_monitor.lua",
  "args": {
    "frequency_hz": 868600000,
    "bandwidth_khz": 125,
    "spreading_factor": 9,
    "coding_rate": 6,
    "tx_power_dbm": 14,
    "current_limit_ma": 140,
    "sync_word": 171,
    "send_text": "hello LoRa"
  },
  "timeout_ms": 0,
  "name": "lora_lcd_monitor",
  "exclusive": "display_app",
  "replace": true
}
```

The LCD shows the last TX payload, last RX payload, RSSI, and SNR. Stop or
replace the async Lua job to release the display and SX1262 device.

## Radio Checks

- Use a suitable antenna before transmitting. Do not transmit without one.
- Confirm both nodes use identical RF settings.
- Keep the requested transmit power within local regulatory limits.
- Common ground is required. The module logic must be 3.3 V compatible.
- Raw LoRa peers cannot communicate with a LoRaWAN gateway without a LoRaWAN
  protocol stack.
