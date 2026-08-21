# ESP-Claw — Arduino UNO Port

This directory contains the Arduino UNO compatibility layer for the [ESP-Claw](../README.md) AI Agent Framework.

## Architecture: Split-Brain Design

The Arduino UNO (ATmega328P: 2KB SRAM, 32KB Flash, 16MHz) cannot run the full ESP-Claw stack — there's simply not enough memory for LLM inference, Lua runtime, event routing, or Wi-Fi networking. Instead, we use a **split-brain architecture**:

```
┌──────────────────────┐    Serial (115200 baud)    ┌──────────────────────┐
│      HOST BRAIN      │◄──────────────────────────►│    ARDUINO UNO       │
│                      │     JSON commands          │  (Peripheral Ctrl)   │
│  • ESP32 + ESP-Claw  │                            │                      │
│  • PC + Python bridge│     ┌──────────────┐       │  • GPIO read/write   │
│  • Raspberry Pi      │────►│ {"c":"dw",   │──────►│  • Analog read (ADC) │
│                      │     │  "p":13,     │       │  • PWM output        │
│  Handles:            │     │  "v":1}      │       │  • Servo control     │
│  • LLM API calls     │     └──────────────┘       │  • I2C master        │
│  • Event routing     │                            │                      │
│  • Memory / skills   │     ┌──────────────┐       │  Status LED blinks   │
│  • IM integrations   │◄────│ {"ok":true}  │◄──────│  to show firmware    │
│                      │     └──────────────┘       │  is running          │
└──────────────────────┘                            └──────────────────────┘
```

The **host** (ESP32, PC, or Raspberry Pi) runs the AI agent, makes LLM API calls, and sends hardware commands to the UNO over Serial. The **UNO** executes GPIO/ADC/PWM/Servo/I2C operations and returns results.

## Quick Start

### 1. Flash the Arduino UNO

Open `arduino_claw_uno/arduino_claw_uno.ino` in the Arduino IDE (or use arduino-cli):

```bash
# Using arduino-cli
arduino-cli compile --fqbn arduino:avr:uno arduino_claw_uno/
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno arduino_claw_uno/
```

### 2. Test with the Python Bridge

```bash
cd host_bridge
pip install -r requirements.txt
python bridge.py --interactive
```

You'll see an interactive prompt:
```
Connected: arduino_uno (ATmega328P)
Capabilities: gpio, adc, pwm, servo, i2c

claw-uno> mode 13 out       # Set pin 13 as output
OK
claw-uno> dw 13 1            # Turn on built-in LED
OK
claw-uno> ar 0               # Read analog pin A0
Value: 512 (2.50V)
claw-uno> i2s                # Scan I2C bus
Found 2 device(s): 0x3C, 0x68
```

### 3. Use as a Python Library

```python
from bridge import ArduinoClawBridge

uno = ArduinoClawBridge(port="/dev/ttyACM0")
uno.connect()

# GPIO — equivalent to ESP-Claw's lua_module_gpio
uno.pin_mode(13, "out")
uno.digital_write(13, 1)      # LED on
value = uno.digital_read(7)   # Read a button

# Analog — equivalent to ESP-Claw's lua_module_adc
reading = uno.analog_read(0)  # Read A0 (0-1023)
uno.pwm_write(9, 128)         # 50% duty cycle on pin 9

# Servo
uno.servo_attach(9)
uno.servo_write(9, 90)        # Move to 90°
angle = uno.servo_read(9)

# I2C — equivalent to ESP-Claw's lua_module_i2c
devices = uno.i2c_scan()
uno.i2c_write(0x3C, [0x00, 0xAE])
data = uno.i2c_read(0x3C, 2)

uno.disconnect()
```

## Serial Protocol Reference

All commands are newline-delimited JSON. Max message size: 128 bytes.

### Identity & Heartbeat

| Command | Request | Response |
|---|---|---|
| Identify | `{"c":"id"}` | `{"board":"uno","chip":"ATmega328P","sram":2048,"flash":32768,"pins":{"digital":14,"analog":6},"caps":["gpio","adc","pwm","servo","i2c"]}` |
| Ping | `{"c":"pi"}` | `{"ok":true}` |

### GPIO

| Command | Request | Response |
|---|---|---|
| pinMode | `{"c":"pm","p":13,"m":"out"}` | `{"ok":true}` |
| digitalWrite | `{"c":"dw","p":13,"v":1}` | `{"ok":true}` |
| digitalRead | `{"c":"dr","p":7}` | `{"ok":true,"v":1}` |

Modes: `"in"` (INPUT), `"out"` (OUTPUT), `"pu"` (INPUT_PULLUP)

### Analog

| Command | Request | Response |
|---|---|---|
| analogRead | `{"c":"ar","p":0}` | `{"ok":true,"v":512}` |
| analogWrite (PWM) | `{"c":"pw","p":9,"v":128}` | `{"ok":true}` |

- ADC pins: A0–A5 (pass 0–5 or 14–19)
- PWM pins: 3, 5, 6, 9, 10, 11 (8-bit: 0–255)

### Servo

| Command | Request | Response |
|---|---|---|
| Attach | `{"c":"sa","p":9}` | `{"ok":true}` |
| Write angle | `{"c":"sv","p":9,"v":90}` | `{"ok":true}` |
| Read angle | `{"c":"sr","p":9}` | `{"ok":true,"v":90}` |
| Detach | `{"c":"sd","p":9}` | `{"ok":true}` |

Max 4 simultaneous servos (SRAM limitation).

### I2C

| Command | Request | Response |
|---|---|---|
| Write | `{"c":"i2w","a":60,"d":"48656C"}` | `{"ok":true}` |
| Read | `{"c":"i2r","a":60,"n":2}` | `{"ok":true,"d":"4F4B"}` |
| Scan | `{"c":"i2s"}` | `{"ok":true,"devs":[60,104]}` |

- Data is hex-encoded (e.g., `"48656C"` = bytes `[0x48, 0x65, 0x6C]`)
- Max 16 bytes per I2C write, max 16 bytes per I2C read
- I2C uses pins A4 (SDA) and A5 (SCL)

### Error Responses

All errors follow this format:
```json
{"ok":false,"e":"error message"}
```

Common errors: `"bad pin"`, `"pin reserved"`, `"bad mode"`, `"not pwm pin"`, `"not adc pin"`, `"no servo slot"`, `"not attached"`, `"i2c nack"`, `"overflow"`, `"parse fail"`

## Mapping to ESP-Claw Components

| ESP-Claw Component | Arduino UNO Equivalent |
|---|---|
| `lua_module_gpio` | `cmd_gpio.cpp` — GPIO via serial |
| `lua_module_adc` | `cmd_analog.cpp` — ADC via serial |
| `lua_module_mcpwm` | `cmd_analog.cpp` — PWM via serial |
| `lua_module_i2c` | `cmd_i2c.cpp` — I2C via serial |
| `board_info.yaml` | `board_defs.h` — UNO pin definitions |
| `claw_core` | Stays on host (ESP32 or PC) |
| `claw_event_router` | Stays on host |
| `claw_memory` | Stays on host |
| `cap_im_*` | Stays on host |

## Pin Map

```
Arduino UNO R3 Pin Map
─────────────────────────────────
 D0  — Serial RX (RESERVED)
 D1  — Serial TX (RESERVED)
 D2  — Digital I/O
 D3  — Digital I/O + PWM ⚡
 D4  — Digital I/O
 D5  — Digital I/O + PWM ⚡
 D6  — Digital I/O + PWM ⚡
 D7  — Digital I/O
 D8  — Digital I/O
 D9  — Digital I/O + PWM ⚡ + Servo
 D10 — Digital I/O + PWM ⚡ + SPI SS
 D11 — Digital I/O + PWM ⚡ + SPI MOSI
 D12 — Digital I/O + SPI MISO
 D13 — Digital I/O + Built-in LED 💡
 A0  — Analog Input
 A1  — Analog Input
 A2  — Analog Input
 A3  — Analog Input
 A4  — Analog Input + I2C SDA
 A5  — Analog Input + I2C SCL
─────────────────────────────────
```

## Memory Budget

The firmware is designed to fit within the UNO's tight constraints:

| Resource | Budget | Usage (approx) |
|---|---|---|
| Flash (32 KB) | 32,256 bytes | ~8-10 KB |
| SRAM (2 KB) | 2,048 bytes | ~700-900 bytes |
| EEPROM (1 KB) | 1,024 bytes | 0 (reserved for future) |

## License

Apache-2.0 — same as the main ESP-Claw project.
