"""claw-trace: parse claw-log's flat-tree ``TRACE`` format and rebuild the tree.

See ``claw-log/docs/trace-format.md`` for the authoritative grammar.

Typical use::

    from claw_trace import build_forest, render_tree

    with open("device.log") as handle:
        forest = build_forest(handle)
    print(render_tree(forest))
"""

from __future__ import annotations

from .adapters import (
    LineAdapter,
    chain,
    keep_after_marker,
    strip_ansi,
)
from .parser import (
    ParseError,
    RecordType,
    TraceRecord,
    parse,
    parse_line,
)
from .tree import (
    EventNode,
    Forest,
    GroupedContext,
    SpanNode,
    build_forest,
    flatten_context,
    render_tree,
)

__all__ = [
    'ParseError',
    'RecordType',
    'TraceRecord',
    'parse',
    'parse_line',
    'LineAdapter',
    'chain',
    'keep_after_marker',
    'strip_ansi',
    'EventNode',
    'Forest',
    'GroupedContext',
    'SpanNode',
    'build_forest',
    'flatten_context',
    'render_tree',
]
