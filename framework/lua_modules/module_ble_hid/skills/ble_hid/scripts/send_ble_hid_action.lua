local actions = require("ble_hid_actions")

local action = type(args) == "table" and type(args.action) == "table" and args.action or args
local ok, err = actions.run(action)
if not ok then
    error("[send_ble_hid_action] " .. tostring(err))
end

print("ble_hid action sent type=" .. tostring(action and action.type))
