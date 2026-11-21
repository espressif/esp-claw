local http = require("http_server")
local storage = require("storage")
local system = require("system")

local a = type(args) == "table" and args or {}
local web_root = type(a.web_root) == "string" and a.web_root or storage.join_path(storage.get_root_dir(), "www", "panel")

local app = http.app("panel")
app:mount_static(web_root)

app:get("/state", function(req)
    return {
        json = {
            ok = true,
            uptime = system.uptime(),
            method = req.method,
            path = req.path,
            query = req.query,
        },
    }
end)

app:post("/echo", function(req)
    return {
        json = {
            ok = true,
            body = req.body,
            content_type = req.content_type,
        },
    }
end)

print(app:url())
app:serve_forever()
