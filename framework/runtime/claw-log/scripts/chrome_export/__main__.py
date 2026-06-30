"""CLI for the standalone Chrome Trace exporter: ``claw-trace-chrome``.

uv run claw-trace-chrome device.log -o trace.json
cat device.log | uv run claw-trace-chrome --strip-ansi -o trace.json
"""

from __future__ import annotations

import argparse
import sys
from typing import Optional, Sequence

from claw_trace import build_forest, strip_ansi

from . import write_chrome_trace


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog='claw-trace-chrome',
        description='Export a claw-log TRACE log to Chrome Trace Event JSON.',
    )
    parser.add_argument('file', nargs='?', help='Log file; reads stdin when omitted.')
    parser.add_argument(
        '-o',
        '--output',
        default='trace.json',
        help='Output JSON path (default: trace.json).',
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
    count = write_chrome_trace(forest, args.output)
    print(f'wrote {args.output} ({count} events)', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
