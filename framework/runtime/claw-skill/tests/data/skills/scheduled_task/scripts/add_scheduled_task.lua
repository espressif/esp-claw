local arg_schema = require("arg_schema")
local capability = require("capability")
local json = require("json")

local ARG_SCHEMA = {
    enabled = arg_schema.bool({ default = true }),
    interval_ms = arg_schema.int({ default = 0, min = 0 }),
    max_runs = arg_schema.int({ default = 0, min = 0 }),
    trigger_count = arg_schema.int({ default = -1, min = 0 }),
}

local raw_args = type(args) == "table" and args or {}
local ctx = arg_schema.parse(raw_args, ARG_SCHEMA)
ctx.script_args = type(raw_args.script_args) == "table" and raw_args.script_args or {}

local VALID_MODES = {
    wake_agent = true,
    send_message = true,
    run_script = true,
}

local VALID_KINDS = {
    cron = true,
    interval = true,
}

local function raw_string(key)
    local value = raw_args[key]
    if type(value) == "string" then
        return value
    end
    return ""
end

local function require_string(key)
    local value = raw_string(key)
    if value == "" then
        error("args." .. key .. " is required")
    end
    return value
end

local function validate_task_id(task_id)
    if not string.match(task_id, "^[a-z0-9_-]+$") then
        error("args.task_id must use lowercase letters, digits, underscore, or hyphen")
    end
end

local function validate_cron_expr(expr)
    local count = 0

    for _ in string.gmatch(expr, "%S+") do
        count = count + 1
    end

    if count ~= 5 then
        error("args.cron_expr must be a 5-field cron expression")
    end
end

local function validate_script_path(path)
    if string.sub(path, 1, 1) ~= "/" then
        error("args.script_path must be an absolute path")
    end
    if string.find(path, "%.%.", 1, true) then
        error("args.script_path must not contain ..")
    end
    if not string.match(path, "%.lua$") then
        error("args.script_path must end with .lua")
    end
end

local function cap_call_required(name, payload, label)
    local ok, out, err = capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })

    if not ok then
        local message = string.format(
            "%s failed: err=%s out=%s",
            label,
            tostring(err),
            tostring(out))
        print("[scheduled_task] " .. message)
        error(message)
    end

    return out
end

local function cap_call_best_effort(name, payload, label)
    local ok, out, err = capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })

    if ok then
        return true, out
    end

    return false, string.format("%s failed: err=%s out=%s", label, tostring(err), tostring(out))
end

local function build_schedule(task_id, kind, mode, text)
    local max_runs = ctx.trigger_count >= 0 and ctx.trigger_count or ctx.max_runs
    local schedule = {
        id = task_id,
        enabled = ctx.enabled,
        kind = kind,
        start_at_ms = 0,
        end_at_ms = 0,
        interval_ms = 0,
        cron_expr = "",
        event_type = "schedule",
        event_key = task_id,
        source_channel = "time",
        chat_id = "",
        content_type = "trigger",
        session_policy = mode == "wake_agent" and "chat" or "trigger",
        text = text,
        payload_json = "{}",
        max_runs = max_runs,
    }

    if mode == "wake_agent" then
        schedule.event_type = "message"
        schedule.event_key = "text"
        schedule.source_channel = require_string("chat_channel")
        schedule.chat_id = require_string("chat_id")
        schedule.content_type = "text"
        schedule.session_policy = "chat"
    end

    if kind == "cron" then
        schedule.cron_expr = require_string("cron_expr")
        validate_cron_expr(schedule.cron_expr)
    elseif kind == "interval" then
        if ctx.interval_ms <= 0 then
            error("args.interval_ms must be greater than 0 when kind is interval")
        end
        schedule.interval_ms = ctx.interval_ms
    else
        error("args.kind must be cron or interval")
    end

    return schedule
end

local function build_router_rule(task_id, mode, text)
    local rule = {
        enabled = true,
        consume_on_match = false,
        match = {
            event_type = "schedule",
            event_key = task_id,
        },
        actions = {},
    }

    if mode == "send_message" then
        rule.id = task_id .. "_notify"
        rule.description = "Send a fixed IM message for scheduled task " .. task_id .. "."
        rule.actions[1] = {
            type = "send_message",
            input = {
                channel = require_string("chat_channel"),
                chat_id = require_string("chat_id"),
                message = text,
            },
        }
    elseif mode == "run_script" then
        local script_path = require_string("script_path")
        validate_script_path(script_path)

        rule.id = task_id .. "_run"
        rule.description = "Run a Lua script for scheduled task " .. task_id .. "."
        rule.actions[1] = {
            type = "run_script",
            input = {
                path = script_path,
                args = ctx.script_args,
            },
        }
    else
        error("args.mode must be wake_agent, send_message, or run_script")
    end

    return rule
end

local function run()
    local task_id = require_string("task_id")
    local kind = require_string("kind")
    local mode = require_string("mode")
    local text = raw_string("text")
    local schedule
    local rule
    local router_out
    local scheduler_ok
    local scheduler_out
    local scheduler_err

    validate_task_id(task_id)

    if not VALID_KINDS[kind] then
        error("args.kind must be cron or interval")
    end
    if not VALID_MODES[mode] then
        error("args.mode must be wake_agent, send_message, or run_script")
    end
    if (mode == "wake_agent" or mode == "send_message") and text == "" then
        error("args.text is required when mode is " .. mode)
    end
    if mode == "run_script" and text == "" then
        text = task_id
    end

    schedule = build_schedule(task_id, kind, mode, text)

    if mode ~= "wake_agent" then
        rule = build_router_rule(task_id, mode, text)

        router_out = cap_call_required("add_router_rule", {
            rule_json = json.encode(rule),
        }, "add_router_rule")
    end

    scheduler_ok, scheduler_out, scheduler_err = capability.call("scheduler_add", {
        schedule_json = json.encode(schedule),
    }, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })

    if not scheduler_ok then
        local rollback_ok = true
        local rollback_out = "no router rule created"

        if rule then
            rollback_ok, rollback_out = cap_call_best_effort("delete_router_rule", {
                id = rule.id,
            }, "delete_router_rule rollback")
        end

        local message = string.format(
            "scheduler_add failed: err=%s out=%s; rollback_ok=%s rollback=%s",
            tostring(scheduler_err),
            tostring(scheduler_out),
            tostring(rollback_ok),
            tostring(rollback_out))
        print("[scheduled_task] " .. message)
        error(message)
    end

    print(string.format(
        "[scheduled_task] added task_id=%s mode=%s kind=%s scheduler_id=%s router_rule_id=%s",
        task_id,
        mode,
        kind,
        schedule.id,
        rule and rule.id or "im_any_message_agent"))
    if router_out then
        print("[scheduled_task] router=" .. tostring(router_out))
    else
        print("[scheduled_task] router=reused im_any_message_agent")
    end
    print("[scheduled_task] scheduler=" .. tostring(scheduler_out))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
