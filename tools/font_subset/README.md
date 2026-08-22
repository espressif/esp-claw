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

The generated TTF can be placed under `application/edge_agent/fatfs_image/system/fonts/` or the device DATA font directory and loaded by the existing LVGL tiny_ttf path.

## Other Font Reduction Methods

- Text-based subsetting: keep only characters collected from actual UI strings. This tool uses this method and is the smallest option for fixed text.
- Unicode-range subsetting: keep a broad range such as CJK Unified Ideographs plus punctuation. This is less precise but safer when runtime text is not fully known.
- Source string aggregation: scan Lua, C, frontend, and config files to build a complete character set before subsetting. This is useful for firmware UI text, but needs a project-aware extractor.
- Bitmap or LVGL C fonts: convert glyphs into fixed-size bitmap data. Runtime cost is low, but each font size needs a separate generated asset.
- Font table trimming: remove hinting, names, bitmap strikes, variations, or layout tables. This can reduce size further but must be validated with the target renderer.
- Split fonts: keep a small base font and load feature- or language-specific fonts separately. This needs runtime fallback or font selection support.
