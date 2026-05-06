--[[
sd_file_server.lua — SD Card File Server

Usage:
  lua_run_script("sd_file_server.lua")

Browse files at: http://192.168.3.28:8080/
--]]

local srv = require("http_server")
local storage = require("storage")

-- Start server on port 8080
local ok, port = srv.start({port = 8080})
if not ok then
    print("Failed to start server")
    return
end
print("SD Card File Server started on port " .. port)

-- Helper: list directory and build HTML
local function list_dir_html(path)
    local items = storage.listdir(path)
    local html = [[
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>ESP-Claw SD Card</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; padding: 20px; }
h1 { color: #e94560; margin-bottom: 20px; }
table { width: 100%; border-collapse: collapse; }
th { text-align: left; padding: 10px 8px; background: #16213e; color: #e94560; }
td { padding: 8px; border-bottom: 1px solid #333; }
tr:hover td { background: #0f3460; }
a { color: #53d8fb; text-decoration: none; }
a:hover { text-decoration: underline; }
.dir { color: #ffd460; font-weight: bold; }
.size { color: #888; text-align: right; }
.back { display: inline-block; margin-bottom: 15px; color: #53d8fb; }
</style>
</head>
<body>
<h1>📂 SD Card: ]] .. path .. [[</h1>
]]
    if path ~= "/sdcard" then
        local parent = path:match("^(.*)/[^/]+$") or "/sdcard"
        html = html .. [[<a class="back" href="/]] .. parent .. [[">⬆ Back</a><br>]]
    end
    html = html .. [[<table><tr><th>Name</th><th>Size</th></tr>]]

    if items and type(items) == "table" then
        for _, item in ipairs(items) do
            local name = tostring(item)
            -- Filter out garbled entries (non-printable chars or huge sizes)
            local printable = true
            for i = 1, #name do
                local b = name:byte(i)
                if b and (b < 32 or b > 126) then
                    printable = false
                    break
                end
            end
            if printable and #name > 0 and name ~= "." and name ~= ".." then
                local item_path = path .. "/" .. name
                local stat = storage.stat(item_path)
                if stat and stat.is_directory then
                    html = html .. [[<tr><td class="dir">📁 <a href="/]] .. item_path .. [[">]] .. name .. [[</a></td><td class="size">dir</td></tr>]]
                else
                    local size = (stat and stat.size) and tostring(stat.size) or "?"
                    html = html .. [[<tr><td>📄 <a href="/]] .. item_path .. [[">]] .. name .. [[</a></td><td class="size">]] .. size .. [[ B</td></tr>]]
                end
            end
        end
    end

    html = html .. [[</table></body></html>]]
    return html
end

-- Route: Root → redirect to /sdcard
srv.route("GET", "/", function(req)
    return "<html><head><meta http-equiv='refresh' content='0;url=/sdcard'></head><body>Redirecting...</body></html>"
end)

-- Route: Serve files and directories under /sdcard
srv.route("GET", "/sdcard/*", function(req)
    local full_path = req.path

    -- Remove leading /
    local rel_path = full_path:match("^/(.+)$") or full_path

    -- Try as directory first
    local stat = storage.stat(rel_path)
    if stat and stat.is_directory then
        return list_dir_html(rel_path)
    end

    -- Try as file
    local ok, data = pcall(storage.read_file, rel_path)
    if ok and data then
        return data
    end

    return "<h1>404 Not Found</h1><p>File or directory not found: " .. full_path .. "</p>"
end)

print("Routes registered. Server ready.")
