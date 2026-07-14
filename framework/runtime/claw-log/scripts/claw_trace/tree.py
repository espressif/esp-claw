"""Reconstruct the span tree and in-effect context from flat ``TRACE`` records.

``trace-format.md`` deliberately writes each structural fact once: the tree comes
from the ``(span, parent)`` edges, span timing from the ``enter``/``exit`` ``ts``
difference, and the inherited context from replaying ``enter``/``exit`` per task
(a child's opened keys shadow its ancestors'). This module performs that
replay and exposes the result as a forest of :class:`SpanNode` plus the events
anchored to their spans.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Optional

from .adapters import LineAdapter
from .parser import (
    RecordType,
    TraceRecord,
    parse,
)

# Incremental context is grouped: ``{group: {key: value}}``.
GroupedContext = dict[str, dict[str, str]]


def _merge_context(ancestor: GroupedContext, opened: GroupedContext) -> GroupedContext:
    """Per-group merge: ancestor context overlaid with the keys a child opens
    within each group (child wins)."""
    merged: GroupedContext = {group: dict(fields) for group, fields in ancestor.items()}
    for group, fields in opened.items():
        merged.setdefault(group, {}).update(fields)
    return merged


def flatten_context(context: GroupedContext) -> dict[str, str]:
    """Flatten a grouped context into a single ``{key: value}`` map (group names
    dropped). Convenient for display/args when keys do not collide across
    groups (as with the ``run`` group's system/session/turn/agent/iteration)."""
    flat: dict[str, str] = {}
    for fields in context.values():
        flat.update(fields)
    return flat


@dataclass
class EventNode:
    """An instantaneous record, resolved to the context in effect when it fired."""

    ts: int
    span_id: Optional[int]
    task: str
    name: str
    target: str
    custom: str
    context: GroupedContext = field(default_factory=dict)


@dataclass
class SpanNode:
    """A reconstructed span: its identity, timing, opened/effective context,
    child spans and the events that fired directly inside it."""

    id: int
    parent_id: Optional[int]
    task: str
    name: str
    target: str
    enter_ts: int
    custom: str
    opened_context: GroupedContext = field(default_factory=dict)
    context: GroupedContext = field(default_factory=dict)
    exit_ts: Optional[int] = None
    children: list['SpanNode'] = field(default_factory=list)
    events: list[EventNode] = field(default_factory=list)

    @property
    def duration_ms(self) -> Optional[int]:
        """``exit_ts - enter_ts`` once the span has closed, else ``None``."""
        if self.exit_ts is None:
            return None
        return self.exit_ts - self.enter_ts


@dataclass
class Forest:
    """Result of reconstructing a trace stream."""

    roots: list[SpanNode] = field(default_factory=list)
    spans: dict[int, SpanNode] = field(default_factory=dict)
    events: list[EventNode] = field(default_factory=list)
    # Events whose enclosing span id was never seen (or ``none``).
    orphan_events: list[EventNode] = field(default_factory=list)


def build_forest(
    source: Iterable[TraceRecord] | Iterable[str] | str,
    adapter: Optional[LineAdapter] = None,
) -> Forest:
    """Build a :class:`Forest` from records (or raw lines / text).

    Tree edges come from ``(span, parent)``; context inheritance and event
    anchoring come from replaying ``enter``/``exit`` per task. Out-of-order or
    cross-thread interleaving is handled because every span stores the effective
    context computed at its own ``enter``.

    ``adapter`` preprocesses each line when ``source`` is raw text/lines (see
    :mod:`claw_trace.adapters`); it is ignored when ``source`` already yields
    :class:`TraceRecord`.
    """
    records = _as_records(source, adapter)
    forest = Forest()
    # Per-task stack of currently-entered span ids, for context inheritance.
    stacks: dict[str, list[int]] = {}

    for record in records:
        if record.type is RecordType.ENTER:
            _on_enter(record, forest, stacks)
        elif record.type is RecordType.EXIT:
            _on_exit(record, forest, stacks)
        else:
            _on_event(record, forest, stacks)

    return forest


def _as_records(
    source: Iterable[TraceRecord] | Iterable[str] | str,
    adapter: Optional[LineAdapter],
) -> list[TraceRecord]:
    if isinstance(source, str):
        return list(parse(source, adapter))
    materialized = list(source)
    if materialized and isinstance(materialized[0], TraceRecord):
        # mypy: the list is homogeneous by contract.
        return materialized  # type: ignore[return-value]
    return list(parse(materialized, adapter))  # type: ignore[arg-type]


def _on_enter(
    record: TraceRecord, forest: Forest, stacks: dict[str, list[int]]
) -> None:
    if record.span is None:
        # An enter with no span id is meaningless; skip defensively.
        return
    stack = stacks.setdefault(record.task, [])
    ancestor = forest.spans[stack[-1]].context if stack else {}
    node = SpanNode(
        id=record.span,
        parent_id=record.parent,
        task=record.task,
        name=record.name or '',
        target=record.target or '',
        enter_ts=record.ts,
        custom=record.custom,
        opened_context=_copy_context(record.context),
        context=_merge_context(ancestor, record.context),
    )
    forest.spans[node.id] = node
    parent = forest.spans.get(record.parent) if record.parent is not None else None
    if parent is not None:
        parent.children.append(node)
    else:
        forest.roots.append(node)
    stack.append(node.id)


def _on_exit(record: TraceRecord, forest: Forest, stacks: dict[str, list[int]]) -> None:
    node = forest.spans.get(record.span) if record.span is not None else None
    if node is not None:
        node.exit_ts = record.ts
    stack = stacks.get(record.task)
    if stack and record.span is not None and stack[-1] == record.span:
        stack.pop()
    elif stack and record.span in stack:
        # Tolerate a missing exit by unwinding to the matching span.
        while stack and stack.pop() != record.span:
            pass


def _on_event(
    record: TraceRecord, forest: Forest, stacks: dict[str, list[int]]
) -> None:
    enclosing = forest.spans.get(record.span) if record.span is not None else None
    event = EventNode(
        ts=record.ts,
        span_id=record.span,
        task=record.task,
        name=record.name or '',
        target=record.target or '',
        custom=record.custom,
        context=_copy_context(enclosing.context) if enclosing is not None else {},
    )
    forest.events.append(event)
    if enclosing is not None:
        enclosing.events.append(event)
    else:
        forest.orphan_events.append(event)


def render_tree(forest: Forest) -> str:
    """Render the forest as an indented, human-readable tree."""
    lines: list[str] = []
    for root in forest.roots:
        _render_span(root, depth=0, lines=lines)
    for event in forest.orphan_events:
        lines.append(f'(orphan) {_render_event(event)}')
    return '\n'.join(lines)


def _render_span(node: SpanNode, depth: int, lines: list[str]) -> None:
    indent = '  ' * depth
    duration = '' if node.duration_ms is None else f' ({node.duration_ms}ms)'
    context = _format_context(node.context)
    custom = f' {node.custom}' if node.custom else ''
    lines.append(
        f'{indent}[{node.name}] span={node.id} task={node.task}{duration}{context}{custom}'
    )
    # Interleave events and child spans in timestamp order under this span.
    items: list[tuple[int, int, object]] = []
    items.extend((event.ts, 0, event) for event in node.events)
    items.extend((child.enter_ts, 1, child) for child in node.children)
    for _, _, item in sorted(items, key=lambda triple: (triple[0], triple[1])):
        if isinstance(item, EventNode):
            lines.append(f'{indent}  - {_render_event(item)}')
        elif isinstance(item, SpanNode):
            _render_span(item, depth + 1, lines)


def _render_event(event: EventNode) -> str:
    context = _format_context(event.context)
    custom = f' {event.custom}' if event.custom else ''
    return f'{event.name}{context}{custom}'.strip()


def _copy_context(context: GroupedContext) -> GroupedContext:
    """Deep-ish copy of a grouped context (independent inner dicts)."""
    return {group: dict(fields) for group, fields in context.items()}


def _format_context(context: GroupedContext) -> str:
    """Render grouped context as one ``<group key=value ...>`` block per group."""
    if not context:
        return ''
    blocks = []
    for group, fields in context.items():
        inner = ' '.join(f'{key}={value}' for key, value in fields.items())
        blocks.append(f'<{group} {inner}>' if inner else f'<{group}>')
    return ' ' + ' '.join(blocks)
