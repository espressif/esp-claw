local pet = require("pet")

local a = type(args) == "table" and args or {}

local ACTION_ALIASES = {
    ["idle"] = "idle",
    ["jump"] = "jumping",
    ["jumping"] = "jumping",
    ["run"] = "running",
    ["running"] = "running",
    ["runing"] = "running",
    ["run left"] = "running-left",
    ["run-left"] = "running-left",
    ["run_left"] = "running-left",
    ["running left"] = "running-left",
    ["running-left"] = "running-left",
    ["running_left"] = "running-left",
    ["run right"] = "running-right",
    ["run-right"] = "running-right",
    ["run_right"] = "running-right",
    ["running right"] = "running-right",
    ["running-right"] = "running-right",
    ["running_right"] = "running-right",
    ["wave"] = "waving",
    ["waving"] = "waving",
    ["failed"] = "failed",
    ["fail"] = "failed",
    ["lose"] = "failed",
    ["waiting"] = "waiting",
    ["wait"] = "waiting",
    ["review"] = "review",
}

local function normalize_action(value)
    local key = tostring(value or ""):match("^%s*(.-)%s*$"):lower():gsub("%s+", " ")
    if key == "" then
        error("args.action is required")
    end
    return ACTION_ALIASES[key] or key:gsub("_", "-")
end

local action = normalize_action(a.action or a.name)

pet.action(action)
print("Pet action: " .. action)
