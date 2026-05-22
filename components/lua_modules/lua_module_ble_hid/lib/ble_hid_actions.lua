local M = {}

local SCHEMA = {
    media = { implemented = true },
    keyboard_key = { implemented = true },
    keyboard_combo = { implemented = true },
    keyboard_text = { implemented = true },
    keyboard = { deprecated = true, implemented = true },
    mouse_button = { implemented = true },
    mouse_move = { implemented = true },
    mouse_scroll = { implemented = true },
}

local function deep_copy(value)
    if type(value) ~= "table" then
        return value
    end
    local copy = {}
    for key, item in pairs(value) do
        copy[deep_copy(key)] = deep_copy(item)
    end
    return copy
end

local function is_array(value)
    if type(value) ~= "table" then
        return false
    end
    local count = 0
    for key, item in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 or type(item) ~= "string" or item == "" then
            return false
        end
        if key > count then
            count = key
        end
    end
    if count == 0 then
        return false
    end
    for index = 1, count do
        if value[index] == nil then
            return false
        end
    end
    return true
end

local function normalize_key_name(value)
    if type(value) ~= "string" then
        return value
    end
    if #value == 1 and value:match("%l") then
        return string.upper(value)
    end
    if value:match("^[%a_]+$") then
        return string.upper(value)
    end
    return value
end

local function normalize_keyboard_key_action(key)
    if type(key) == "string" and #key == 1 and key:match("%u") then
        return {
            type = "keyboard_combo",
            keys = { "SHIFT", key },
        }
    end
    return {
        type = "keyboard_key",
        key = normalize_key_name(key),
    }
end

local function normalize_legacy_keyboard(action)
    if not is_array(action.keys) then
        return nil, "keyboard action requires non-empty keys array"
    end
    if #action.keys == 1 then
        return normalize_keyboard_key_action(action.keys[1])
    end
    local keys = deep_copy(action.keys)
    for index, key in ipairs(keys) do
        keys[index] = normalize_key_name(key)
    end
    return {
        type = "keyboard_combo",
        keys = keys,
    }
end

function M.normalize_action(action)
    if type(action) ~= "table" then
        return nil, "action must be a table"
    end

    local normalized = deep_copy(action)
    if type(normalized.type) ~= "string" or normalized.type == "" then
        return nil, "action.type must be a non-empty string"
    end
    if not SCHEMA[normalized.type] then
        return nil, "unsupported BLE HID action type: " .. normalized.type
    end

    if normalized.type == "keyboard" then
        return normalize_legacy_keyboard(normalized)
    end
    if normalized.type == "media" and normalized.gesture == nil then
        normalized.gesture = "single"
    elseif normalized.type == "keyboard_key" then
        return normalize_keyboard_key_action(normalized.key)
    elseif normalized.type == "keyboard_combo" and type(normalized.keys) == "table" then
        for index, key in ipairs(normalized.keys) do
            normalized.keys[index] = normalize_key_name(key)
        end
    elseif normalized.type == "mouse_button" then
        normalized.button = normalized.button or "left"
        normalized.gesture = normalized.gesture or "click"
    elseif normalized.type == "mouse_move" then
        normalized.dx = normalized.dx or normalized.x or 0
        normalized.dy = normalized.dy or normalized.y or 0
        normalized.scale = normalized.scale or 1
    elseif normalized.type == "mouse_scroll" then
        normalized.vertical = normalized.vertical or normalized.wheel or 0
        normalized.horizontal = normalized.horizontal or normalized.pan or 0
    end
    return normalized
end

function M.validate_action(action)
    local normalized, err = M.normalize_action(action)
    if not normalized then
        return nil, err
    end

    local action_type = normalized.type
    if action_type == "media" then
        if type(normalized.key) ~= "string" or normalized.key == "" then
            return nil, "media action requires string key"
        end
        if type(normalized.gesture) ~= "string" or normalized.gesture == "" then
            return nil, "media action gesture must be a string"
        end
    elseif action_type == "keyboard_key" then
        if type(normalized.key) ~= "string" or normalized.key == "" then
            return nil, "keyboard_key action requires string key"
        end
    elseif action_type == "keyboard_combo" then
        if not is_array(normalized.keys) then
            return nil, "keyboard_combo action requires non-empty keys array"
        end
    elseif action_type == "keyboard_text" then
        if type(normalized.text) ~= "string" then
            return nil, "keyboard_text action requires string text"
        end
    elseif action_type == "mouse_button" then
        if normalized.button ~= nil and type(normalized.button) ~= "string" then
            return nil, "mouse_button action button must be a string"
        end
        if normalized.gesture ~= nil and type(normalized.gesture) ~= "string" then
            return nil, "mouse_button action gesture must be a string"
        end
    elseif action_type == "mouse_move" then
        if normalized.dx ~= nil and type(normalized.dx) ~= "number" then
            return nil, "mouse_move action dx must be a number"
        end
        if normalized.dy ~= nil and type(normalized.dy) ~= "number" then
            return nil, "mouse_move action dy must be a number"
        end
        if normalized.scale ~= nil and type(normalized.scale) ~= "number" then
            return nil, "mouse_move action scale must be a number"
        end
    elseif action_type == "mouse_scroll" then
        if normalized.vertical ~= nil and type(normalized.vertical) ~= "number" then
            return nil, "mouse_scroll action vertical must be a number"
        end
        if normalized.horizontal ~= nil and type(normalized.horizontal) ~= "number" then
            return nil, "mouse_scroll action horizontal must be a number"
        end
    else
        return nil, "unsupported BLE HID action type: " .. tostring(action_type)
    end

    return normalized
end

function M.is_implemented(action_type)
    return SCHEMA[action_type] and SCHEMA[action_type].implemented == true
end

function M.action_types()
    local types = {}
    for action_type, _ in pairs(SCHEMA) do
        types[#types + 1] = action_type
    end
    table.sort(types)
    return types
end

local function require_ble_hid()
    local ok, ble_hid = pcall(require, "ble_hid")
    if not ok then
        return nil, "require ble_hid failed: " .. tostring(ble_hid)
    end
    return ble_hid
end

function M.run(action)
    local normalized, err = M.validate_action(action)
    if not normalized then
        return nil, err
    end
    if not M.is_implemented(normalized.type) then
        return nil, normalized.type .. " BLE HID actions are not implemented yet"
    end

    local ble_hid
    ble_hid, err = require_ble_hid()
    if not ble_hid then
        return nil, err
    end

    if normalized.type == "media" then
        return ble_hid.media(normalized.key, normalized.gesture)
    elseif normalized.type == "keyboard_key" then
        return ble_hid.key(normalized.key)
    elseif normalized.type == "keyboard_combo" then
        local unpack_fn = table.unpack or unpack
        return ble_hid.combo(unpack_fn(normalized.keys))
    elseif normalized.type == "keyboard_text" then
        return ble_hid.text(normalized.text)
    elseif normalized.type == "mouse_button" then
        return ble_hid.mouse_button(normalized.button, normalized.gesture)
    elseif normalized.type == "mouse_move" then
        return ble_hid.mouse_move((normalized.dx or 0) * (normalized.scale or 1),
            (normalized.dy or 0) * (normalized.scale or 1))
    elseif normalized.type == "mouse_scroll" then
        return ble_hid.mouse_scroll(normalized.vertical or 0, normalized.horizontal or 0)
    end

    return nil, "unsupported BLE HID action type: " .. tostring(normalized.type)
end

return M
