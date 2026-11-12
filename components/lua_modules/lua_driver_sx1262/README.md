# SX1262 Lua Driver

This component exposes an SX1262 LoRa radio as the Lua module `sx1262`. It adds
the radio as a device on an SPI bus that the board has already initialized, so
the bus can also be used by a display or Ethernet controller. The driver does
not initialize or free the shared SPI bus.

Only one SX1262 object may be open at a time. Call `close()` when finished.
The module supports LoRa packets up to 255 bytes; it does not implement LoRaWAN.

## Create

```lua
local sx1262 = require("sx1262")

local radio = sx1262.new({
    spi_host = 2,          -- SPI3_HOST on ESP32-S3
    cs_gpio = 14,
    reset_gpio = 42,
    dio1_gpio = 45,
    busy_gpio = 38,
    frequency_hz = 868600000,
    bandwidth_khz = 125,
    spreading_factor = 9,
    coding_rate = 6,       -- 6 means 4/6
    tx_power_dbm = 14,
    current_limit_ma = 140,
    preamble_length = 16,
    sync_word = 0xAB,
    tcxo_voltage = 1.6,
    tcxo_delay_us = 5000,
    use_dcdc = true,
    dio2_rf_switch = true,
    crc = false,
    invert_iq = false,
})
```

`frequency_hz` and all four control GPIOs are required. `spi_host` defaults to
`2`. Valid bandwidth values are `7`, `10`, `15`, `20`, `31`, `41`, `62`,
`125`, `250`, and `500` kHz. Spreading factor is 5-12 and coding rate is the
denominator 5-8 for LoRa 4/5 through 4/8. The HPD16A TCXO is enabled by default
at 1.6 V, matching the board's bundled RadioLib example; set `tcxo_voltage = 0`
only for an XTAL module. `current_limit_ma`
defaults to 140 mA for the SX1262 high-power PA path and accepts 60-140 mA.

The SPI bus must already exist. `new()` raises an error for invalid arguments,
pin conflicts, missing SPI bus initialization, allocation failures, or radio
configuration failures.

## Send

```lua
local ok, err = radio:send("hello", 5000)
local ok2, err2 = radio:send({ 0x01, 0x02, 0x03 }, 5000)
```

`send(data[, timeout_ms])` accepts a binary string or a table of bytes. It
returns `true` after TX_DONE, or `nil, error` on timeout or a radio/SPI error.
It leaves the radio out of continuous receive mode, so call `start_receive()`
after sending when reception should continue.

## Receive

```lua
assert(radio:start_receive())
local packet, err = radio:receive(1000)
if packet then
    print(packet.data, packet.length, packet.rssi, packet.snr)
end
```

`start_receive()` enters continuous LoRa receive mode. `receive(timeout_ms)`
waits for a packet and returns a table with `data` (binary string), `bytes`,
`length`, `rssi`, `snr`, and `signal_rssi`. A wait timeout returns
`nil, "timeout"` without stopping continuous reception. CRC/header failures and
radio errors also return `nil, error`. TX and RX use DIO1 for prompt wake-up and
also poll the radio IRQ status, so a missed GPIO edge does not lose a packet or
misreport a completed transmission as a timeout.

## State And Cleanup

- `radio:status()` returns mode, radio error bits, receive state, and the active
  RF parameters.
- `radio:standby()` stops continuous receive and returns `true` or `nil, error`.
- `radio:close()` releases the radio's SPI device, GPIO handlers, pins, and
  synchronization objects. It does not release the shared SPI bus.

All radio operations block the calling Lua task for at most their specified
timeout. Do not access one radio object concurrently from multiple Lua tasks.
Both radios must use identical frequency, bandwidth, spreading factor, coding
rate, sync word, CRC, and IQ settings. Select a frequency permitted in the
deployment region and matching the installed HPD16A frequency variant.
