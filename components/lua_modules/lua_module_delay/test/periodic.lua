local delay = require("delay")

local ticker = delay.periodic(10)
for _ = 1, 3 do
    assert(type(ticker:wait()) == "boolean")
end
ticker:reset()
assert(type(ticker:wait()) == "boolean")

local ok = pcall(delay.periodic, 0)
assert(not ok)
print("periodic PASS")
