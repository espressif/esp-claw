local bridge = ...
local json
local function call(method, args, pcm)
    json = json or require("json")
    local result = json.decode(bridge(method, json.encode(args), pcm))
    if result.error then return nil, result.error end
    return result.value
end
local outputs = setmetatable({}, {__mode = "k"})
local M = {}
function M.open_output(opts)
    assert(opts == nil or type(opts) == "table", "output options must be a table")
    local id, err = call("openOutput", {})
    if not id then return nil, err end
    local output = {}
    outputs[output] = id
    function output:info() return call("outputInfo", {id}) end
    function output:status() return call("outputStatus", {id}) end
    function output:get_volume() return call("getVolume", {id}) end
    function output:set_volume(value) return call("setVolume", {id, value}) end
    function output:set_mute(value) assert(type(value) == "boolean"); return call("setMute", {id, value}) end
    function output:write(pcm) assert(type(pcm) == "string"); return call("write", {id}, pcm) end
    function output:close() return call("closeOutput", {id}) end
    return output
end
local function create(opts, sample)
    assert(type(opts) == "table" and outputs[opts.output], "an audio output is required")
    local config = {}
    for k, v in pairs(opts) do if k ~= "output" then config[k] = v end end
    local id, err = call(sample and "createSampler" or "createPlayer", {outputs[opts.output], config})
    if not id then return nil, err end
    local player = {}
    -- Keep the output alive for the player's lifetime.
    player.output = opts.output
    function player:close() return call(sample and "closeSampler" or "closePlayer", {id}) end
    if not sample then
        function player:play(path, options)
            local ok, message = call("play", {id, path})
            if not ok or not options or not options.wait then return ok, message end
            local status
            repeat require("delay").delay_ms(10); status = assert(self:poll()) until not status.running
            if status.state == "error" then return nil, status.error end
            return true
        end
        function player:stop() return call("stop", {id}) end
        function player:pause() return call("pause", {id}) end
        function player:resume() return call("resume", {id}) end
        function player:poll() return call("poll", {id}) end
    else
        function player:load(sample_id, pcm) assert(type(pcm) == "string"); return call("load", {id, sample_id}, pcm) end
        function player:start() return call("start", {id}) end
        function player:trigger(sample_id, gain, delay_ms, duration_ms, rate)
            local voice, message = call("trigger", {id, sample_id, gain or 1, delay_ms or 0, duration_ms or 0, rate or 1})
            if not voice then return nil, message end
            return {player = self, id = voice}
        end
        function player:stop(voice)
            assert(type(voice) == "table" and voice.player == self, "voice belongs to another player")
            return call("stopVoice", {id, voice.id})
        end
        function player:clear() return call("clear", {id}) end
        function player:status() return call("sampleStatus", {id}) end
    end
    return player
end
function M.player(opts) return create(opts, false) end
function M.sample_player(opts) return create(opts, true) end
return M
