# Wave Rover — Руководство пользователя

## Содержание

1. [Первый запуск](#первый-запуск)
2. [Подключение к Wi-Fi ровера (режим AP)](#режим-ap-по-умолчанию)
3. [Подключение ровера к домашнему Wi-Fi (режим STA)](#режим-sta)
4. [Подключение MCP-клиента](#подключение-mcp-клиента)
   - [Claude Code CLI](#claude-code-cli)
   - [Claude Desktop](#claude-desktop)
   - [Тест через curl](#тест-через-curl)
5. [Настройка токена авторизации](#токен-авторизации)
6. [Основные команды через инструменты MCP](#основные-команды)

---

## Первый запуск

После прошивки ровер загружается, инициализирует I2C, моторы и сенсоры, поднимает Wi-Fi и запускает MCP-сервер.

На OLED-дисплее появится:
```
FW:0.1.0
AP mode
BATT:0.0V
MCP:ON
```

Серийный вывод (115200):
```
I wave_rover: Wave Rover MCP firmware v0.1.0 starting
I wr_config:  config loaded: wifi_mode=0 mcp_port=80
I wr_wifi:    AP started: SSID=WR-ESP32 IP=192.168.4.1
I wr_mcp:     MCP server started on port 80 at /mcp
I wave_rover: boot complete. MCP at http://192.168.4.1:80/mcp
```

---

## Режим AP (по умолчанию)

Ровер поднимает собственную точку доступа Wi-Fi.

| Параметр | Значение |
|----------|----------|
| SSID     | `WR-ESP32` |
| Пароль   | `12345678` |
| IP ровера | `192.168.4.1` |
| Порт MCP | `80` |

**Подключитесь к `WR-ESP32`** с компьютера или телефона.

Проверка доступности:
```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'
# → {"jsonrpc":"2.0","id":1,"result":{}}
```

---

## Режим STA

Чтобы ровер подключался к вашей домашней сети (и был доступен на одном IP с компьютером):

```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{
    "jsonrpc": "2.0", "id": 1,
    "method": "tools/call",
    "params": {
      "name": "rover.set_wifi",
      "arguments": {
        "mode": "sta",
        "ssid": "ВашSSID",
        "password": "ВашПароль",
        "save": true
      }
    }
  }'
```

После ответа `"note":"reboot_to_apply"` — перезагрузите ровер (отключите и подключите питание).

При следующем старте ровер подключится к вашей сети. IP будет виден в серийном мониторе:
```
I wr_wifi: STA connected, IP=192.168.1.42
I wave_rover: boot complete. MCP at http://192.168.1.42:80/mcp
```

Если ровер не подключился за 30 секунд — он останется без Wi-Fi, но MCP не запустится. Проверьте SSID/пароль и повторите.

> **Безопасность:** не вставляйте пароль в команду, которую сохраняете в истории shell или скриптах. Используйте переменную окружения:
> ```bash
> PASS="ВашПароль"
> curl ... -d "{...\"password\":\"$PASS\"...}"
> ```

---

## Веб-интерфейс управления

Откройте браузер и перейдите по адресу:

```
http://192.168.4.1/
```

Интерфейс (тёмная тема, работает на телефоне и компьютере):

- **Статус** — индикатор E-STOP, напряжение батареи
- **Drive** — виртуальный джойстик (мышь / тач), слайдер скорости, кнопки поворота, STOP, E-STOP
- **Wi-Fi** — смена режима (AP/STA/AP+STA), SSID, пароль, сканирование сетей, сохранение (ровер перезагружается)

> При смене Wi-Fi настроек ровер перезагружается автоматически. Новый IP будет виден в серийном мониторе.

---

## Подключение MCP-клиента

Ровер реализует [MCP 2024-11-05](https://spec.modelcontextprotocol.io) через HTTP POST. Адрес эндпоинта:

```
http://<IP ровера>:80/mcp
```

### Claude Code CLI

Добавьте ровер как MCP-сервер:

```bash
claude mcp add --transport http wave-rover http://192.168.4.1:80/mcp
```

Если включён токен авторизации:
```bash
claude mcp add --transport http wave-rover http://192.168.4.1:80/mcp \
  --header "Authorization: Bearer ВашТокен"
```

Проверка — в Claude Code:
```
/mcp
```
Должен появиться `wave-rover` со статусом connected.

Теперь в чате с Claude можно писать:
> «Покажи статус ровера»  
> «Проедь вперёд на 0.3 скорости 1 секунду»  
> «Покажи напряжение батареи»

### Claude Desktop

Откройте `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS) или `%APPDATA%\Claude\claude_desktop_config.json` (Windows) и добавьте:

```json
{
  "mcpServers": {
    "wave-rover": {
      "transport": "http",
      "url": "http://192.168.4.1:80/mcp"
    }
  }
}
```

С токеном:
```json
{
  "mcpServers": {
    "wave-rover": {
      "transport": "http",
      "url": "http://192.168.4.1:80/mcp",
      "headers": {
        "Authorization": "Bearer ВашТокен"
      }
    }
  }
}
```

Перезапустите Claude Desktop. В меню инструментов появятся `rover.*` инструменты.

### Тест через curl

Список всех инструментов:
```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

Статус ровера:
```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"rover.get_status","arguments":{}}}'
```

Список ресурсов (данные в реальном времени):
```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"resources/list","params":{}}'
```

Конфигурация (ресурс):
```bash
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"rover://config"}}'
```

---

## Токен авторизации

По умолчанию авторизация отключена (`auth_enabled = false`). Для сети, где ровер доступен посторонним, рекомендуется включить токен.

**Сейчас** токен можно задать только через NVS напрямую (инструмент настройки токена будет в следующей версии). Временный обходной путь — задать токен в коде:

В `application/wave_rover/components/wave_rover_config/wave_rover_config.c`, функция `wave_rover_config_defaults`:

```c
cfg->auth_enabled = true;
strlcpy(cfg->auth_token, "ваш-секретный-токен", sizeof(cfg->auth_token));
```

Перепрошейте. После этого все запросы без заголовка `Authorization: Bearer ваш-секретный-токен` будут получать ошибку 401.

---

## Основные команды

Все команды отправляются через `tools/call`. Примеры через `rover_cli.py`:

```bash
# Статус
python3 tools/rover_cli.py --host 192.168.4.1 status

# Батарея
python3 tools/rover_cli.py --host 192.168.4.1 power

# IMU
python3 tools/rover_cli.py --host 192.168.4.1 imu

# Остановить моторы
python3 tools/rover_cli.py --host 192.168.4.1 stop

# Экстренная остановка
python3 tools/rover_cli.py --host 192.168.4.1 emergency-stop

# Снять экстренную остановку
python3 tools/rover_cli.py --host 192.168.4.1 clear-estop

# Калибровка IMU (держите ровер неподвижно ~1 секунду)
curl -s http://192.168.4.1:80/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"rover.calibrate_imu","arguments":{"samples":50,"interval_ms":20}}}'

# Движение (только с --allow-motion, ровер должен быть в безопасном месте)
python3 tools/rover_cli.py --host 192.168.4.1 move \
  --linear 0.3 --angular 0 --duration-ms 1000 --allow-motion
```


## Power Optimization

Wave Rover implements a three-level power management system that automatically adjusts CPU frequency, Wi-Fi power-save mode, magnetometer polling, and display power based on rover activity.

### Power Modes

| Mode | Entry condition | CPU | Wi-Fi PS | Magnetometer | Display |
|------|----------------|-----|----------|--------------|---------|
| **ACTIVE** | Boot, any MCP/web command, rover driving | 240 MHz (locked) | Off (WIFI_PS_NONE) | 10 Hz continuous | On |
| **IDLE** | No activity for `active_timeout_sec` (default 60 s) | 80–240 MHz DFS | MIN_MODEM | 10 Hz continuous | On |
| **LOW_POWER** | No activity for `idle_to_low_power_sec` (default 300 s), or battery critical | 80–240 MHz DFS | MAX_MODEM | Power-down | Off |

> **DEEP_SLEEP deferred:** Deep sleep requires tearing down Wi-Fi and the MCP/web server, with a wake latency and reassociation cost incompatible with always-on remote control. This will be revisited if standby power measurements justify it.

Automatic transitions are driven by a 1 Hz background task. Any MCP tool call (motor/nav/display commands), web `/cmd` or `/settings` POST, or explicit `rover.power_set_mode` resets the idle timer and transitions to ACTIVE.

### Configuration

These fields are set in NVS and editable via `rover.power_configure` or the Settings page:

| Field | Default | Description |
|-------|---------|-------------|
| `power_mgr_enabled` | `true` | Enable/disable the power manager |
| `power_active_timeout_sec` | `60` | Seconds of inactivity before ACTIVE → IDLE |
| `power_idle_to_low_power_sec` | `300` | Total idle seconds before → LOW_POWER |
| `power_wifi_power_save` | `true` | Enable `esp_wifi_set_ps()` transitions |
| `power_reduce_cpu_frequency` | `true` | Enable `esp_pm_configure()` DFS |
| `power_disable_display_idle` | `true` | Turn SSD1306 off in LOW_POWER |
| `power_critical_battery_v` | `9.6 V` | Voltage threshold forcing LOW_POWER + motor stop |
| `power_telemetry_active_sec` | `5` | INA219 poll interval in ACTIVE mode |
| `power_telemetry_idle_sec` | `30` | INA219 poll interval in IDLE mode |
| `power_telemetry_low_power_sec` | `120` | INA219 poll interval in LOW_POWER mode |

> **Note:** `rover.power_configure` persists settings to NVS. The running power_mgr uses its boot-time config copy; changes take effect after reboot.

### MCP Tools

| Tool | Description |
|------|-------------|
| `rover.power_get_status` | Returns JSON with `mode`, `battery_voltage`, `low_battery`, `uptime_sec`, `last_activity_sec_ago`, `locks_active` |
| `rover.power_set_mode` | Force a mode: `{"mode":"IDLE"}`. Rejected if rover is driving or a sleep lock is active. |
| `rover.power_configure` | Persist config: `{"active_timeout_sec":120,"wifi_power_save":true,...}` |
| `rover.power_prevent_sleep` | Acquire a mode-floor lock: `{"reason":"recording","ttl_sec":300}`. Returns `lock_id`. |
| `rover.power_release_sleep_lock` | Release a lock: `{"lock_id":1}` |

Web endpoints: `GET /power` (same JSON as `power_get_status`), `POST /power {"mode":"IDLE"}`. The web UI **Power** tab provides one-click mode switching.

Prometheus metrics (via `GET /metrics`):

```
wave_rover_power_mode{mode="active"}   1   # 1 for active mode, 0 otherwise
wave_rover_power_mode{mode="idle"}     0
wave_rover_power_mode{mode="low_power"} 0
wave_rover_power_locks_active          0   # 1 when any sleep-prevention lock is held
```

### Test Plan

| # | Scenario | Expected mode | What should be ON | What should be OFF | Expected log |
|---|----------|--------------|-------------------|--------------------|--------------|
| 1 | Boot → Wi-Fi connect → no activity | ACTIVE → IDLE after `active_timeout_sec` | Wi-Fi, MCP, Web, display | — | `power: mode changed ACTIVE -> IDLE, reason=active_timeout` |
| 2 | Drive via MCP/web for 60 s | ACTIVE throughout | motors, full CPU, Wi-Fi PS off | — | (no transition while driving) |
| 3 | Idle 5+ minutes | LOW_POWER after `idle_to_low_power_sec` | Wi-Fi (max PS), MCP, Web | display, mag continuous | `power: mode changed IDLE -> LOW_POWER, reason=idle_timeout` |
| 4 | Web UI open, polling `/status` only | IDLE/LOW_POWER (status polling does not call notify_activity) | — | — | idle timer continues |
| 5 | MCP read-only tool calls only | IDLE/LOW_POWER per timers | — | — | — |
| 6 | Wi-Fi connection lost | mode logic unaffected | — | — | existing Wi-Fi reconnect log |
| 7 | Battery voltage < `power_critical_battery_v` | forced LOW_POWER, motors stopped | — | motors | `power: critical battery, voltage=X.XX` |
| 8 | OTA update in progress | pinned ACTIVE via `ota` lock | Wi-Fi full power, full CPU | — | `power: lock acquired, reason=ota, ttl=0` |
| 9 | LOW_POWER → drive command | immediate ACTIVE | motors, full CPU, Wi-Fi PS off | — | `power: mode changed LOW_POWER -> ACTIVE, reason=rover_busy` |

### Current Measurement (fill in with USB power meter)

| Scenario | Expected mode | Current (A) | Notes |
|----------|--------------|-------------|-------|
| Boot, Wi-Fi connected | ACTIVE | | |
| Idle 1 min | IDLE | | |
| Idle 5 min | LOW_POWER | | |
| Driving | ACTIVE | | |
