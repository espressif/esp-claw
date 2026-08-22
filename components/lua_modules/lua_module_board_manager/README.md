# Lua Board Manager

This module describes how to correctly use board_manager when writing Lua scripts.

## How to call
- Import it with `local board_manager = require("board_manager")`
- Call `board_manager.get_board_info()` to read board metadata such as `name`, `chip`, and `version`
- Call `board_manager.init_device(name)` before using a board-managed peripheral
- Call `board_manager.deinit_device(name)` when the peripheral is no longer needed
- Call `board_manager.get_device_handle(name)` or `board_manager.get_device_config_handle(name)` to resolve low-level handles
- Call `board_manager.get_display_lcd_params(name)` to get `panel_handle`, `io_handle`, `lcd_width`, `lcd_height`, `panel_if`, and `pixel_format`
- Use the built-in constants `board_manager.PANEL_IF_IO`, `board_manager.PANEL_IF_RGB`, `board_manager.PANEL_IF_MIPI_DSI`, `board_manager.PIXEL_FORMAT_RGB565`, and `board_manager.PIXEL_FORMAT_RGB888`
- `panel_if` returned by `get_display_lcd_params(...)` matches one of those constants
- `pixel_format` returned by `get_display_lcd_params(...)` matches one of the pixel format constants
- Call `board_manager.get_lcd_touch_handle(name)` to get the raw LCD touch handle
- Call `board_manager.get_audio_codec_input_params(name)` or `board_manager.get_audio_codec_output_params(name)` to get codec handles and format parameters
- Call `board_manager.get_camera_paths()` to get camera device paths such as `dev_path` and `meta_path`

## Return values
- `get_board_info()` returns a metadata table.
- `init_device(name)` and `deinit_device(name)` return `true` on success or `nil, err` on failure.
- `get_device_handle(name)`, `get_device_config_handle(name)`, and `get_lcd_touch_handle(name)` return a lightuserdata handle or `nil, err`.
- `get_display_lcd_params(name)` returns `panel_handle, io_handle, lcd_width, lcd_height, panel_if, pixel_format`.
- `get_audio_codec_input_params(name)` returns `codec_handle, sample_rate, channels, bits, init_gain_db`.
- `get_audio_codec_output_params(name)` returns `codec_handle, sample_rate, channels, bits`.
- `get_camera_paths()` returns a table with camera path fields when the board exposes them.

## Example
```lua
local board_manager = require("board_manager")

local info = board_manager.get_board_info()
print(info.name, info.chip)

board_manager.init_device("display_lcd")
local panel_handle, io_handle, width, height, panel_if, pixel_format =
    board_manager.get_display_lcd_params("display_lcd")
print(width, height, panel_if, pixel_format)
if panel_if == board_manager.PANEL_IF_MIPI_DSI then
    print("using DSI panel")
end
if pixel_format == board_manager.PIXEL_FORMAT_RGB888 then
    print("using RGB888 pixels")
end

local camera_paths = board_manager.get_camera_paths()
print(camera_paths.dev_path)
```
