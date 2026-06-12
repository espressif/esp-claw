# Wave Rover Reference Firmware Analysis

## 1. Hardware confirmed for SKU 25377

- 4WD chassis with 4× N20 geared motors (2 per driver side)
- ESP32-WROOM-32 multi-function driver board
- Integrated 3S 18650 UPS module (INA219 monitor)
- SSD1306 OLED 128×32
- QMI8658 (accel+gyro) + AK09918 (mag) 9-axis IMU
- TF/SD card slot (not used in MVP)
- Serial bus servo interface (not used in MVP)

## 2. Controller board model

General Driver for Robots (Waveshare). ESP32-WROOM-32 module.

## 3. ESP32 chip/board target

ESP32-WROOM-32. PlatformIO board: `esp32dev`. Framework: `espidf`.

## 4. Motor driver and GPIO/PWM mapping

| Signal | GPIO | Role |
|---|---|---|
| PWMA | 25 | Motor A (left) PWM |
| AIN1 | 21 | Motor A direction 1 |
| AIN2 | 17 | Motor A direction 2 |
| PWMB | 26 | Motor B (right) PWM |
| BIN1 | 22 | Motor B direction 1 |
| BIN2 | 23 | Motor B direction 2 |
| AENCA/B | 35/34 | Encoder A (left, stubs) |
| BENCA/B | 27/16 | Encoder B (right, stubs) |

LEDC channel A=5, channel B=6. Frequency: 100 kHz. Resolution: 8-bit (0–255).

WAVE ROVER uses direct PWM (no PID). Speed [-1.0, 1.0] → PWM [0, 255]:
- Positive speed: AIN1=HIGH, AIN2=LOW, ledcWrite(ch, round(spd*255))
- Negative speed: AIN1=LOW, AIN2=HIGH, ledcWrite(ch, round(-spd*255))
- Zero: AIN1=LOW, AIN2=LOW, ledcWrite(ch, 0)

## 5. UPS/INA219 wiring and I2C address

I2C bus: SDA=GPIO32, SCL=GPIO33. INA219 address: **0x42**.
Shunt: 0.01 Ω. Bus range: 16 V. PGain: ±320 mV.
Reads: bus voltage (V), shunt voltage (mV), current (mA), power (mW).
3S LiPo: nominal 11.1 V (range 9–12.6 V).

## 6. OLED controller and I2C address

SSD1306, 128×32 px. I2C address: **0x3C**. Same I2C bus (SDA=32, SCL=33).
ESP-IDF driver: use `i2c_master_transmit` with SSD1306 command protocol.
4 text lines at 8px font height.

## 7. IMU chip and I2C address

QMI8658: accel+gyro, I2C address **0x6B**.
AK09918: magnetometer, I2C address **0x0C**.
Both on same I2C bus. Init via register writes (WHO_AM_I check, CTRL7 enable).

## 8. JSON command protocol (ugv_base_general)

| Cmd | T value | Format | Notes |
|---|---|---|---|
| Speed ctrl | 1 | `{"T":1,"L":<float>,"R":<float>}` | L/R in [-1,1] |
| PWM input | 11 | `{"T":11,"L":<int>,"R":<int>}` | L/R in [-255,255] |
| ROS ctrl | 13 | `{"T":13,"X":<m/s>,"Z":<rad/s>}` | linear+angular |
| OLED ctrl | 3 | `{"T":3,"lineNum":<0-3>,"Text":"..."}` | |
| Get IMU | 126 | `{"T":126}` | returns accel/gyro/mag |
| Calibrate IMU | 127 | `{"T":127}` | 5-second still calibration |
| Heartbeat stop | — | No cmd for 3000ms | Motors stop automatically |

## 9. Serial/USB/HTTP/ESP-NOW transports

Reference firmware accepts JSON commands on:
- UART0 (Serial, 115200 baud)
- USB-Serial (same as UART0)
- HTTP POST (Wi-Fi AP or STA mode)
- ESP-NOW broadcast (MAC FF:FF:FF:FF:FF:FF)

## 10. Existing build system

Reference firmware: Arduino IDE. Libraries: ArduinoJson, Adafruit_SSD1306, INA219_WE, ESP32Encoder.
Our firmware: ESP-IDF 5.5 via PlatformIO `espidf` framework. Native I2C master driver.

## 11. What can be reused directly

- GPIO pin definitions (direct from ugv_config.h)
- Motor direction logic (direct translation from switchPortCtrlA/B)
- INA219 register protocol (translated from INA219_WE to raw I2C)
- SSD1306 command sequence (translatable from Adafruit_SSD1306)
- QMI8658 register map (from QMI8658.h, direct translation)
- JSON command protocol reference (for MCP tool parameter alignment)

## 12. What must be wrapped behind MCP

All hardware access. MCP tools are the only public interface.

## 13. What is still unknown

- Exact 3S battery voltage thresholds for "low battery" flag (assumed <10.5 V).
- QMI8658 full initialization sequence (power-on defaults vs explicit config).
- Whether AK09918 requires QMI8658 pass-through mode or is directly accessible.
- Exact Wi-Fi AP default password (assumed `12345678` from community sources).
- Exact flash partition layout for wave rover (using rover_demo's partitions.csv as reference).
