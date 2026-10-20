local storage = require("storage")
local data = storage.get_root_dir()
local root = storage.join_path(data, "storage_tree_test")
assert(not storage.exists(root), "test directory already exists; inspect it before retrying")
assert(storage.mkdir(root))

local function rejects(path, code)
    local ok, err = storage.remove_tree(path)
    assert(ok == nil and err.code == code, err and err.message)
    assert(type(err.errno) == "number" and err.operation == "remove_tree")
    assert(type(err.path) == "string" and type(err.message) == "string")
end

for _, path in ipairs({ "/", data, data .. "/", "/system/apps", root .. "/..", root .. "/.", root .. "//child", root .. "/" }) do
    rejects(path, "EINVAL")
end
assert(not pcall(storage.remove_tree, root .. "\0ignored"))
assert(storage.exists(root))
rejects(root .. "/" .. string.rep("x", 1024), "ENAMETOOLONG")
rejects(data .. "_other/child", "EINVAL")
assert(storage.remove_tree(root .. "/missing"))
assert(storage.remove_tree(root .. "/missing/child"))
local info, message, err = storage.stat(root .. "/missing")
assert(info == nil and type(message) == "string" and err.code == "ENOENT")

for i = 1, 12 do
    local dir = root .. "/dir" .. i
    assert(storage.mkdir(dir))
    assert(storage.write_file(dir .. "/data", "payload"))
end
assert(storage.remove_tree(root))
assert(not storage.exists(root))
assert(storage.remove_tree(root))
assert(storage.mkdir(root))
local deep = root
for i = 1, 16 do deep = deep .. "/d"; assert(storage.mkdir(deep)) end
rejects(root, "ELOOP")
assert(storage.exists(deep))
assert(storage.remove_tree(root .. "/d"))
assert(storage.remove_tree(root))
print("PASS: remove_tree nested cleanup, retries, root/path guards, depth limit, structured stat errors")
