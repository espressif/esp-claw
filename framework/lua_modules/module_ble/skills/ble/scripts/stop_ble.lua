-- BLE module stop entry point for Agent calls.
--
-- This script stops ordinary BLE advertising and deinitializes the generic BLE
-- module. It is the recovery path for skills/ble/scripts/start_ble.lua failures.

local ble = require("ble")

local function log(msg)
    print("[stop_ble] " .. msg)
end

local function call(label, fn)
    local ok, result, err = pcall(fn)
    if not ok then
        log(label .. " raised: " .. tostring(result))
        return nil, tostring(result)
    end
    if result == nil then
        log(label .. " failed: " .. tostring(err))
        return nil, err
    end
    log(label .. " ok")
    return result
end

call("adv_stop", function()
    return ble.adv_stop()
end)

local ok, err = call("deinit", function()
    return ble.deinit()
end)

if ok == nil then
    error("[stop_ble] deinit failed: " .. tostring(err))
end

log("stopped")
