local json = require("json")
local storage = require("storage")

local MAX_DIMENSION = 64
local MAX_GLYPHS = 4096
local MAX_BYTES = 1024 * 1024
local MAX_BDF_BYTES = 2 * 1024 * 1024
local NO_FALLBACK = 0xFFFFFFFF

local function fail(line, message)
  if line then error(string.format("BDF line %d: %s", line, message), 0) end
  error(message, 0)
end

local function relative_path(name, value, suffix)
  if type(value) ~= "string" or value == "" then error(name .. " must be a non-empty string", 0) end
  if value:sub(1, 1) == "/" or value:find("\\", 1, true) or ("/" .. value .. "/"):find("/../", 1, true) then
    error(name .. " must be a safe DATA-relative path", 0)
  end
  if value:lower():sub(-#suffix) ~= suffix then error(name .. " must end with " .. suffix, 0) end
  return storage.join_path(storage.get_root_dir(), value)
end

local function valid_codepoint(value)
  return value >= 0 and value <= 0x10FFFF and not (value >= 0xD800 and value <= 0xDFFF)
end

local function integers(line, keyword, count, line_number)
  local rest = line:match("^" .. keyword .. "%s+(.+)$")
  if not rest then return nil end
  local values = {}
  for value in rest:gmatch("[-+]?%d+") do values[#values + 1] = tonumber(value) end
  if #values ~= count then fail(line_number, keyword .. " requires " .. count .. " integers") end
  return table.unpack(values)
end

local function bitmap_row(line, width, line_number)
  local bytes = (width + 7) // 8
  if #line ~= bytes * 2 or not line:match("^[%x]*$") then fail(line_number, "invalid BITMAP row width") end
  if width % 8 ~= 0 and bytes > 0 then
    local unused = 8 - width % 8
    if (tonumber(line:sub(-2), 16) & ((1 << unused) - 1)) ~= 0 then fail(line_number, "non-zero BITMAP padding bits") end
  end
  return (line:gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end))
end

local function parse_bdf(text)
  local font = { glyphs = {}, seen = {}, glyph_count = 0, skipped = 0 }
  local glyph, in_bitmap, saw_start, saw_end
  local line_number = 0
  for raw in (text .. "\n"):gmatch("([^\n]*)\n") do
    line_number = line_number + 1
    local line = raw:gsub("\r$", "")
    if in_bitmap and line ~= "ENDCHAR" then
      glyph.rows[#glyph.rows + 1] = bitmap_row(line, glyph.width, line_number)
    elseif line:match("^STARTFONT%s+") then
      local version = line:match("^STARTFONT%s+(%S+)$")
      if saw_start or (version ~= "2.1" and version ~= "2.2") then fail(line_number, "unsupported STARTFONT") end
      saw_start = true
    elseif line == "ENDFONT" then
      if glyph then fail(line_number, "ENDFONT inside glyph") end
      saw_end = true
    elseif line:match("^FONTBOUNDINGBOX%s+") then
      font.box_width, font.box_height = integers(line, "FONTBOUNDINGBOX", 4, line_number)
    elseif line:match("^FONT_ASCENT%s+") then
      font.ascent = integers(line, "FONT_ASCENT", 1, line_number)
    elseif line:match("^FONT_DESCENT%s+") then
      font.descent = integers(line, "FONT_DESCENT", 1, line_number)
    elseif line:match("^DEFAULT_CHAR%s+") then
      font.default_char = integers(line, "DEFAULT_CHAR", 1, line_number)
    elseif line:match("^CHARSET_REGISTRY%s+") then
      local registry = line:match('^CHARSET_REGISTRY%s+"?([^"%s]+)"?$')
      if not registry then fail(line_number, "invalid CHARSET_REGISTRY") end
      registry = registry:upper()
      if registry ~= "ISO10646" and registry ~= "UNICODE" then fail(line_number, "legacy character sets are not supported") end
    elseif line:match("^METRICSSET%s+") then
      local metrics = integers(line, "METRICSSET", 1, line_number)
      if metrics ~= 0 then fail(line_number, "vertical metrics are not supported") end
    elseif line:match("^CHARS%s+") then
      font.declared_count = integers(line, "CHARS", 1, line_number)
    elseif line:match("^STARTCHAR%s+") then
      if glyph then fail(line_number, "nested STARTCHAR") end
      glyph = { name = line:match("^STARTCHAR%s+(.+)$"), rows = {}, line = line_number }
    elseif line == "BITMAP" then
      if not glyph or glyph.width == nil then fail(line_number, "BITMAP before BBX") end
      in_bitmap = true
    elseif line == "ENDCHAR" then
      if not glyph or not in_bitmap then fail(line_number, "unexpected ENDCHAR") end
      in_bitmap = false
      font.glyph_count = font.glyph_count + 1
      if glyph.encoding == nil or glyph.advance == nil or glyph.width == nil then fail(glyph.line, "glyph is missing ENCODING, DWIDTH, or BBX") end
      if #glyph.rows ~= glyph.height then fail(line_number, string.format("glyph %s has %d BITMAP rows, expected %d", glyph.name, #glyph.rows, glyph.height)) end
      if glyph.encoding == -1 then
        font.skipped = font.skipped + 1
      else
        if not valid_codepoint(glyph.encoding) then fail(glyph.line, "invalid Unicode ENCODING") end
        if font.seen[glyph.encoding] then fail(glyph.line, "duplicate ENCODING " .. glyph.encoding) end
        font.seen[glyph.encoding] = true
        glyph.bitmap = table.concat(glyph.rows)
        glyph.rows = nil
        font.glyphs[#font.glyphs + 1] = glyph
      end
      glyph = nil
    elseif glyph then
      local encoding = integers(line, "ENCODING", 1, line_number)
      if encoding ~= nil then
        glyph.encoding = encoding
      else
        local advance, vertical = integers(line, "DWIDTH", 2, line_number)
        if advance ~= nil then
          if vertical ~= 0 then fail(line_number, "vertical DWIDTH is not supported") end
          glyph.advance = advance
        else
          local width, height, x_offset, y_offset = integers(line, "BBX", 4, line_number)
          if width ~= nil then
            glyph.width, glyph.height, glyph.x_offset, glyph.y_offset = width, height, x_offset, y_offset
          end
        end
      end
    end
  end

  if not saw_start or not saw_end then fail(nil, "BDF is missing STARTFONT or ENDFONT") end
  if glyph or in_bitmap then fail(nil, "unterminated glyph") end
  if not font.declared_count or font.declared_count ~= font.glyph_count then fail(nil, "CHARS does not match the glyph count") end
  if not font.ascent or not font.descent or font.ascent < 0 or font.descent < 0 or font.ascent + font.descent < 1 or font.ascent + font.descent > MAX_DIMENSION then
    fail(nil, "FONT_ASCENT + FONT_DESCENT must be between 1 and 64")
  end
  if not font.box_width or font.box_width < 1 or font.box_width > MAX_DIMENSION or font.box_height < 1 or font.box_height > MAX_DIMENSION then
    fail(nil, "FONTBOUNDINGBOX dimensions must be between 1 and 64")
  end
  if #font.glyphs == 0 or #font.glyphs > MAX_GLYPHS then fail(nil, "encoded glyph count must be between 1 and 4096") end

  for _, item in ipairs(font.glyphs) do
    if item.advance < 0 or item.advance > MAX_DIMENSION then fail(item.line, "DWIDTH must be between 0 and 64") end
    if item.width < 0 or item.height < 0 or item.width > MAX_DIMENSION or item.height > MAX_DIMENSION or ((item.width == 0) ~= (item.height == 0)) then
      fail(item.line, "BBX dimensions must both be zero or between 1 and 64")
    end
    if item.x_offset < -MAX_DIMENSION or item.x_offset > MAX_DIMENSION or item.y_offset < -font.descent or item.y_offset + item.height > font.ascent then
      fail(item.line, "BBX lies outside supported font metrics")
    end
  end
  table.sort(font.glyphs, function(a, b) return a.encoding < b.encoding end)
  return font
end

local function encode_dfn(font)
  local space
  for _, glyph in ipairs(font.glyphs) do if glyph.encoding == 32 then space = glyph break end end
  local default_advance = space and space.advance or font.box_width
  if not default_advance or default_advance < 1 or default_advance > MAX_DIMENSION then fail(nil, "no valid default advance") end
  local fallback = font.default_char
  if fallback == nil and font.seen[63] then fallback = 63 end
  if fallback ~= nil and not font.seen[fallback] then fail(nil, "DEFAULT_CHAR does not name an encoded glyph") end

  local records, bitmaps, offset = {}, {}, 0
  for _, glyph in ipairs(font.glyphs) do
    records[#records + 1] = string.pack("<I4I4i2i2i2I2I2", glyph.encoding, offset, glyph.advance, glyph.x_offset, glyph.y_offset, glyph.width, glyph.height)
    bitmaps[#bitmaps + 1] = glyph.bitmap
    offset = offset + #glyph.bitmap
  end
  local header = string.pack("<c4i2i2i2I2I4I4", "DFN1", font.ascent, font.descent, default_advance, 0, #font.glyphs, fallback or NO_FALLBACK)
  local output = header .. table.concat(records) .. table.concat(bitmaps)
  if #output > MAX_BYTES then fail(nil, "DFN output exceeds 1 MiB") end
  return output, fallback or NO_FALLBACK
end

local function run()
  local options = type(args) == "table" and args or {}
  local input = relative_path("input", options.input, ".bdf")
  local output = relative_path("output", options.output, ".dfn")
  if input == output then error("input and output must differ", 0) end
  if storage.exists(output) and options.overwrite ~= true then error("output already exists", 0) end

  local info, stat_error = storage.stat(input)
  if not info then error("cannot stat input: " .. tostring(stat_error), 0) end
  if info.type ~= "file" or info.size > MAX_BDF_BYTES then error("input BDF must be a file no larger than 2 MiB", 0) end
  local source = storage.read_file(input)
  local font = parse_bdf(source)
  source = nil
  collectgarbage()
  local data, fallback = encode_dfn(font)
  local temporary = output .. ".tmp"
  if storage.exists(temporary) then storage.remove(temporary) end
  local ok, err = pcall(function()
    storage.write_file(temporary, data)
    if storage.exists(output) then storage.remove(output) end
    storage.rename(temporary, output)
  end)
  if not ok then
    if storage.exists(temporary) then storage.remove(temporary) end
    error(err, 0)
  end
  print(json.encode({ ok = true, format = "DFN1", input = input, output = output, glyphs = #font.glyphs, skipped = font.skipped,
    ascent = font.ascent, descent = font.descent, fallback = fallback, bytes = #data }))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[bdf_font_converter] ERROR: " .. tostring(err))
  error(err, 0)
end
