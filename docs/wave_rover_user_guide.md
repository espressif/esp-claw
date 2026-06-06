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
7. [Dry-run режим и реальное железо](#dry-run)

---

## Первый запуск

После прошивки ровер загружается в **безопасном режиме** (`dry_run = true`) — моторы и сенсоры отключены, Wi-Fi и MCP-сервер работают. Это позволяет настроить подключение до того, как ровер сможет двигаться.

На OLED-дисплее появится:
```
FW:0.1.0
no wifi
BATT:0.0V
MCP:ON
```

Серийный вывод (115200):
```
I wave_rover: Wave Rover MCP firmware v0.1.0 starting
I wr_config:  config loaded: wifi_mode=0 mcp_port=80 dry_run=1
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

- **Статус** — индикатор E-STOP, напряжение батареи, режим dry-run
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

---

## Dry-run

По умолчанию `dry_run = true` — моторы и сенсоры отключены. Это безопасный режим для разработки и тестирования MCP-подключения без риска движения ровера.

Чтобы включить реальное железо:

1. Убедитесь, что все провода подключены правильно (I2C: SDA=32, SCL=33; моторы: GPIO 17,21,22,23,25,26).
2. В `wave_rover_config.c` измените дефолт:
   ```c
   cfg->dry_run = false;
   ```
3. Или задайте через NVS после прошивки.
4. Перепрошейте / перезагрузите.

После отключения dry_run:
- `rover.get_power` вернёт реальное напряжение батареи
- `rover.get_imu` вернёт реальные данные акселерометра/гироскопа
- `rover.move` / `rover.drive_tank` / `rover.turn` будут двигать ровер

> ⚠ Перед первым движением убедитесь, что ровер стоит на ровной поверхности, ничему не мешает, и у вас готова команда `rover.emergency_stop`.
