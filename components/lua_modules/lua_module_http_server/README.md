# Lua HTTP Server

This module lets a long-running Lua script publish static DATA files and HTTP callbacks through the existing edge_agent HTTP server.

## API

- `http.app(app_id)` creates an application object. `app_id` may contain only letters, digits, `_`, and `-`.
- `app:mount_static(root_path)` publishes files from an absolute safe directory path and returns `true`.
- `app:get(path, callback)` registers a GET route and returns the app object.
- `app:post(path, callback)` registers a POST route and returns the app object.
- `app:url()` returns the static URL prefix, such as `/lua/panel/`.
- `app:serve_forever()` dispatches callback requests until the Lua job is stopped.

Callback `req` fields:
- `method`: `"GET"` or `"POST"`
- `path`: route path without the `/api/lua/<app_id>` prefix
- `query`: decoded query-string table
- `body`: request body string
- `content_type`: request content type

Callback return values:
- `nil`: HTTP 204
- `string`: HTTP 200 `text/plain; charset=utf-8`
- `{ json = table [, status = code] }`: JSON response
- `{ body = string [, content_type = type] [, status = code] }`: custom body response

## Example

```lua
local http = require("http_server")
local storage = require("storage")
local system = require("system")

local app = http.app("panel")
app:mount_static(storage.join_path(storage.get_root_dir(), "www", "panel"))

app:get("/state", function(req)
  return {
    json = {
      ok = true,
      uptime = system.uptime(),
      method = req.method,
      path = req.path,
    },
  }
end)

app:post("/echo", function(req)
  return { json = { ok = true, body = req.body } }
end)

print(app:url())
app:serve_forever()
```

## URL Space

- Static files: `/lua/<app_id>/...`
- Callback APIs: `/api/lua/<app_id>/...`

## Static Page Rules

Open static pages with the trailing slash, for example `/lua/panel/`.
Without the trailing slash, browsers treat `/lua/panel` as a file path and
resolve relative assets like `./app.js` as `/lua/app.js`, which is invalid
because `app.js` would be parsed as the app id.

For packaged skills with a fixed default `app_id`, prefer absolute asset URLs
in HTML:

```html
<link rel="stylesheet" href="/lua/panel/style.css">
<script src="/lua/panel/app.js"></script>
```

If a skill allows callers to change `app_id`, either keep the documented
trailing-slash URL as the required entry point or update the HTML asset URLs to
match the chosen app id.

## Notes

Run scripts that serve callbacks through `lua_run_script_async`, because `serve_forever()` keeps the Lua state alive until the job is stopped.
