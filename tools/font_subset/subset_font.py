#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import logging
import sys
import unicodedata
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont, TTLibError
from fontTools.ttLib.scaleUpem import scale_upem


LOGGER = logging.getLogger('font_subset')
SYMBOL_GLYPH_PREFIX = 'sym'
BMP_LAST_CODEPOINT = 0xFFFF


class FontSubsetError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Generate a TTF subset that only contains characters collected from a UTF-8 text file.')
    parser.add_argument('--font', required=True, help='Input TTF font path.')
    parser.add_argument('--text', required=True, help='UTF-8 text file used to collect retained characters.')
    parser.add_argument('--output', required=True, help='Output subset TTF path.')
    parser.add_argument('--verbose', action='store_true', help='Print input/output size and glyph coverage details.')
    parser.add_argument('--drop-missing', action='store_true', help='Continue silently when the input font does not contain some text characters.')
    parser.add_argument('--keep-layout', action=argparse.BooleanOptionalAction, default=True, help='Keep OpenType layout tables such as GSUB/GPOS. Enabled by default.')
    parser.add_argument('--symbol-font', help='Optional TrueType/WOFF symbol font, such as the LVGL FontAwesome font, merged into the output.')
    parser.add_argument('--symbol-text', help='UTF-8 text file with the symbol characters taken from --symbol-font.')
    parser.add_argument('--symbol-names', help='Text file with one --symbol-font glyph name per line, such as "snowflake". "#" starts a comment.')
    args = parser.parse_args()
    if bool(args.symbol_font) != bool(args.symbol_text or args.symbol_names):
        parser.error('--symbol-font needs --symbol-text and/or --symbol-names, and they need --symbol-font')
    return args


def fail(message: str) -> None:
    raise FontSubsetError(message)


def read_text_file(path: Path) -> str:
    try:
        return path.read_text(encoding='utf-8')
    except FileNotFoundError:
        fail(f'Missing text file: {path}')
    except UnicodeDecodeError as exc:
        fail(f'Text file must be UTF-8: {path}: {exc}')
    except OSError as exc:
        fail(f'Read text file failed: {path}: {exc}')


def is_drawable(char: str) -> bool:
    # Control and format characters only affect layout; private-use characters (Co) are icon glyphs and must be kept.
    category = unicodedata.category(char)
    return not category.startswith('C') or category == 'Co'


def collect_codepoints(text: str) -> list[int]:
    codepoints = sorted({ord(char) for char in text if is_drawable(char)})
    if not codepoints:
        fail('Text file does not contain any characters.')
    return codepoints


def load_font(path: Path) -> TTFont:
    if not path.is_file():
        fail(f'Missing font file: {path}')
    try:
        return TTFont(path)
    except TTLibError as exc:
        fail(f'Open font failed: {path}: {exc}')
    except OSError as exc:
        fail(f'Read font failed: {path}: {exc}')


def collect_supported_codepoints(font: TTFont) -> set[int]:
    # cmap is the authoritative Unicode-to-glyph mapping used for text coverage checks.
    supported: set[int] = set()
    for table in font['cmap'].tables if 'cmap' in font else []:
        if table.isUnicode():
            supported.update(table.cmap.keys())
    return supported


def resolve_glyph_names(font: TTFont, text: str) -> list[int]:
    names = [line.split('#', 1)[0].strip() for line in text.splitlines()]
    names = [name for name in names if name]
    # A glyph may be mapped by several code points; prefer the lowest so the result is stable.
    by_name: dict[str, int] = {}
    for codepoint, name in sorted(font.getBestCmap().items(), reverse=True):
        by_name[name] = codepoint
    unknown = [name for name in names if name not in by_name]
    if unknown:
        fail(f'Symbol font does not contain glyph names: {" ".join(unknown)}')
    return sorted({by_name[name] for name in names})


def format_codepoints(codepoints: list[int], limit: int = 16) -> str:
    shown = ' '.join(f'U+{codepoint:04X}' for codepoint in codepoints[:limit])
    if len(codepoints) > limit:
        shown += f' ... (+{len(codepoints) - limit} more)'
    return shown


def build_options(keep_layout: bool) -> subset.Options:
    options = subset.Options()
    options.flavor = None
    options.with_zopfli = False
    options.recalc_bounds = True
    options.recalc_timestamp = False
    options.canonical_order = True
    if keep_layout:
        options.layout_features = ['*']
    else:
        options.layout_features = []
        options.drop_tables += ['GSUB', 'GPOS']
    return options


def subset_font(font: TTFont, codepoints: list[int], keep_layout: bool) -> TTFont:
    options = build_options(keep_layout)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    return font


def merge_symbol_glyphs(base: TTFont, symbol_font: TTFont, codepoints: list[int]) -> tuple[list[int], list[int]]:
    """Append symbol glyphs scaled to the base em. Base glyphs win when both fonts map a code point.

    Returns the merged and the missing code points.
    """
    if 'glyf' not in base or 'glyf' not in symbol_font:
        fail('Symbol merge needs TrueType (glyf) outlines in both fonts.')
    base_cmap = base.getBestCmap()
    symbol_supported = collect_supported_codepoints(symbol_font)
    wanted = [codepoint for codepoint in codepoints if codepoint not in base_cmap and codepoint in symbol_supported]
    missing = [codepoint for codepoint in codepoints if codepoint not in base_cmap and codepoint not in symbol_supported]
    if not wanted:
        return [], missing

    subset_font(symbol_font, wanted, keep_layout=False)
    scale_upem(symbol_font, base['head'].unitsPerEm)
    symbol_cmap = symbol_font.getBestCmap()
    symbol_glyf, symbol_hmtx = symbol_font['glyf'], symbol_font['hmtx']
    glyf, hmtx = base['glyf'], base['hmtx']
    vmtx = base['vmtx'] if 'vmtx' in base else None
    # Vertical metrics follow the base typographic box so vertical layout stays consistent.
    vertical_advance = base['head'].unitsPerEm
    vertical_top = base['OS/2'].sTypoAscender

    glyph_order = list(base.getGlyphOrder())
    added: dict[int, str] = {}
    for codepoint in wanted:
        source_name = symbol_cmap[codepoint]
        glyph = symbol_glyf[source_name]
        if glyph.isComposite():
            fail(f'Composite symbol glyph is not supported: U+{codepoint:04X}')
        glyph.recalcBounds(symbol_glyf)
        name = f'{SYMBOL_GLYPH_PREFIX}{codepoint:04X}'
        glyph_order.append(name)
        glyf.glyphs[name] = glyph
        hmtx[name] = symbol_hmtx[source_name]
        if vmtx is not None:
            vmtx[name] = (vertical_advance, vertical_top - getattr(glyph, 'yMax', 0))
        added[codepoint] = name

    base.setGlyphOrder(glyph_order)
    glyf.setGlyphOrder(glyph_order)
    for table in base['cmap'].tables:
        if table.isUnicode():
            table.cmap.update({codepoint: name for codepoint, name in added.items() if table.format != 4 or codepoint <= BMP_LAST_CODEPOINT})
    return list(added), missing


def save_font(font: TTFont, output_path: Path) -> None:
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        font.save(output_path)
    except OSError as exc:
        fail(f'Write output font failed: {output_path}: {exc}')


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=logging.WARNING, format='%(levelname)s: %(message)s')
    LOGGER.setLevel(logging.INFO if args.verbose else logging.WARNING)

    font_path = Path(args.font).resolve()
    text_path = Path(args.text).resolve()
    output_path = Path(args.output).resolve()

    text = read_text_file(text_path)
    requested_codepoints = collect_codepoints(text)
    font = load_font(font_path)
    supported_codepoints = collect_supported_codepoints(font)
    retained_codepoints = [codepoint for codepoint in requested_codepoints if codepoint in supported_codepoints]
    missing_codepoints = [codepoint for codepoint in requested_codepoints if codepoint not in supported_codepoints]

    if missing_codepoints and not args.drop_missing:
        LOGGER.warning('Input font does not contain %d requested characters: %s', len(missing_codepoints), format_codepoints(missing_codepoints))
    if not retained_codepoints:
        fail('No requested characters are supported by the input font.')

    subset_font(font, retained_codepoints, args.keep_layout)

    merged_symbols: list[int] = []
    if args.symbol_font:
        symbol_font = load_font(Path(args.symbol_font).resolve())
        symbol_codepoints: set[int] = set()
        if args.symbol_text:
            symbol_codepoints.update(collect_codepoints(read_text_file(Path(args.symbol_text).resolve())))
        if args.symbol_names:
            symbol_codepoints.update(resolve_glyph_names(symbol_font, read_text_file(Path(args.symbol_names).resolve())))
        merged_symbols, missing_symbols = merge_symbol_glyphs(font, symbol_font, sorted(symbol_codepoints))
        if missing_symbols and not args.drop_missing:
            LOGGER.warning('Symbol font does not contain %d requested characters: %s', len(missing_symbols), format_codepoints(missing_symbols))

    save_font(font, output_path)

    if args.verbose:
        input_size = font_path.stat().st_size
        output_size = output_path.stat().st_size
        ratio = output_size / input_size * 100 if input_size else 0
        LOGGER.info('Requested characters: %d', len(requested_codepoints))
        LOGGER.info('Retained characters: %d', len(retained_codepoints))
        LOGGER.info('Missing characters: %d', len(missing_codepoints))
        if args.symbol_font:
            LOGGER.info('Merged symbol characters: %d', len(merged_symbols))
        LOGGER.info('Input font size: %d bytes', input_size)
        LOGGER.info('Output font size: %d bytes (%.2f%%)', output_size, ratio)
        LOGGER.info('Output font written: %s', output_path)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except FontSubsetError as exc:
        LOGGER.error('subset_font.py: error: %s', exc)
        sys.exit(1)
