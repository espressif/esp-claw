local mcpwm = require("mcpwm")
local delay = require("delay")

local DEFAULT_GPIOS = {4, 5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11}
local DEFAULT_FREQUENCY_HZ = 1000
local DEFAULT_RESOLUTION_HZ = 1000000
local DEFAULT_HOLD_MS = 3000

local a = type(args) == "table" and args or {}
local gpios = type(a.gpios) == "table" and a.gpios or DEFAULT_GPIOS
local frequency_hz = type(a.frequency_hz) == "number" and math.floor(a.frequency_hz) or DEFAULT_FREQUENCY_HZ
local resolution_hz = type(a.resolution_hz) == "number" and math.floor(a.resolution_hz) or DEFAULT_RESOLUTION_HZ
local hold_ms = type(a.hold_ms) == "number" and math.floor(a.hold_ms) or DEFAULT_HOLD_MS

local handles = {}

local function cleanup()
    for _, handle in ipairs(handles) do
        pcall(handle.stop, handle)
        pcall(handle.close, handle)
    end
    handles = {}
end

local function normalize()
    if #gpios ~= 12 then
        error("[mcpwm12] args.gpios must contain exactly 12 GPIO numbers")
    end
    if frequency_hz <= 0 then
        error("[mcpwm12] frequency_hz must be > 0")
    end
    if resolution_hz < frequency_hz then
        error("[mcpwm12] resolution_hz must be >= frequency_hz")
    end
    if hold_ms < 0 then
        error("[mcpwm12] hold_ms must be >= 0")
    end
end

local ok, err = xpcall(function()
    normalize()

    for i = 1, 6 do
        local base = (i - 1) * 2
        local group_id = i <= 3 and 0 or 1
        local duty_a = ((base * 7) % 90) + 5
        local duty_b = (((base + 1) * 7) % 90) + 5
        local handle = mcpwm.new({
            gpio = gpios[base + 1],
            gpio_b = gpios[base + 2],
            group_id = group_id,
            resolution_hz = resolution_hz,
            frequency_hz = frequency_hz,
            duty_percent = duty_a,
            duty_percent_b = duty_b,
        })

        handle:start()
        handles[#handles + 1] = handle

        print(string.format(
            "[mcpwm12] handle=%d group=%d ch1_gpio=%d duty=%.1f%% ch2_gpio=%d duty=%.1f%%",
            i, group_id, gpios[base + 1], duty_a, gpios[base + 2], duty_b))
    end

    print("[mcpwm12] 12 PWM outputs started")
    delay.delay_ms(hold_ms)

    for i, handle in ipairs(handles) do
        handle:set_duty(1, 50)
        handle:set_duty(2, 50)
        print(string.format("[mcpwm12] handle=%d all channels set to 50%%", i))
    end

    delay.delay_ms(hold_ms)
    print("[mcpwm12] done")
end, debug.traceback)

cleanup()
if not ok then
    error(err)
end
