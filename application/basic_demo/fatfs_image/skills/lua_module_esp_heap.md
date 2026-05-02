# Lua ESP Heap

This skill describes how to correctly use esp_heap when writing Lua scripts.

## How to call
- Import it with `local esp_heap = require("esp_heap")`
- Read `local caps = esp_heap.caps` to get capability flags such as `caps.DEFAULT`, `caps.INTERNAL`, and `caps.SPIRAM`
- Call `esp_heap.get_info(caps)` to read heap statistics such as `free_size`, `allocated_size`, and `largest_free_block`
- Call `esp_heap.get_task_watermarks()` to inspect stack high-water marks for tasks
- Call `esp_heap.get_current_task()` to inspect the current task state

## Example
```lua
local esp_heap = require("esp_heap")
local caps = esp_heap.caps

local info = esp_heap.get_info(caps.DEFAULT)
print(info.free_size, info.largest_free_block)

local task = esp_heap.get_current_task()
print(task.name, task.stack_high_water_mark_bytes)
```
