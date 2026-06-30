"""serial-log: stream complete subprocess log lines to per-line callbacks.

Spawn a command that emits logs (typically ``idf.py monitor``) and receive each
complete line via a callback, with ANSI color stripped and decoding configured.
Framework-agnostic and dependency-free; what consumes the lines (a parser, a
test harness, a file) is entirely up to the caller.
"""

from __future__ import annotations

from .stream import LogCallback, LogStream, Unsubscribe

__all__ = ['LogStream', 'LogCallback', 'Unsubscribe']

__version__ = '0.1.0'
