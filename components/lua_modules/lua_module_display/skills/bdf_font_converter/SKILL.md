---
{
  "name": "bdf_font_converter",
  "description": "Convert a Unicode BDF bitmap font in writable storage to the DFN1 format used by the display module.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "peripherals": ["display"]
  }
}
---

# BDF Font Converter

Use this skill when the user wants to create a display font from BDF source. The input and output paths are relative to the writable DATA root.

Run `{CUR_SKILL_DIR}/scripts/bdf_to_dfn.lua` once with `lua_run_script`:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/bdf_to_dfn.lua",
  "args": {
    "input": "fonts/generated.bdf",
    "output": "fonts/generated.dfn",
    "overwrite": false
  }
}
```

The converter supports horizontal Unicode BDF 2.1/2.2 fonts with `FONT_ASCENT`, `FONT_DESCENT`, `ENCODING`, `DWIDTH`, `BBX`, and 1bpp `BITMAP` data. Keep glyph dimensions and line height at or below 64 pixels. Unencoded glyphs are skipped. Vertical metrics, legacy encoded character sets, and more than 4096 encoded glyphs are rejected.

Generate BDF with Unicode scalar values in `ENCODING`. Include a `DEFAULT_CHAR` property or a glyph encoded as 63 when missing glyph fallback is wanted. If conversion fails, report the exact parser error without retrying with changed paths.
