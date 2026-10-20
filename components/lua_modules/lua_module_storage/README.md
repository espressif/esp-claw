# Lua Storage

Filesystem access through `local storage = require("storage")`.

## API

| Function | Behavior / return value |
| --- | --- |
| `get_root_dir()` | Return the writable DATA root path. |
| `join_path(...)` | Join path strings, ignoring empty components and duplicate separators between components. Does not resolve `..` or create directories. |
| `exists(path)` | Return whether the path exists. |
| `stat(path)` | Return `{ type, size, mtime, mode }`, or `nil, err, detail` on failure. |
| `mkdir(path)` | Create a directory; return `true` on success. |
| `write_file(path, content)` | Write file contents; return `true` on success. |
| `read_file(path)` | Return file contents as a string. |
| `listdir(path)` | Return entries with `name`, `type`, and, when available, `size`, `mtime`, and `mode`. |
| `remove(path)` | Delete a file or empty directory; return `true` on success. |
| `remove_tree(path)` | Recursively delete a DATA child path; return `true` if deleted or already missing, or `nil, detail` on failure. |
| `rename(old_path, new_path)` | Rename or move a path; return `true` on success. |
| `get_free_space()` | Return `{ total, free, used }` in bytes for DATA storage. |

## Removal and errors

- `remove()` raises a Lua error on failure, including missing paths and non-empty directories. Use `pcall(storage.remove, path)` to handle it.
- `remove_tree()` accepts only absolute descendants of DATA. The DATA root itself, `.`/`..`, repeated separators, and trailing separators are rejected. Paths must be shorter than 1024 bytes; directory depth is limited to 16 levels, including the target directory.
- Recursive deletion may be partial on failure. Do not modify the target tree concurrently.
- `detail` returned by `stat()` and `remove_tree()` contains `code`, `errno`, `operation`, `path`, and `message`. Invalid arguments may raise Lua errors instead.

## Example

```lua
local storage = require("storage")

local dir = storage.join_path(storage.get_root_dir(), "demo")
local file = storage.join_path(dir, "test.txt")

storage.mkdir(dir)
storage.write_file(file, "hello")
print(storage.read_file(file))
storage.remove(file)
storage.remove(dir)
```
