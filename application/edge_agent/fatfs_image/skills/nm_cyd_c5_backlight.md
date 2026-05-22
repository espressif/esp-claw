# NM-CYD-C5 屏幕背光控制

控制 NM-CYD-C5 板载 ST7789 LCD 的背光（GPIO25，10-bit LEDC PWM，高电平有效）。**严禁自行编写 Lua 操作 LEDC / GPIO 来调亮度**，统一调用本脚本即可。

## When to use
当用户请求 “调亮屏幕 / 调暗屏幕 / 屏幕亮度 / 背光 X% / 关掉背光 / 关屏 / 关闭屏幕 / 屏幕关闭 / screen off / set screen off / 全亮 / dim screen / set brightness / backlight off / set backlight to N% / blacklight off / set blacklight to N%” 等任何指向 LCD **背光亮度** 的请求时使用。这是关闭/打开屏幕的**唯一正确入口**——不要用 `nm_cyd_c5_screen mode=clear`（那只是黑屏并不关背光）。

注意：本技能只调亮度（PWM 占空比），不绘制任何内容。屏幕显示文字 / 颜色仍然走 `nm_cyd_c5_screen` 技能。

## How to use
**必须**使用同步运行：

```
run_lua_script script="nm_cyd_c5_backlight" args=<JSON 对象>
```

不要先 `activate_skill lua_module_display`，本脚本已自洽；不要尝试操作 `gpio` / `ledc` / `board_manager`。

### 参数
- `percent` (整数 0..100)：目标亮度百分比。**唯一必填字段**（除非用 `mode=off`）。
- `mode` (可选)：`set` (默认) | `off` (= percent=0) | `on` (= percent=100)。

### 示例
- 调到 30%：`{"percent":30}`
- 全亮：`{"percent":100}` 或 `{"mode":"on"}`
- 关闭背光：`{"percent":0}` 或 `{"mode":"off"}`
- 半亮：`{"mode":"set","percent":50}`

### 返回
脚本会打印 `[nm_cyd_c5_backlight] set 30%` 这样的一行。如果 LEDC 设置失败会抛错，**不会**伪装成功。
