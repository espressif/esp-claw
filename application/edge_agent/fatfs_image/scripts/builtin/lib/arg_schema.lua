local M = {}

local function clamp(value, min_value, max_value)
    if min_value ~= nil and value < min_value then
        return min_value
    end
    if max_value ~= nil and value > max_value then
        return max_value
    end
    return value
end

local function copy_object(value)
    if type(value) ~= "table" then
        return value
    end

    local cloned = {}
    for key, item in pairs(value) do
        cloned[key] = copy_object(item)
    end
    return cloned
end

local function normalize_with_spec(spec, value)
    return spec.normalize(value, spec)
end

function M.int(options)
    local spec = options or {}
    spec.normalize = function(value, current)
        if type(value) ~= "number" then
            return current.default
        end

        local normalized = current.floor == false and value or math.floor(value)
        return clamp(normalized, current.min, current.max)
    end
    return spec
end

function M.bool(options)
    local spec = options or {}
    spec.normalize = function(value, current)
        if type(value) == "boolean" then
            return value
        end
        return current.default
    end
    return spec
end

function M.object(options)
    local spec = options or {}
    local fields = spec.fields or {}

    spec.normalize = function(value, current)
        local source = type(value) == "table" and value or {}
        local normalized = {}

        for key, field_spec in pairs(fields) do
            normalized[key] = normalize_with_spec(field_spec, source[key])
        end

        for key, default_value in pairs(current.default or {}) do
            if normalized[key] == nil then
                normalized[key] = copy_object(default_value)
            end
        end

        return normalized
    end
    return spec
end

function M.parse(raw_args, schema)
    local source = type(raw_args) == "table" and raw_args or {}
    local normalized = {}

    for key, spec in pairs(schema) do
        normalized[key] = normalize_with_spec(spec, source[key])
    end

    return normalized
end

return M
