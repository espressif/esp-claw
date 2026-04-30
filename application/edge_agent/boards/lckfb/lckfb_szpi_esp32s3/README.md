# LCKFB 实战派 ESP32-S3 (SZPI) - ESP-Claw 适配

## 硬件信息

- **模组**: ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM)
- **显示屏**: ST7789 2.0" IPS 320×240 (SPI)
- **触摸屏**: FT6336 电容触摸 (I2C)
- **音频 DAC**: ES8311 (I2C + I2S)
- **音频 ADC**: ES7210 四通道 (I2C + I2S, 使用2通道麦克风)
- **音频功放**: NS4150B (通过 PCA9557 IO 扩展器控制使能)
- **姿态传感器**: QMI8658 (I2C)
- **IO 扩展**: PCA9557 (控制 LCD_CS, PA_EN, DVP_PWDN)
- **TF 卡**: 1-SD 模式

## 引脚分配

| 功能 | GPIO |
|------|------|
| I2C SDA | 1 |
| I2C SCL | 2 |
| I2S MCLK | 38 |
| I2S BCK | 14 |
| I2S WS | 13 |
| I2S DOUT (Speaker) | 45 |
| I2S DIN (Mic) | 12 |
| LCD SPI MOSI | 40 |
| LCD SPI CLK | 41 |
| LCD DC | 39 |
| LCD Backlight | 42 |
| SD CMD | 48 |
| SD CLK | 47 |
| SD DAT0 | 21 |

## 编译与烧录

```bash
cd esp-claw/application/edge_agent

# 选择开发板
idf.py gen-bmgr-config -c ./boards -b lckfb_szpi_esp32s3

# 编译
idf.py build

# 烧录并监控
idf.py flash monitor
```

## LLM 配置 (Minimax M2.7)

烧录完成后，设备会启动一个 Wi-Fi AP (esp-claw-XXXXXX)。连接后访问 `http://192.168.4.1` 进入 Web 配置页面，设置以下参数：

| 配置项 | 值 |
|--------|-----|
| LLM Profile | `custom_openai_compatible` |
| LLM Base URL | `https://api.minimax.chat/v1` |
| LLM Model | `M2.7` |
| LLM API Key | 你的 Minimax API Key |

配置完成后重启设备即可使用。

## 注意事项

- QMI8658 IMU 暂未在 ESP-Claw 框架中支持，后续可通过 Lua 模块扩展
- LCD CS 引脚通过 PCA9557 IO 扩展器控制，已在 setup_device.c 中自动初始化
- 音频功放 (PA) 通过 PCA9557 控制，开机自动使能
