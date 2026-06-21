local pet = require("pet")

local list = pet.list()
local current = pet.current()

if #list == 0 then
    print("No downloadable pets installed")
    return
end

for _, item in ipairs(list) do
    local mark = item.id == current and "*" or " "
    local meta = ""
    if item.title and item.title ~= "" then
        meta = meta .. " title=" .. item.title
    end
    if item.profile and item.profile ~= "" then
        meta = meta .. " profile=" .. item.profile
    end
    print(string.format("%s %s - %s [%s]%s", mark, item.id, item.name or item.id, item.asset or "", meta))
end
