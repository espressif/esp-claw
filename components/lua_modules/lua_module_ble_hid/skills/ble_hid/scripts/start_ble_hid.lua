local ble_hid = require("ble_hid")

local name = (type(args) == "table" and type(args.name) == "string" and args.name ~= "")
    and args.name
    or "esp-claw-hid"

local function fail(label, result, err)
    if result == nil then
        error("[start_ble_hid] " .. label .. " failed: " .. tostring(err))
    end
end

local status = ble_hid.status()
if not status.initialized then
    local ok, err = ble_hid.init({ name = name })
    fail("ble_hid.init", ok, err)
end

status = ble_hid.status()
if not status.advertising and not status.connected then
    local ok, err = ble_hid.start({ name = name })
    fail("ble_hid.start", ok, err)
end

status = ble_hid.status()
print("ble_hid status initialized=" .. tostring(status.initialized)
    .. " advertising=" .. tostring(status.advertising)
    .. " connected=" .. tostring(status.connected)
    .. " bonded=" .. tostring(status.bonded))
