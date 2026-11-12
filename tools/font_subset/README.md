# Font Subset Tool

This tool generates a smaller TTF font by keeping only the characters that appear in a UTF-8 text file.

## Dependency

```bash
python3 -m pip install fonttools
```

## Usage

```bash
python3 tools/font_subset/subset_font.py \
    --font tools/font_subset/input.ttf \
    --text tools/font_subset/character.txt \
    --output subset.ttf \
    --verbose
```

By default, the output font keeps every distinct drawable character that appears in the text file and is supported by the input font. Unicode control characters such as newline, carriage return, and tab are ignored because they are handled by text layout rather than font glyph rendering. The tool does not add ASCII, digits, spaces, or punctuation unless those characters are present in the text file.

Useful options:

- `--verbose`: print input/output size and glyph coverage details.
- `--drop-missing`: do not warn when some text characters are missing from the input font.
- `--no-keep-layout`: drop OpenType layout tables such as GSUB/GPOS for a smaller output. Use this only after checking that rendered text still looks correct.
- `--symbol-font` with `--symbol-text`: merge the characters listed in the symbol text file from a second TrueType/WOFF font, scaled to the main font's em. Characters already in the main font are kept from the main font. Private-use characters (Unicode category `Co`) are kept so icon fonts can be merged.
- `--symbol-names`: select symbol font glyphs by name, one per line, such as `snowflake`. It can replace or extend `--symbol-text`; unknown names stop the build.

The generated TTF can be placed under `application/edge_agent/fatfs_image/system/fonts/` or the device DATA font directory and loaded by the existing LVGL tiny_ttf path.

## System Font

`application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf` is built from Noto Sans SC Regular 2.004 (the static TTF shipped in the LVGL repository at `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`) and the LVGL FontAwesome symbol font. `lvgl_symbols.txt` lists every `LV_SYMBOL_*` icon from `lv_symbol_def.h`, so widgets such as the keyboard, dropdown and checkbox render their icons with this font. `fa_icons.txt` adds extra FontAwesome 5 icons by name; every entry is exposed to Lua as `lvgl.SYMBOL.<NAME>` by `components/lua_modules/lua_module_lvgl/src/lua_lvgl_symbols.c`, so update both files together.

```bash
python3 tools/font_subset/subset_font.py \
    --font <lvgl>/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
    --text tools/font_subset/character.txt \
    --symbol-font <lvgl>/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff \
    --symbol-text tools/font_subset/lvgl_symbols.txt \
    --symbol-names tools/font_subset/fa_icons.txt \
    --output application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \
    --verbose
```

`<lvgl>` is an LVGL 9.x source tree, for example `application/edge_agent/managed_components/lvgl__lvgl` after a build. Regenerate `lvgl_symbols.txt` when LVGL adds symbols.

## Other Font Reduction Methods

- Text-based subsetting: keep only characters collected from actual UI strings. This tool uses this method and is the smallest option for fixed text.
- Unicode-range subsetting: keep a broad range such as CJK Unified Ideographs plus punctuation. This is less precise but safer when runtime text is not fully known.
- Source string aggregation: scan Lua, C, frontend, and config files to build a complete character set before subsetting. This is useful for firmware UI text, but needs a project-aware extractor.
- Bitmap or LVGL C fonts: convert glyphs into fixed-size bitmap data. Runtime cost is low, but each font size needs a separate generated asset.
- Font table trimming: remove hinting, names, bitmap strikes, variations, or layout tables. This can reduce size further but must be validated with the target renderer.
- Split fonts: keep a small base font and load feature- or language-specific fonts separately. This needs runtime fallback or font selection support.
