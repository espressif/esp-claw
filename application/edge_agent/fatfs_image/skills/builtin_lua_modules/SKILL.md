---
{
  "name": "builtin_lua_modules",
  "description": "Built-in Lua module documentation.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Builtin Lua Modules

To read documentation for a module, call `read_file("scripts/docs/<Doc file path>")`.
To read a module test script, call `read_file("scripts/builtin/test/<Test script path>")`.
Read all the files you need in one go as much as possible.
Do not fabricate functions that are not documented.

| Module | Doc file path | Test script path |
| --- | --- | --- |
| `lua_driver_adc` | `lua_driver_adc.md` | `adc_read.lua` |
| `lua_driver_gpio` | `lua_driver_gpio.md` | - |
| `lua_driver_i2c` | `lua_driver_i2c.md` | `i2c_scan_rw.lua` |
| `lua_driver_mcpwm` | `lua_driver_mcpwm.md` | `mcpwm_12ch.lua`<br>`servo_sweep.lua` |
| `lua_driver_uart` | `lua_driver_uart.md` | `uart_at.lua` |
| `lua_module_board_manager` | `lua_module_board_manager.md` | - |
| `lua_module_button` | `lua_module_button.md` | `button_events.lua` |
| `lua_module_call_capability` | `lua_module_call_capability.md` | `capability_call.lua` |
| `lua_module_delay` | `lua_module_delay.md` | - |
| `lua_module_display` | `lua_module_display.md` | `display_shapes.lua` |
| `lua_module_esp_heap` | `lua_module_esp_heap.md` | - |
| `lua_module_event_publisher` | `lua_module_event_publisher.md` | `llm_analyze_trigger.lua` |
| `lua_module_http` | `lua_module_http.md` | - |
| `lua_module_led_strip` | `lua_module_led_strip.md` | `led_strip_rainbow.lua` |
| `lua_module_ssd1306` | `lua_module_ssd1306.md` | `ssd1306_test.lua` |
| `lua_module_storage` | `lua_module_storage.md` | - |
| `lua_module_system` | `lua_module_system.md` | `system_info.lua` |
