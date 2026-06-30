local image   = require("image")
local storage = require("storage")

local TAG = "[image_file_convert_test]"
local ROOT = storage.get_root_dir()
local INPUT_PATH = storage.join_path(ROOT, "image_module_input.jpg")
local COPY_PATH = storage.join_path(ROOT, "image_module_copy.jpg")
local BAD_PATH = storage.join_path(ROOT, "image_module_bad.txt")

local JPEG_BASE64 = [[
/9j/4AAQSkZJRgABAQEAeAB4AAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAEAAAAAAAD/7AARRHVja3kAAQAEAAAAZAAA
/9sAQwACAQECAQECAgICAgICAgMFAwMDAwMGBAQDBQcGBwcHBgcHCAkLCQgICggHBwoNCgoLDAwMDAcJDg8NDA4LDAwM/9sAQwEC
AgIDAwMGAwMGDAgHCAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwM/8AAEQgAKABpAwEi
AAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNR
YQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6
g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/E
AB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRC
kaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJ
ipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/aAAwDAQACEQMR
AD8A99+Nn/B1lZ+E/ijrWm+CfhGnijwzYXL29nqt74hNhJfqjFfNEK28m1Gxlctu2kZAPA5b/iLS17/ogul/+Fi//wAh18e6x/wR
b+Nlnq93F5vwxk8qZ03p480pVbDEZAaZWAPowB9QDxXhX7R/7LnjX9k/xta6D420uCxur+0W/sZ7W9hvrO/t2ZlEsM8LNHIu5WU4
OQykEA1nSxFCo+WnNSa7NP8AIuthMVRip1qcop7NxaT+9H6b/wDEWlr3/RBdL/8ACxf/AOQ69S+HX/BxP8Rtd+FsfxQ1z9mHWrP4
O2epLp+reJ9N8QfaxZksEJSN7eMPh2VcllTcQm8MQK/DWvTrf9tL4oaV+ypqHwXt/F19H8M9Quvt0+i+VEUaQOJdok2+YIzIFkMY
YIXG7GSSejlRzKTP6xPDfiGz8XeHrDVtPmW40/VLeO7tpQCBLFIodGAPPKkHn1q9XD/sy/8AJt3w9/7FrTv/AEljr4T/AGsv+Cj3
xY+E/wDwWs+G/wAE9D1fSrf4f+JJNJF9aS6ZHJO4neQSgSn51JCjBB4/nmjTm0P0korwX/gpp49+I3wm/Yl8deMPhXqFtYeLvCNn
/bKfaLJLyO4tYGD3KbHBGfIEjAgZygHc15r/AMER/wBvrXP+CgH7IM2u+L7qyufGvhzWbjStWe2gW3WZTtmglEa8KDFIE46tExo8
x31sfYlFfmD+29/wU1+MUv8AwV48H/s6/BvWtI03T5306x1uabSYr6SOebddXMoL9FisijbRj5lfJ7B//BQ7/gsv8Sv+GtD+z3+z
D4YtfFHj6zmaz1PU5oFuhFcqm+SCBGZYlEIz5s0x2KwZdo2li+UXMj9O6K/G3xx/wUr/AG4/+CZXibw/rP7RHhXQ/F3gLXLtbWSa
2hs42ViC5hiuLMhI59iuyrMhVwjYPBZfoL/gsH/wVF8UfAf9iD4V/Fj4H69p6WfxB1SHyrq60+O6820lsppgpSTOxwyAMOqlSpxg
iiwcx+iFFcP+zP411L4k/s4fD/xFrEsU2r+IPDenalfSRR+Ukk81rHJIyqOFBZiQB0FdxSKPwXnX9++0fLuOMV5F/wAFgpVHw+/Z
1TcPl8K6oduemdZuu34fpX6IX/xbhu76aaTwZ8OTJLIzsf8AhFNPbJJyeWhJP4kmuG/4KDfH/wD4Rnwj8I4T4D+FGrQz6Jd3CJq3
gzT79bQ/bZYysKyRlYkIRSQgUFsk54x+Y+B/AOIzHiV4HL6qlUnSnZSTirJwk3f3u21up9t4gfSUyLjHLI5dhMNVpypzVRuXK00l
KFlZ73qJ+iZ+JoYN3plx/wAe8n+6a/TGW78N/tR/Ab4yab4g+G/wt0uTwv4D1HxNpV/4d8J2Wi39ne2hiaNhNbxqzRncVeNsqyno
CAR+Z1x/x7yf7pr904o4YxuQY95dj+XnSUvdd1Z7atL8j81y/H0sZS9vRva9tdNj+uv9mX/k274e/wDYtad/6Sx1+UH7fX/Ky/8A
Bn/f0H/0Oav1f/Zl/wCTbvh7/wBi1p3/AKSx1+bf/BUT/gnX+0h8Uf8Agpzonxs+DWg6HeL4Z0/T2067vtTtY1F1AZCd0MrAkDcO
2D/L5mJ6L2R+qesaTa+INIurC+hjurO+he3uIZBlJY3BVlI9CCR+Nfit/wAEa/Fi/wDBNj/gqP8AHT4G+JbuS10CS0vJ7aWU8uNN
D3dvNj/b0+SaQ9/lH4fR3wK1D/gpNJ8bvB6+OrH4fp4JbWrMeIGgOneaun+cv2nZsctu8rfjbznFcL/wW7/4JAfFX9qf9qvQ/iZ8
GdPtJ7zUtDOma+f7Vi06VZYt0aSbnZS4kt5fKYDPyw4PDc15A77o4L/g3y8FX37YH/BQH40ftKeILZ28q4uE0/zORFd6hK0jKh/6
Y2qCLHZZxVb/AIN6fs8v/BUn9oh9e2/8Jj5OolfO/wBdg6t/peM8/wCs8nPfpX6Af8Eev2JtQ/YM/Yd0Hwfr9ta23i/ULq41jxAt
vKsyC6mfCoHXhtkEcEZIJBKEjg18v/8ABQX/AII3fFLRf2vZP2hv2W/EVroPjbUJ2vdT0eWdLUtdOu2aaB5AYXWfrJDMApYu247t
qm4uXQ1P+Cn3/BRX49/s2eJPG66h+zj4T8WfBjw3eWwt/EGuI09rch/KWORkLFc+fLsBC8HH1r5w/wCCwHx2m/aZ/wCCK37PHjmf
w3ofhF9e8UTyLpGjxmOxs0SPUIkESkDAKoGxjGWPXrXX+Pf2CP28/wDgp1qGi+FvjzrWheA/h5p94lzdpBJZN5rLkeasFozmaUAn
aJXSME5GO/vf/BYP/gl54q+OH7Dfwo+EvwR0C1vLT4e6nCEt7vUYrYx2kVlNAHZ5CodyzKWPUlicdaNA1Psn9i7/AJM6+E3/AGJu
j/8ApDDXplfk14E8O/8ABUL4beBdF8O6Tpfw6h0vw/YQabZo8uluyQwxrGgLF+TtUc963v7S/wCCpv8A0D/hf/31p3/xdKxXMfSv
jL/gmc2oeJLqfRvElvZ6bK5eGCe0aSSIH+EsGGceuBmuI/ao/wCCQmrftC6J4Gt7Txxp+mS+E9Nn0+Yzaa8i3G+4ecOuHBXHmFSD
noDx0oop8JzfDWZrOMm/d1kpRv8AErS3XLK6/DTofM0eDcopTnOnSs5qz96W109FfTVLb0OS+Ff/AAQ31nwJ4R+JWmXXxE024bx1
4M1HwtBJDpTr9kkuQm2ZgZPmVdnKjBOeor5z8Ef8Gnuqr4ssG8TfGLT5tBWUG+i0zQ3ju5Yv4ljeSVkRiONzKwHXaelFFe1xBxJm
Gd4v6/mU+ao0leyWi20SSPcweX0MLT9lQVlvu3+Z+ynhnw3Z+D/Den6Tp8P2fT9Lto7S2iBJ8uKNQiLk88KAOavUUV4Z2BRRRQAU
UUUAFFFFABRRRQB//9k=
]]

local BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local base64_map = {}

for i = 1, #BASE64_ALPHABET do
    base64_map[BASE64_ALPHABET:sub(i, i)] = i - 1
end

local function assert_true(value, message)
    if not value then
        error(message, 2)
    end
end

local function base64_decode(input)
    local compact = input:gsub("%s", "")
    local out = {}

    for i = 1, #compact, 4 do
        local c1 = compact:sub(i, i)
        local c2 = compact:sub(i + 1, i + 1)
        local c3 = compact:sub(i + 2, i + 2)
        local c4 = compact:sub(i + 3, i + 3)
        local b1 = base64_map[c1] or 0
        local b2 = base64_map[c2] or 0
        local b3 = c3 == "=" and 0 or base64_map[c3] or 0
        local b4 = c4 == "=" and 0 or base64_map[c4] or 0
        local value = b1 * 262144 + b2 * 4096 + b3 * 64 + b4

        out[#out + 1] = string.char(math.floor(value / 65536) % 256)
        if c3 ~= "=" then
            out[#out + 1] = string.char(math.floor(value / 256) % 256)
        end
        if c4 ~= "=" then
            out[#out + 1] = string.char(value % 256)
        end
    end

    return table.concat(out)
end

local function remove_if_exists(path)
    if storage.exists(path) then
        local ok, err = pcall(storage.remove, path)
        assert_true(ok, "storage.remove failed: " .. tostring(err))
    end
end

local function assert_frame_info(info, format, min_bytes)
    assert_true(type(info) == "table", "frame:info() should return a table")
    assert_true(info.valid == true, "frame should be valid")
    assert_true(info.width == 105 and info.height == 40, "unexpected frame size")
    assert_true(info.pixel_format == format, "unexpected frame format: " .. tostring(info.pixel_format))
    assert_true(info.bytes >= min_bytes, "unexpected frame byte count")
end

remove_if_exists(INPUT_PATH)
remove_if_exists(COPY_PATH)
remove_if_exists(BAD_PATH)

local jpeg_bytes = base64_decode(JPEG_BASE64)
storage.write_file(INPUT_PATH, jpeg_bytes)
storage.write_file(BAD_PATH, "not an image")

local run_ok, run_err = xpcall(function()
    do
        local frame <close> = image.load_file(INPUT_PATH)
        local info = frame:info()
        assert_frame_info(info, "JPEG", #jpeg_bytes)

        local jpeg_frame <close> = image.convert(frame, image.JPEG)
        local jpeg_info = jpeg_frame:info()
        assert_frame_info(jpeg_info, "JPEG", 1)

        local rgb_frame <close> = image.convert(frame, image.RGB565)
        local rgb_info = rgb_frame:info()
        assert_frame_info(rgb_info, "RGBP", rgb_info.width * rgb_info.height * 2)

        local gray_frame <close> = image.convert(frame, image.GRAY8)
        local gray_info = gray_frame:info()
        assert_frame_info(gray_info, "GREY", gray_info.width * gray_info.height)

        frame:release()
        assert_frame_info(rgb_frame:info(), "RGBP", rgb_info.width * rgb_info.height * 2)
        assert_frame_info(gray_frame:info(), "GREY", gray_info.width * gray_info.height)

        image.save_file(COPY_PATH, jpeg_frame)
        local saved = storage.stat(COPY_PATH)
        assert_true(saved ~= nil and saved.size > 0, "saved JPEG should exist and be non-empty")
    end

    -- image.resize: produces an independent frame, default output preserves
    -- the source's logical channels (color -> RGB565, gray stays gray).
    do
        local frame <close> = image.load_file(INPUT_PATH)

        local small <close> = image.resize(frame, { width = 32, height = 16 })
        local small_info = small:info()
        assert_true(small_info.valid == true, "resized frame should be valid")
        assert_true(small_info.width == 32 and small_info.height == 16, "resize default size mismatch")
        assert_true(small_info.pixel_format == "RGBP", "resize default output should be RGB565, got " .. tostring(small_info.pixel_format))
        assert_true(small_info.bytes == 32 * 16 * 2, "resize RGB565 byte count mismatch: " .. tostring(small_info.bytes))

        local gray <close> = image.resize(frame, { width = 20, height = 10, format = image.GRAY8, filter = "bilinear" })
        local gray_info = gray:info()
        assert_true(gray_info.pixel_format == "GREY", "resize GRAY8 format mismatch: " .. tostring(gray_info.pixel_format))
        assert_true(gray_info.width == 20 and gray_info.height == 10, "resize GRAY8 size mismatch")
        assert_true(gray_info.bytes == 20 * 10, "resize GRAY8 byte count mismatch: " .. tostring(gray_info.bytes))

        -- Independent store: releasing the source must not invalidate resized frames.
        frame:release()
        assert_true(small:info().valid == true, "resized RGB565 should outlive source release")
        assert_true(gray:info().valid == true, "resized GRAY8 should outlive source release")

        -- Chained conversion on a resized frame uses the new frame's own cache.
        local thumb_jpeg <close> = image.convert(small, image.JPEG)
        local thumb_info = thumb_jpeg:info()
        assert_true(thumb_info.pixel_format == "JPEG", "convert(small, JPEG) format mismatch")
        assert_true(thumb_info.bytes > 0, "convert(small, JPEG) bytes should be > 0")
    end

    -- Reject unsupported output format and invalid dimensions.
    do
        local frame <close> = image.load_file(INPUT_PATH)
        local bad_fmt = pcall(image.resize, frame, { width = 16, height = 16, format = image.YUYV })
        assert_true(bad_fmt == false, "image.resize should reject non-RGB565/GRAY8 output")
        local bad_dim = pcall(image.resize, frame, { width = 0, height = 16 })
        assert_true(bad_dim == false, "image.resize should reject zero width")
        local bad_filter = pcall(image.resize, frame, { width = 16, height = 16, filter = "bicubic" })
        assert_true(bad_filter == false, "image.resize should reject unknown filter")
    end

    local bad_load_ok = pcall(image.load_file, BAD_PATH)
    assert_true(bad_load_ok == false, "image.load_file should reject unsupported suffix")

    local bad_save_ok = pcall(function()
        local frame <close> = image.load_file(INPUT_PATH)
        image.save_file(BAD_PATH, frame)
    end)
    assert_true(bad_save_ok == false, "image.save_file should reject unsupported suffix")
end, debug.traceback)

remove_if_exists(INPUT_PATH)
remove_if_exists(COPY_PATH)
remove_if_exists(BAD_PATH)

if not run_ok then
    error(run_err)
end

print(TAG .. " PASS")
