# Lua Storage

This module describes how to correctly use storage when writing Lua scripts.

## How to call
- Import it with `local storage = require("storage")`
- Call `storage.get_root_dir()` to get the storage root directory
- Call `storage.join_path(...)` to join path segments with one `/`
- Call `storage.exists(path)` to check whether a path exists
- Call `storage.stat(path)` to get path metadata, or `nil, err` if it does not exist
- Call `storage.mkdir(path)` to create a directory
- Call `storage.write_file(path, content)` to write a file
- Call `storage.read_file(path)` to read a file
- Call `storage.listdir(path)` to list directory entries
- Call `storage.remove(path)` to remove a file or empty directory
- Call `storage.rename(old_path, new_path)` to rename or move a path
- Call `storage.get_free_space()` to get `{ total, free, used }` bytes for the storage root

## Return values
- `storage.get_root_dir()` returns the DATA root path string.
- `storage.join_path(...)` returns the joined path string.
- `storage.exists(path)` returns a boolean.
- `storage.stat(path)` returns `{ type, size, mtime, mode }` or `nil, err`.
- `storage.listdir(path)` returns an array of entries. Each entry includes `name`, `type`, and, when available, `size`, `mtime`, and `mode`.
- `storage.mkdir(path)`, `storage.write_file(path, content)`, `storage.remove(path)`, and `storage.rename(old_path, new_path)` return `true` on success.
- `storage.read_file(path)` returns the file contents as a string.
- `storage.get_free_space()` returns `{ total, free, used }`.

## Path joining
- Prefer `storage.join_path(...)` whenever building a path from `storage.get_root_dir()` and child names.
- Pass each path component as a separate string argument, for example `storage.join_path(root, "logs", "today.txt")`.
- `join_path` removes duplicate separators between components, so `storage.join_path(storage.get_root_dir(), "/demo/", "test.txt")` returns `<storage_root>/demo/test.txt`.
- Empty string components are ignored, so optional subdirectories can be passed directly when they may be empty.
- The first component decides whether the result is absolute. Use `storage.get_root_dir()` as the first component for filesystem paths in this demo.
- Do not put multiple logical components in one string when they can be separate arguments; `storage.join_path(root, "demo", filename)` is easier to audit than `storage.join_path(root, "demo/" .. filename)`.
- `join_path` only joins strings. It does not create directories, validate that a path exists, or prevent `..` path traversal.

## Example
```lua
local storage = require("storage")

local root = storage.get_root_dir()
local dir = storage.join_path(root, "demo")
local file = storage.join_path(dir, "test.txt")
local log_file = storage.join_path(root, "logs", "today.txt")

storage.mkdir(dir)
storage.write_file(file, "hello")
local text = storage.read_file(file)

if storage.exists(file) then
    local info = storage.stat(file)
    local entries = storage.listdir(dir)
    local space = storage.get_free_space()
end
```
