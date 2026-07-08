"""Line-level parser for claw-log's flat-tree ``TRACE`` format.

The authoritative grammar lives in ``claw-log/docs/trace-format.md``. A line is::

    <transport-prefix> TRACE <timestamp> <type> <tracing-context> <incremental-context>* <custom>

- Everything before the ``TRACE`` marker is the transport prefix (ESP_LOG's
  ``I (..) tag:`` or the host logger's prefix) and is ignored.
- ``<timestamp>`` is a monotonic timestamp in milliseconds.
- ``<type>`` is ``enter`` / ``exit`` / ``event``.
- ``<tracing-context>`` is one ``<key=value ...>`` block; tokens are
  single-space separated and contain no spaces.
- ``<incremental-context>`` blocks appear **only on ``enter``**, zero or more,
  each a named context group of the form ``<context=<group> key=value ...>``
  (the first token is ``context=<group>``). A block is recognised purely by that
  leading ``context=`` token, so custom text starting with ``<`` is never
  mistaken for one. Groups are caller-defined, not baked into the parser.
- ``<custom>`` is free text appended after the blocks (``enter`` / ``event``
  only); it may contain spaces, commas, pipes and angle brackets.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from enum import Enum
from typing import Iterable, Iterator, Optional

from .adapters import LineAdapter

# The leading token of an incremental-context block names its group:
# ``<context=run …>``.
_CONTEXT_GROUP_KEY = 'context'

# Sentinel a record uses for "no span" / "no parent".
_NONE_TOKEN = 'none'

# Matches the record marker at a token boundary and captures ts, type and the
# remainder (blocks + custom). DOTALL is irrelevant since lines never contain
# newlines, but kept defensive.
_RECORD_RE = re.compile(r'(?:^|\s)TRACE\s+(\S+)\s+(\S+)\s*(.*)$')


class ParseError(ValueError):
    """A line that *is* a ``TRACE`` record but violates the grammar.

    Lines with no ``TRACE`` marker are not errors (``parse_line`` returns
    ``None`` for them); only a malformed record raises.
    """


class RecordType(str, Enum):
    ENTER = 'enter'
    EXIT = 'exit'
    EVENT = 'event'


@dataclass(frozen=True)
class TraceRecord:
    """One parsed ``TRACE`` line.

    Fields absent for a given ``type`` are ``None`` (``parent``/``name``/
    ``target`` on ``exit``) or empty (``context``/``custom``).

    ``context`` is the incremental context this ``enter`` *opens*, grouped by
    context group: ``{group: {key: value}}`` (e.g.
    ``{"run": {"session": "session-1"}}``). Empty for ``exit``/``event``
    and for ``enter`` lines that open no group.
    """

    ts: int
    type: RecordType
    span: Optional[int]
    task: str
    parent: Optional[int] = None
    name: Optional[str] = None
    target: Optional[str] = None
    context: dict[str, dict[str, str]] = field(default_factory=dict)
    custom: str = ''
    raw: str = ''


def _parse_kv(block: str) -> dict[str, str]:
    """Parse a block's ``key=value`` tokens (single-space separated)."""
    tokens: dict[str, str] = {}
    for token in block.split():
        key, sep, value = token.partition('=')
        if not sep or not key:
            raise ParseError(f'block token is not key=value: {token!r}')
        tokens[key] = value
    return tokens


def _take_block(text: str) -> tuple[Optional[str], str]:
    """If ``text`` (after leading spaces) starts with a ``<...>`` block, return
    ``(block_contents, remainder)``; otherwise ``(None, text)`` unchanged."""
    stripped = text.lstrip()
    if not stripped.startswith('<'):
        return None, text
    end = stripped.find('>')
    if end == -1:
        raise ParseError("unterminated '<...>' block")
    return stripped[1:end], stripped[end + 1 :]


def _require(tokens: dict[str, str], key: str, type_: RecordType) -> str:
    try:
        return tokens[key]
    except KeyError:
        raise ParseError(f"{type_.value} record missing '{key}'") from None


def _parse_id(value: str) -> Optional[int]:
    """``"none"`` -> ``None``; otherwise the integer id."""
    if value == _NONE_TOKEN:
        return None
    try:
        return int(value)
    except ValueError:
        raise ParseError(f'invalid span/parent id: {value!r}') from None


def parse_line(
    line: str, adapter: Optional[LineAdapter] = None
) -> Optional[TraceRecord]:
    """Parse a single line.

    When ``adapter`` is given it preprocesses the line first (strip a prefix,
    remove ANSI escapes, …); an adapter returning ``None`` skips the line. See
    :mod:`claw_trace.adapters`.

    Returns ``None`` when the (adapted) line carries no ``TRACE`` marker (a
    transport-only or unrelated line). Raises :class:`ParseError` when a
    ``TRACE`` record is present but malformed. ``raw`` keeps the original,
    un-adapted line.
    """
    original = line
    if adapter is not None:
        adapted = adapter(line)
        if adapted is None:
            return None
        line = adapted

    match = _RECORD_RE.search(line)
    if match is None:
        return None

    ts_str, type_str, remainder = match.groups()
    try:
        ts = int(ts_str)
    except ValueError:
        raise ParseError(f'timestamp is not an integer: {ts_str!r}') from None
    try:
        record_type = RecordType(type_str)
    except ValueError:
        raise ParseError(f'unknown record type: {type_str!r}') from None

    block, remainder = _take_block(remainder)
    if block is None:
        raise ParseError("missing tracing-context '<...>' block")
    tracing = _parse_kv(block)

    task = _require(tracing, 'task', record_type)

    if record_type is RecordType.EXIT:
        # exit carries only the tracing context; no incremental/custom.
        return TraceRecord(
            ts=ts,
            type=record_type,
            span=_parse_id(_require(tracing, 'span', record_type)),
            task=task,
            raw=original,
        )

    if record_type is RecordType.ENTER:
        name = _require(tracing, 'span-name', record_type)
        context, remainder = _take_incremental_context(remainder)
        return TraceRecord(
            ts=ts,
            type=record_type,
            span=_parse_id(_require(tracing, 'span', record_type)),
            parent=_parse_id(_require(tracing, 'parent', record_type)),
            task=task,
            name=name,
            target=_require(tracing, 'target', record_type),
            context=context,
            custom=remainder.strip(),
            raw=original,
        )

    # event
    return TraceRecord(
        ts=ts,
        type=record_type,
        span=_parse_id(_require(tracing, 'span', record_type)),
        task=task,
        name=_require(tracing, 'event-name', record_type),
        target=_require(tracing, 'target', record_type),
        custom=remainder.strip(),
        raw=original,
    )


def _take_incremental_context(
    remainder: str,
) -> tuple[dict[str, dict[str, str]], str]:
    """Pull the leading incremental-context blocks (``enter`` only) off the front.

    Consumes zero or more ``<context=<group> key=value ...>`` blocks, returning
    ``({group: {key: value}}, rest)``. A ``<...>`` block whose first token is not
    ``context=`` belongs to the custom context, so iteration stops and the block
    is left in place.
    """
    groups: dict[str, dict[str, str]] = {}
    while True:
        block, after = _take_block(remainder)
        if block is None:
            break
        tokens = block.split()
        group = _block_group(tokens)
        if group is None:
            # Not an incremental block: the leading '<' is custom text.
            break
        fields = groups.setdefault(group, {})
        for token in tokens[1:]:
            key, sep, value = token.partition('=')
            if not sep or not key:
                raise ParseError(f'context block token is not key=value: {token!r}')
            fields[key] = value
        remainder = after
    return groups, remainder


def _block_group(tokens: list[str]) -> Optional[str]:
    """Return the group name if ``tokens`` start with ``context=<group>``, else
    ``None`` (the block is not an incremental-context block)."""
    if not tokens:
        return None
    key, sep, value = tokens[0].partition('=')
    if key != _CONTEXT_GROUP_KEY or not sep or not value:
        return None
    return value


def parse(
    source: Iterable[str] | str, adapter: Optional[LineAdapter] = None
) -> Iterator[TraceRecord]:
    """Parse every ``TRACE`` line from a string or an iterable of lines.

    ``adapter`` preprocesses each line (see :mod:`claw_trace.adapters`). Lines
    without a ``TRACE`` marker — or dropped by the adapter — are skipped.
    Malformed records raise :class:`ParseError`.
    """
    lines = source.splitlines() if isinstance(source, str) else source
    for line in lines:
        record = parse_line(line, adapter)
        if record is not None:
            yield record
