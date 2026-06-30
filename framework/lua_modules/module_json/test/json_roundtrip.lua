local json = require("json")

local encoded = json.encode({
    ok = true,
    value = 3,
    list = {1, 2, "x"},
})
print(encoded)

local decoded = json.decode(encoded)
assert(decoded.ok == true)
assert(decoded.value == 3)
assert(decoded.list[1] == 1)
assert(decoded.list[2] == 2)
assert(decoded.list[3] == "x")

local object = json.decode('{"ok":true,"list":[1,2,"x"]}')
assert(object.ok == true)
assert(object.list[3] == "x")

local ok_invalid = pcall(function()
    json.decode("{bad")
end)
assert(ok_invalid == false)

local ok_unsupported = pcall(function()
    json.encode(function() end)
end)
assert(ok_unsupported == false)

print("json_roundtrip ok")
