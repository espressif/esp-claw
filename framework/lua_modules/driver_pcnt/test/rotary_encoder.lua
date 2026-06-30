local pcnt = require("pcnt")
local delay = require("delay")

local gpio_a = 0
local gpio_b = 2

local encoder = pcnt.new({
    low_limit = -100,
    high_limit = 100,
    accum_count = true,
    glitch_ns = 1000,
    edge_gpio = gpio_a,
    level_gpio = gpio_b,
    pos_edge = "decrease",
    neg_edge = "increase",
    high_level = "keep",
    low_level = "inverse",
})

encoder:add_channel({
    edge_gpio = gpio_b,
    level_gpio = gpio_a,
    pos_edge = "increase",
    neg_edge = "decrease",
    high_level = "keep",
    low_level = "inverse",
})

encoder:clear()
encoder:start()

for _ = 1, 30 do
    print(string.format("encoder count=%d", encoder:get_count()))
    delay.delay_ms(1000)
end

encoder:stop()
encoder:close()
