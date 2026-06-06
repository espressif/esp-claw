# Troubleshooting

## Build: "Missing the `src` folder"

Ensure `platformio.ini` contains `src_dir = main` under `[platformio]`.

## Build: managed component not found

Run `pio run` (not `idf.py`) from `application/wave_rover/`. PlatformIO resolves
managed components automatically via the IDF component manager.

## Build: construct not found

```bash
cd application/wave_rover
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pio run
```

## OLED not displaying

- Check I2C wiring: SDA=GPIO32, SCL=GPIO33.
- Verify address 0x3C with an I2C scanner sketch.
- Set `dry_run = false` in NVS and reboot.
- Chars outside ASCII 32–90 render as space (font covers uppercase + digits only).

## INA219 shows 0V

- Check I2C address 0x42 (not the default 0x40 used by most breakout boards).
- Confirm 3S LiPo battery is connected and charged.
- Shunt resistor = 0.01 Ω (INA219 onboard shunt).

## IMU returns `present: false`

- QMI8658C at 0x6B; AK09918 at 0x0C.
- Confirm I2C connections to the IMU daughterboard.
- WHO_AM_I for QMI8658 should return 0x05.

## Motors not moving

1. Verify `dry_run = false` in NVS.
2. Check that GPIO 17, 21, 22, 23, 25, 26 are not reassigned by other code.
3. Check emergency stop: `rover.get_status` → `state` field.
4. Check low battery: `rover.get_power` → `low_battery`.

## curl: Connection refused

- Rover is not on the expected IP. Connect to AP `WR-ESP32` and use `192.168.4.1`.
- Or check STA mode: the assigned IP is printed in the serial monitor on boot.

## Motors don't stop after duration_ms

- Ensure the motor worker task is running (started by `wr_hal_init()`).
- Check `wr_motor_submit_and_wait()` returns `ESP_OK`; if it returns `ESP_ERR_INVALID_STATE`,
  emergency stop was active.

## High Wi-Fi reconnect time

Default STA reconnect timeout is 30 s. Extend `WR_WIFI_CONNECT_TIMEOUT_MS` in `wr_wifi.c`
or add retry logic in `app_main.c`.
