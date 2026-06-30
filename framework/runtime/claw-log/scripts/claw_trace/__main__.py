"""CLI: reconstruct and pretty-print a ``TRACE`` log.

uv run claw-trace device.log
cat device.log | uv run claw-trace
"""

from __future__ import annotations

import argparse
import sys
from typing import Optional, Sequence

from .adapters import strip_ansi
from .tree import build_forest, render_tree


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog='claw-trace',
        description='Parse a claw-log TRACE log and print the reconstructed span tree.',
    )
    parser.add_argument(
        'file',
        nargs='?',
        help='Log file to read; reads stdin when omitted.',
    )
    parser.add_argument(
        '--strip-ansi',
        action='store_true',
        help='Strip ANSI color escapes from each line before parsing.',
    )
    args = parser.parse_args(argv)

    if args.file is None:
        text = sys.stdin.read()
    else:
        with open(args.file, 'r', encoding='utf-8') as handle:
            text = handle.read()

    adapter = strip_ansi if args.strip_ansi else None
    forest = build_forest(text, adapter)
    print(render_tree(forest))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
