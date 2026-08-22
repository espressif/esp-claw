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


LOGGER = logging.getLogger('font_subset')


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
    return parser.parse_args()


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


def collect_codepoints(text: str) -> list[int]:
    # Keep drawable characters from the source text. Control characters only affect layout and do not need font glyphs.
    codepoints = sorted({ord(char) for char in text if not unicodedata.category(char).startswith('C')})
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
    save_font(font, output_path)

    if args.verbose:
        input_size = font_path.stat().st_size
        output_size = output_path.stat().st_size
        ratio = output_size / input_size * 100 if input_size else 0
        LOGGER.info('Requested characters: %d', len(requested_codepoints))
        LOGGER.info('Retained characters: %d', len(retained_codepoints))
        LOGGER.info('Missing characters: %d', len(missing_codepoints))
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
