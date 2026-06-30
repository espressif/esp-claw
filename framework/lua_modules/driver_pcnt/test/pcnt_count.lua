local pcnt = require("pcnt")
local delay = require("delay")

local edge_gpio = 4

local unit = pcnt.new({
    edge_gpio = edge_gpio,
    glitch_ns = 1000,
})

unit:start()
delay.delay_ms(1000)
print(string.format("pcnt gpio=%d count=%d", edge_gpio, unit:get_count()))
unit:clear()
print(string.format("pcnt after clear=%d", unit:get_count()))
unit:stop()
unit:close()
