local http = require("http_server")
local system = require("system")

local app = http.app("panel")
app:mount_static("/fatfs/www/panel")

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
