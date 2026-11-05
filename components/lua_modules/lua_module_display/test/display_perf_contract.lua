local display = require("display")
local system = require("system")
local options = type(args) == "table" and args or {}
local frames = math.tointeger(options.frames or 120)
assert(frames and frames > 0 and frames <= 500)

for pass = 1, 2 do
    for count = 1, 2 do
        local framebuffer_bytes, elapsed_ms, present_us
        do
            local screen <close> = display.open({ framebuffer_count = count })
            framebuffer_bytes = screen:info().framebuffer_bytes
            screen:begin({ clear = 0xFF101820 })
            screen:present()
            local start_ms = system.millis()
            present_us = 0
            for i = 1, frames do
                screen:begin()
                screen:fill_rect(12, 12, 64, 64, i % 2 == 0 and 0xFF2050A0 or 0xFF70B030)
                assert(screen:present())
                local stats = screen:stats()
                assert(stats.dirty_pixels == 4096)
                present_us = present_us + stats.present_us
            end
            elapsed_ms = system.millis() - start_ms
        end
        print(string.format("[display_perf_contract] pass=%d buffers=%d frames=%d fb_bytes=%d total_ms=%d avg_present_us=%d",
            pass, count, frames, framebuffer_bytes, elapsed_ms, present_us // frames))
    end
end
