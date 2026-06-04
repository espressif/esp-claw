---
{
  "name": "cap_http_request",
  "description": "Call allowlisted HTTP or HTTPS endpoints directly and return response status plus body text, or stream the response body to a file.",
  "metadata": {
    "cap_groups": [
      "cap_http_request"
    ],
    "manage_mode": "readonly"
  }
}
---

# HTTP Request

Use this skill when the task needs a direct HTTP or HTTPS request to a known endpoint instead of search-engine results.

## When to use
- The user needs raw API or file content from a specific URL.
- A workflow already knows the target endpoint and only needs a guarded fetch.
- The request should stay inside the device allowlist policy.

## Available capability
- `http_request`: send standard HTTP requests to allowlisted domains or IPs and return status plus body text, or save the response body directly to a file.

## Calling rules
- Only call `http_request` when the target host is expected to be in the configured allowlist.
- Input must be a JSON object:

```json
{
  "url": "https://api.example.com/v1/data",
  "method": "GET",
  "headers": {
    "Authorization": "Bearer <token>"
  },
  "body": "",
  "timeout_ms": 15000,
  "max_body_bytes": 16384,
  "save_path": "/spiffs/download.bin",
  "max_file_bytes": 1048576
}
```

- `url` is required and must start with `http://` or `https://`.
- `method` defaults to `GET`.
- `body` is only valid for methods that accept a request body.
- `save_path` is optional. When present, the response body is streamed directly to this file and is not returned in memory.
- `max_body_bytes` applies only when returning the response body as text.
- `max_file_bytes` is optional and applies only when `save_path` is present.
- Redirects are allowed only when the redirect target also matches the allowlist.

## Output shape
- The capability returns plain text, not JSON.
- Success format:
  - `HTTP <status>`
  - followed by the response body text
- File-save success format:
  - `HTTP <status>`
  - `Saved response body to <path> (<bytes> bytes)`
- Common error strings include:
  - `Error: HTTP allowlist is empty; configure search_http_allowlist first`
  - `Error: host '...' is not in allowlist`
  - `Error: method must be GET/POST/PUT/PATCH/DELETE/HEAD`
  - `Error: save_path must be a non-empty string`
  - `Error: failed to open save_path`
  - `Error: http request failed (...)`

## Examples

```json
{
  "url": "https://skills-lab.esp-claw.com/raw/tags.json"
}
```

```json
{
  "url": "https://api.example.com/v1/items",
  "method": "POST",
  "headers": {
    "Content-Type": "application/json"
  },
  "body": "{\"name\":\"demo\"}"
}
```

```json
{
  "url": "https://example.com/file.bin",
  "save_path": "/spiffs/file.bin",
  "max_file_bytes": 10485760
}
```
