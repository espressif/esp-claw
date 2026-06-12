local mcpwm = require("mcpwm")
local delay = require("delay")

local DEFAULT_GPIOS = {4, 5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11}
local DEFAULT_FREQUENCY_HZ = 1000
local DEFAULT_RESOLUTION_HZ = 1000000
local DEFAULT_HOLD_MS = 3000
local DEFAULT_HANDLES_PER_GROUP = 3

local a = type(args) == "table" and args or {}
local gpios = type(a.gpios) == "table" and a.gpios or DEFAULT_GPIOS
local channel_count = type(a.channel_count) == "number" and math.floor(a.channel_count) or #gpios
local frequency_hz = type(a.frequency_hz) == "number" and math.floor(a.frequency_hz) or DEFAULT_FREQUENCY_HZ
local resolution_hz = type(a.resolution_hz) == "number" and math.floor(a.resolution_hz) or DEFAULT_RESOLUTION_HZ
local hold_ms = type(a.hold_ms) == "number" and math.floor(a.hold_ms) or DEFAULT_HOLD_MS
local base_group_id = type(a.group_id) == "number" and math.floor(a.group_id) or 0
local handles_per_group = type(a.handles_per_group) == "number"
    and math.floor(a.handles_per_group)
    or DEFAULT_HANDLES_PER_GROUP

local handles = {}

local function cleanup()
    for _, handle in ipairs(handles) do
        pcall(handle.stop, handle)
        pcall(handle.close, handle)
    end
    handles = {}
end

local function check_gpio_list()
    local seen = {}
    for i = 1, channel_count do
        local gpio = gpios[i]
        if type(gpio) ~= "number" or math.floor(gpio) ~= gpio then
            error(string.format("[mcpwm12] gpios[%d] must be an integer GPIO number", i))
        end
        if seen[gpio] then
            error(string.format("[mcpwm12] duplicate GPIO: %d", gpio))
        end
        seen[gpio] = true
    end
end

local function normalize()
    if channel_count < 2 or channel_count > 12 or channel_count % 2 ~= 0 then
        error("[mcpwm12] channel_count must be an even number in range 2-12")
    end
    if #gpios < channel_count then
        error("[mcpwm12] args.gpios must contain at least channel_count GPIO numbers")
    end
    if base_group_id < 0 then
        error("[mcpwm12] group_id must be >= 0")
    end
    if handles_per_group <= 0 then
        error("[mcpwm12] handles_per_group must be > 0")
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
    check_gpio_list()
end

local function group_for_handle(handle_index)
    if type(a.group_ids) == "table" and type(a.group_ids[handle_index]) == "number" then
        return math.floor(a.group_ids[handle_index])
    end
    return base_group_id + math.floor((handle_index - 1) / handles_per_group)
end

local ok, err = xpcall(function()
    normalize()

    local handle_count = channel_count / 2
    print(string.format(
        "[mcpwm12] starting %d PWM outputs with %d handles freq=%d resolution=%d",
        channel_count,
        handle_count,
        frequency_hz,
        resolution_hz
    ))

    for i = 1, handle_count do
        local base = (i - 1) * 2
        local group_id = group_for_handle(i)
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

        if handle:get_channel_count() ~= 2 then
            error(string.format("[mcpwm12] handle=%d expected 2 channels", i))
        end

        handle:start()
        handles[#handles + 1] = handle

        print(string.format(
            "[mcpwm12] handle=%d group=%d ch1_gpio=%d duty=%.1f%% ch2_gpio=%d duty=%.1f%%",
            i, group_id, gpios[base + 1], duty_a, gpios[base + 2], duty_b))
    end

    print(string.format("[mcpwm12] %d PWM outputs started", channel_count))
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
