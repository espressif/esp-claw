"""Per-line adapters (preprocessors) for :func:`claw_trace.parse`.

A :data:`LineAdapter` is ``Callable[[str], Optional[str]]``: it receives one raw
line and returns the line to feed the parser, or ``None`` to drop the line. The
adapter runs **before** the ``TRACE`` marker is located, so it can:

- strip ANSI color escapes the host logger emits on a TTY (:func:`strip_ansi`),
- normalise a transport prefix that the default marker search can't handle,
- filter out unrelated lines (return ``None``).

Adapters compose with :func:`chain`. The default (no adapter) already tolerates
the common ``I (2153) claw_core::iteration_loop: TRACE ...`` prefix, because the
parser searches for the ``TRACE`` marker rather than anchoring at column 0; an
adapter is only needed for transformations the marker search alone can't do.
"""

from __future__ import annotations

import re
from typing import Callable, Optional

# A per-line preprocessor: maps a raw line to the line to parse, or ``None`` to
# skip it.
LineAdapter = Callable[[str], Optional[str]]

# ANSI SGR (color/style) escape sequences, e.g. "\x1b[32m".
_ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')


def strip_ansi(line: str) -> Optional[str]:
    """Remove ANSI color/style escapes (host logger output on a TTY)."""
    return _ANSI_RE.sub('', line)


def keep_after_marker(marker: str = 'TRACE') -> LineAdapter:
    """Build an adapter that drops everything before ``marker``.

    Returns ``None`` for lines without the marker (so they are skipped). Useful
    when a transport prefix itself contains tokens that confuse the parser; the
    default parser does not need this since it searches for the marker.
    """

    def adapter(line: str) -> Optional[str]:
        index = line.find(marker)
        return None if index == -1 else line[index:]

    return adapter


def chain(*adapters: LineAdapter) -> LineAdapter:
    """Compose adapters left to right; short-circuits to ``None`` once any
    adapter drops the line."""

    def composed(line: str) -> Optional[str]:
        current: Optional[str] = line
        for adapter in adapters:
            if current is None:
                return None
            current = adapter(current)
        return current

    return composed
