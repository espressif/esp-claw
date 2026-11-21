local delay = require("delay")
local lvgl = require("lvgl")

local ICON_SIZE = 20
local CELL = 30
local MARGIN = 8
local SHOW_MS = 8000

local SAMPLE = {
    "OK", "CLOSE", "BACKSPACE", "NEW_LINE", "KEYBOARD", "WIFI", "BLUETOOTH", "BATTERY_FULL",
    "SNOWFLAKE", "SUN", "TEMPERATURE_HIGH", "LIGHTBULB", "CLOUD_SUN", "ROBOT", "MICROPHONE", "SMILE",
    "CLOCK", "INFO_CIRCLE", "MAP_MARKER_ALT", "STORE", "SHOPPING_CART", "ROCKET", "LEAF", "CAT",
}

assert(type(lvgl.SYMBOL) == "table", "lvgl.SYMBOL missing")
local count = 0
for name, text in pairs(lvgl.SYMBOL) do
    assert(type(name) == "string" and type(text) == "string" and #text > 0, "bad symbol entry: " .. tostring(name))
    count = count + 1
end
for _, name in ipairs(SAMPLE) do
    assert(lvgl.SYMBOL[name], "missing symbol: " .. name)
end
print(string.format("[lvgl_symbols] %d symbols", count))

lvgl.init({
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local font = lvgl.font_load({ size = ICON_SIZE, cache_size = 128 })
    local scr = lvgl.create_screen()
    local width = scr:get_size()
    scr:set_style({ bg_color = "#101820" })

    local columns = math.max(1, (width - 2 * MARGIN) // CELL)
    for i, name in ipairs(SAMPLE) do
        local index = i - 1
        lvgl.label(scr, {
            text = lvgl.SYMBOL[name],
            x = MARGIN + (index % columns) * CELL,
            y = MARGIN + (index // columns) * CELL,
            w = CELL,
            h = CELL,
            text_color = "#f5f7fa",
            font = font,
        })
    end

    scr:load()
    delay.delay_ms(SHOW_MS)
end)

lvgl.deinit()

if not ok then
    error(err)
end
