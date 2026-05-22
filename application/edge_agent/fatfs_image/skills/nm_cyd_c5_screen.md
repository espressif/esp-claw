# NM-CYD-C5 屏幕控制

控制 NM-CYD-C5 板载 320×240 ST7789 LCD。**严禁自行编写 display Lua 脚本**，统一调用本脚本。脚本内部会正确接管屏幕、绘制、同步等待 `duration_ms`、再释放屏幕，让表情应用恢复显示。

## When to use
当用户请求“屏幕显示 / 显示 X / 屏幕变红 / 屏幕变蓝 / 提示信息 / 提示消息 / show on screen / set the screen to ...”等任何指向板载 LCD **绘制内容** 的请求时使用。

**不要**用本技能去“关屏 / 关背光 / 调暗 / set backlight off / screen off / 屏幕关闭”——那是亮度控制，必须使用 `nm_cyd_c5_backlight`（参数 `{"mode":"off"}` 或 `{"percent":0}`）。`mode=clear` 只是黑屏占位，**不会**真正关掉背光。

## How to use
**必须**使用同步运行：

```
run_lua_script script="nm_cyd_c5_screen" args=<JSON 对象>
```

不要使用 `lua_run_script_async`，否则脚本会立即返回，等待时间 `duration_ms` 不会真正生效，屏幕马上被表情应用重新接管。
不要先 `activate_skill lua_module_display`，本脚本已自洽。

### 模式（mode）
- `color`：纯色填充（`color` 名称或 `r/g/b`），`duration_ms` 默认 3000
- `text`：居中文字 + 背景色，`text` 必填，`bg`/`fg`/`font_size`/`duration_ms`
- `message`：标题栏 + 正文，`title` + `text`，其它同上
- `clear`：黑屏 `duration_ms`（默认 1000）然后释放屏幕

### 命名颜色
red, green, blue, white, black, yellow, cyan, magenta, orange, purple, pink, gray

### 示例
- 屏幕变红 10 秒：`{"mode":"color","color":"red","duration_ms":10000}`
- 蓝色 5 秒：`{"color":"blue","duration_ms":5000}`
- 自定义颜色：`{"r":50,"g":50,"b":200,"duration_ms":3000}`
- 居中显示文字：`{"mode":"text","text":"Hello!","bg":"blue"}`
- 状态弹窗：`{"mode":"message","title":"Status","text":"WiFi OK","bg":"black","duration_ms":4000}`
- 关闭/释放屏幕：`{"mode":"clear"}`

### 时长说明
`duration_ms` 是脚本内部 `delay.delay_ms` 的值。设为 0 表示立刻释放（不建议，用户看不到）。常见值 1000-10000。
