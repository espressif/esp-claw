# Lua HTTP

This module provides a minimal HTTP client for Lua scripts running on the device.
It is suitable for short LAN requests (REST probes, webhooks, local API calls).
Response bodies are capped to prevent heap blow-ups (default 8 KB, max 64 KB).

## How to call
- Import it with `local http = require("http")`
- `http.request(opts)` — general request, returns a result table
- `http.get(url[, opts])` — shortcut for `GET`
- `http.post(url, body[, opts])` — shortcut for `POST` with body

### `opts` table fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | (required) | Full URL including scheme |
| `method` | string | `"GET"` | `"GET"`, `"POST"`, `"PATCH"`, `"PUT"`, `"DELETE"`, `"HEAD"` |
| `body` | string | `nil` | Request body (for POST/PUT/PATCH) |
| `headers` | table | `{}` | Key-value pairs added as HTTP headers |
| `timeout_ms` | integer | `3000` | Timeout in milliseconds (100–60000) |
| `max_body_bytes` | integer | `8192` | Maximum bytes to buffer in response body (256–65536) |

### Result table fields

| Field | Type | Description |
|-------|------|-------------|
| `ok` | boolean | `true` if the request completed without a transport error |
| `status` | integer | HTTP status code (0 if transport failed) |
| `body` | string | Response body (may be absent if empty) |
| `truncated` | boolean | `true` if response was larger than `max_body_bytes` |
| `error` | string | Error description (only present when `ok` is `false`) |
| `method` | string | Method that was used |

## Examples

```lua
local http = require("http")

-- Simple GET
local r = http.get("http://192.168.1.100/api/status")
if r.ok and r.status == 200 then
    print(r.body)
end

-- POST with JSON body
local r2 = http.post("http://192.168.1.100/api/command",
    '{"cmd":"reboot"}',
    { headers = { ["Content-Type"] = "application/json" } })
print(r2.ok, r2.status)

-- Full request with custom options
local r3 = http.request({
    url        = "http://192.168.1.100/api/data",
    method     = "PATCH",
    body       = '{"value":42}',
    headers    = { Authorization = "Bearer token123" },
    timeout_ms = 5000,
    max_body_bytes = 4096,
})
if r3.ok then
    print("Response:", r3.body)
end
```
