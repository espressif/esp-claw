-- ESP-NOW bring-up and self-test.
--
-- Verifies init, version, peer management, broadcast send, event processing,
-- encrypted-peer (lmk) handling, stats, and deinit. Runs on a single board:
-- the send-complete path is asserted (broadcast link status is always success),
-- while received frames are informational and only appear when a second board
-- is broadcasting on the same channel.

local LOG_PREFIX = "[espnow_test]"
local MESSAGE = "ping from esp-claw"
local POLL_MS = 100
local LOOP_CYCLES = 50

-- Fake unicast peer used to exercise peer-table and encryption handling. The
-- locally-administered bit (0x02) keeps it out of any real device's range; we
-- never transmit to it, so no second board is required.
local TEST_PEER_MAC = string.char(0x02, 0x00, 0x00, 0x00, 0x00, 0x01)
local TEST_PMK = "pmk_16byte_key!!" -- exactly 16 bytes
local TEST_LMK = "1234567890abcdef" -- exactly 16 bytes

local delay_ok, delay = pcall(require, "delay")
local esp_now = require("esp_now")

local recv_count = 0
local send_count = 0
local initialized = false

local function log(msg)
    print(LOG_PREFIX .. " " .. msg)
end

local function assert_ok(ok, err)
    if not ok then
        error(LOG_PREFIX .. " " .. tostring(err))
    end
    return ok
end

local function sleep_ms(ms)
    if delay_ok and delay and type(delay.delay_ms) == "function" then
        delay.delay_ms(ms)
        return
    end
    if os and type(os.clock) == "function" then
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do end
    end
end

local function mac_to_hex(mac)
    if type(mac) ~= "string" or #mac ~= 6 then
        return "??"
    end
    return string.format("%02x:%02x:%02x:%02x:%02x:%02x",
        mac:byte(1), mac:byte(2), mac:byte(3),
        mac:byte(4), mac:byte(5), mac:byte(6))
end

local function run()
    -- init
    assert_ok(esp_now.init())
    initialized = true
    log("init ok; version=" .. tostring(esp_now.get_version()))

    -- radio helpers
    local local_mac = assert_ok(esp_now.get_mac())
    assert(#local_mac == 6, "get_mac must return a 6-byte string")
    log("local mac=" .. mac_to_hex(local_mac))
    local channel = assert_ok(esp_now.get_channel())
    log("current channel=" .. tostring(channel))

    -- events
    assert_ok(esp_now.on_event(function(ev)
        if ev.type == "recv" then
            recv_count = recv_count + 1
            log("recv from=" .. mac_to_hex(ev.src_mac)
                .. " rssi=" .. tostring(ev.rssi)
                .. " data=" .. tostring(ev.data))
        elseif ev.type == "send" then
            send_count = send_count + 1
            log("send success=" .. tostring(ev.success))
        end
    end))

    -- peer management
    assert_ok(esp_now.add_peer({ peer_addr = esp_now.BROADCAST_MAC, channel = 0 }))
    assert(esp_now.peer_exists(esp_now.BROADCAST_MAC), "broadcast peer should exist")

    local num = assert_ok(esp_now.get_peer_num())
    log("peer_num total=" .. tostring(num.total) .. " encrypt=" .. tostring(num.encrypt))

    local peers = esp_now.fetch_peers()
    log("fetch_peers count=" .. tostring(#peers))

    local peer = assert_ok(esp_now.get_peer(esp_now.BROADCAST_MAC))
    log("broadcast peer channel=" .. tostring(peer.channel) .. " ifidx=" .. tostring(peer.ifidx))

    -- send
    assert_ok(esp_now.send(esp_now.BROADCAST_MAC, MESSAGE))

    -- event loop
    for _ = 1, LOOP_CYCLES do
        esp_now.process_events(POLL_MS)
        sleep_ms(10)
    end

    log("totals: recv=" .. tostring(recv_count) .. " send=" .. tostring(send_count))
    assert(send_count >= 1, "expected at least one send-complete event")

    -- encrypted peer: setting lmk must force encrypt=true regardless of the
    -- encrypt flag. A PMK is set first so the encrypted peer is always accepted.
    assert_ok(esp_now.set_pmk(TEST_PMK))
    local before = assert_ok(esp_now.get_peer_num())
    assert_ok(esp_now.add_peer({
        peer_addr = TEST_PEER_MAC,
        channel = 0,
        lmk = TEST_LMK,
        encrypt = false,
    }))
    local secure_peer = assert_ok(esp_now.get_peer(TEST_PEER_MAC))
    assert(secure_peer.encrypt == true, "lmk should force encrypt=true")

    local after = assert_ok(esp_now.get_peer_num())
    assert(after.total == before.total + 1, "total peer count should increase by 1")
    assert(after.encrypt == before.encrypt + 1, "encrypt peer count should increase by 1")

    assert_ok(esp_now.del_peer(TEST_PEER_MAC))
    assert(not esp_now.peer_exists(TEST_PEER_MAC), "test peer should be removed")

    -- stats
    local stats = assert_ok(esp_now.stats())
    log("stats inited=" .. tostring(stats.inited)
        .. " callback_set=" .. tostring(stats.callback_set)
        .. " send_count=" .. tostring(stats.send_count)
        .. " max_data_len=" .. tostring(stats.max_data_len))
    assert(stats.inited == true, "stats.inited should be true")
    assert(stats.callback_set == true, "stats.callback_set should be true")
    assert(stats.send_count >= 1, "stats.send_count should be >= 1")

    -- cleanup
    assert_ok(esp_now.del_peer(esp_now.BROADCAST_MAC))
    assert_ok(esp_now.deinit())
    initialized = false
    log("Test Passed.")
end

-- Guarantees the driver is torn down even if an assertion fails mid-run, so a
-- failed run does not leave ESP-NOW initialized with stale peers behind.
local function cleanup()
    if not initialized then
        return
    end
    pcall(esp_now.del_peer, TEST_PEER_MAC)
    pcall(esp_now.del_peer, esp_now.BROADCAST_MAC)
    pcall(esp_now.deinit)
    initialized = false
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    log("ERROR: " .. tostring(err))
    error(err)
end
