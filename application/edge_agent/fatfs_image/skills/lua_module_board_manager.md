# Lua Board Manager

This skill describes how to correctly use board_manager when writing Lua scripts.

## How to call
- Import it with `local board_manager = require("board_manager")`
- Call `board_manager.get_board_info()` to read board metadata such as `name`, `chip`, and `version`
- Call `board_manager.init_device(name)` before using a board-managed peripheral
- Call `board_manager.deinit_device(name)` when the peripheral is no longer needed
- Call `board_manager.get_device_handle(name)` or `board_manager.get_device_config_handle(name)` to resolve low-level handles
- Call `board_manager.get_display_lcd_params(name)` to get `panel_handle`, `io_handle`, `lcd_width`, `lcd_height`, and `panel_if`
- Use the built-in constants `board_manager.PANEL_IF_IO`, `board_manager.PANEL_IF_RGB`, and `board_manager.PANEL_IF_MIPI_DSI`
- `panel_if` returned by `get_display_lcd_params(...)` matches one of those constants
- Call `board_manager.get_lcd_touch_handle(name)` to get the raw LCD touch handle
- Call `board_manager.get_audio_codec_input_params(name)` or `board_manager.get_audio_codec_output_params(name)` to get codec handles and format parameters
- Call `board_manager.get_camera_paths()` to get camera device paths such as `dev_path` and `meta_path`

## Example
```lua
local board_manager = require("board_manager")

local info = board_manager.get_board_info()
print(info.name, info.chip)

board_manager.init_device("display_lcd")
local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")
print(width, height, panel_if)
if panel_if == board_manager.PANEL_IF_MIPI_DSI then
    print("using DSI panel")
end

local camera_paths = board_manager.get_camera_paths()
print(camera_paths.dev_path)
```
