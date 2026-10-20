local display = require("display")
local storage = require("storage")

local root = storage.get_root_dir()
local bdf_path = storage.join_path(root, "display_font_contract.bdf")
local dfn_path = storage.join_path(root, "display_font_contract.dfn")
local action = type(args) == "table" and args.action or "prepare"

local bdf = [[STARTFONT 2.1
FONT contract
SIZE 9 75 75
FONTBOUNDINGBOX 8 9 0 -2
STARTPROPERTIES 3
FONT_ASCENT 7
FONT_DESCENT 2
DEFAULT_CHAR 63
ENDPROPERTIES
CHARS 5
STARTCHAR W
ENCODING 87
SWIDTH 500 0
DWIDTH 8 0
BBX 8 7 0 0
BITMAP
81
81
81
99
99
A5
42
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 250 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
STARTCHAR g
ENCODING 103
SWIDTH 375 0
DWIDTH 6 0
BBX 5 7 0 -2
BITMAP
70
88
88
78
08
88
70
ENDCHAR
STARTCHAR question
ENCODING 63
SWIDTH 375 0
DWIDTH 6 0
BBX 5 7 0 0
BITMAP
70
88
08
10
20
00
20
ENDCHAR
STARTCHAR i
ENCODING 105
SWIDTH 188 0
DWIDTH 3 0
BBX 1 7 1 0
BITMAP
80
00
80
80
80
80
80
ENDCHAR
ENDFONT
]]

if action == "prepare" then
  storage.write_file(bdf_path, bdf)
  if storage.exists(dfn_path) then storage.remove(dfn_path) end
  print("display BDF fixture prepared: " .. bdf_path)
  return
end

if action ~= "verify" then error("action must be prepare or verify") end
local font <close> = display.load_font(dfn_path)
local screen <close> = display.open()
local width, height = screen:measure_text("Wi", { font = font })
assert(width == 11 and height == 9, string.format("Wi metrics mismatch: %dx%d", width, height))
width, height = screen:measure_text("g?", { font = font })
assert(width == 12 and height == 9, string.format("g? metrics mismatch: %dx%d", width, height))
width, height = screen:measure_text("\t", { font = font })
assert(width == 16 and height == 9, string.format("tab metrics mismatch: %dx%d", width, height))
width, height = screen:measure_text("Ω", { font = font })
assert(width == 6 and height == 9, string.format("fallback metrics mismatch: %dx%d", width, height))

screen:begin({ clear = "#102030" })
screen:text(16, 16, "Wi g? Ω", { font = font, color = "#ffffff" })
screen:present()
print("display DFN contract passed: proportional=11x9 descender=12x9 tab=16x9 fallback=6x9")
