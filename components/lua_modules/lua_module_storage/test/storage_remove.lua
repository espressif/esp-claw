local storage = require("storage")
local root = storage.join_path(storage.get_root_dir(), "storage_remove_test")
assert(not storage.exists(root), "test directory already exists; inspect it before retrying")
assert(storage.mkdir(root))

local function rejects(path)
    local ok, err = pcall(storage.remove, path)
    assert(not ok and type(err) == "string", "remove must raise an error")
end

local file = storage.join_path(root, "file.txt")
assert(storage.write_file(file, "keep"))
assert(storage.remove(file))
assert(not storage.exists(file))
rejects(file)

local empty = storage.join_path(root, "empty")
assert(storage.mkdir(empty))
assert(storage.remove(empty))
assert(not storage.exists(empty))

assert(storage.write_file(file, "keep"))
rejects(file .. "\0suffix")
assert(storage.read_file(file) == "keep")
assert(storage.remove(file))

local nested = storage.join_path(root, "nested")
local child = storage.join_path(nested, "child")
local payload = storage.join_path(child, "payload.txt")
assert(storage.mkdir(nested))
assert(storage.mkdir(child))
assert(storage.write_file(payload, "payload"))
rejects(nested)
rejects(child)
assert(storage.read_file(payload) == "payload")

-- Exercise the same post-order deletion used by App Store staging cleanup.
local function remove_tree(path)
    local info = assert(storage.stat(path))
    if info.type == "dir" then
        for _, entry in ipairs(storage.listdir(path)) do remove_tree(storage.join_path(path, entry.name)) end
    end
    assert(storage.remove(path))
end
remove_tree(nested)
assert(not storage.exists(nested))
assert(storage.mkdir(nested))
remove_tree(nested)
assert(storage.remove(root))
print("PASS: storage.remove files, empty directories, nested cleanup, non-empty rejection, missing paths, NUL rejection")
