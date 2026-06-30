"""CLI: capture a live TRACE log from a monitor command and export Chrome trace.

uv run claw-trace-capture --cmd "cargo espflash monitor" -o trace.json
uv run claw-trace-capture --cmd "idf.py monitor" --tee -o trace.json

The monitor runs as a child process so Ctrl-C is handled cleanly: the child is
terminated and the collected records are still exported (unlike a raw pipe,
where SIGINT would kill the exporter before it writes the file).

Chrome export is a batch step: span-tree reconstruction needs the whole capture,
so the JSON is written once the monitor exits (or on Ctrl-C).
"""

from __future__ import annotations

import argparse
import sys
from typing import Optional, Sequence

from chrome_export import write_chrome_trace
from claw_trace import ParseError, TraceRecord, build_forest, parse_line
from serial_log import LogStream


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog='claw-trace-capture',
        description='Capture a live TRACE log from a monitor command and export '
        'it to Chrome Trace Event JSON.',
    )
    parser.add_argument(
        '--cmd',
        required=True,
        help='Monitor command to run, e.g. "cargo espflash monitor".',
    )
    parser.add_argument(
        '-o',
        '--output',
        default='trace.json',
        help='Output JSON path (default: trace.json).',
    )
    parser.add_argument(
        '--encoding',
        default='utf-8',
        help="Encoding used to decode the monitor's bytes (default: utf-8).",
    )
    parser.add_argument(
        '--tee',
        action='store_true',
        help='Also echo each captured line to stderr live.',
    )
    args = parser.parse_args(argv)

    records: list[TraceRecord] = []
    malformed = 0

    def on_log(line: str) -> None:
        nonlocal malformed
        if args.tee:
            print(line, file=sys.stderr)
        # serial_log already stripped ANSI color, so no adapter is needed here.
        # Tolerate the occasional corrupt serial line (a CLI-boundary decision):
        # one bad frame must not abort a live capture.
        try:
            record = parse_line(line)
        except ParseError as error:
            malformed += 1
            print(f'skipped malformed TRACE line: {error}', file=sys.stderr)
            return
        if record is not None:
            records.append(record)

    stream = LogStream(args.cmd, on_log, encoding=args.encoding)
    stream.start()
    try:
        stream.wait()
    except KeyboardInterrupt:
        print('\ninterrupted; exporting captured records...', file=sys.stderr)
    finally:
        stream.stop()

    forest = build_forest(records)
    count = write_chrome_trace(forest, args.output)
    summary = f'wrote {args.output} ({count} events from {len(records)} records'
    summary += f', {malformed} malformed)' if malformed else ')'
    print(summary, file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
