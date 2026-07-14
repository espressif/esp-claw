"""Standalone tool: export a ``claw_trace`` forest to the Chrome Trace Event
Format (loadable in ``chrome://tracing`` or https://ui.perfetto.dev).

This is **not** part of the ``claw_trace`` library — it is a separate consumer
of it. ``claw_trace`` stays a pure parsing/reconstruction lib with no dependency
on ``chrometrace``; all Chrome-specific translation lives here.

Mapping:

- each **span** -> a *complete* event (``X``): ``enter`` timestamp + duration, so
  the viewer renders the span tree as a flame chart (nesting comes from
  overlapping intervals on the same thread). An unclosed span becomes a
  *duration begin* (``B``) with no end.
- each **event** -> an *instant* event (``i``, thread scope). Only explicitly
  marked ``counter.<series>=<number>`` fields additionally become a *counter*
  event (``C``); ordinary numeric fields remain instant-event arguments.
- ``run.session`` selects a session process and requires ``run.system``;
  ``run.system`` by itself selects a system process; records with neither use
  the ``unattributed`` process. ``task`` -> thread (``tid``). The inherited
  context, ``target`` and custom fields ride along in ``args``. The system scope
  is part of a session process's identity.
"""

from __future__ import annotations

import os
import re
from typing import Callable

import chrometrace
from chrometrace import TraceEvent, TraceEventType

from claw_trace import EventNode, Forest, GroupedContext, SpanNode, flatten_context

__all__ = ['chrome_trace_events', 'write_chrome_trace']

# Chrome timestamps are microseconds; our trace timestamps are milliseconds.
_US_PER_MS = 1000
# pid/process name used only when neither a system nor a session is attributed.
_UNATTRIBUTED_PROCESS = 'unattributed'

# A loose ``key=value`` token (value has no spaces); used to lift fields out of
# the free-form custom context for nicer ``args`` and explicit counter series.
_KV_TOKEN = re.compile(r'^([^\s=]+)=(\S+)$')
_COUNTER_PREFIX = 'counter.'

# Resolves a (pid, tid) lane from a span/event's context + task.
_Lane = Callable[[GroupedContext, str], 'tuple[int, int]']


class _IdAllocator:
    """Hands out stable small integer ids for hashable keys (pid / tid)."""

    def __init__(self, start: int = 1) -> None:
        self._next = start
        self._ids: dict[object, int] = {}

    def get(self, key: object) -> int:
        if key not in self._ids:
            self._ids[key] = self._next
            self._next += 1
        return self._ids[key]


def _loose_kv(text: str) -> dict[str, str]:
    """Best-effort split of free-form custom text into ``key=value`` tokens.

    Tokens that are not ``key=value`` are ignored; the original text is never
    required to be structured (the spec calls custom context free text).
    """
    fields: dict[str, str] = {}
    for token in text.split():
        match = _KV_TOKEN.match(token)
        if match is not None:
            fields[match.group(1)] = match.group(2)
    return fields


def _counter_series(fields: dict[str, str]) -> dict[str, float]:
    """Parse only explicitly marked ``counter.<series>=<number>`` fields."""
    series: dict[str, float] = {}
    for key, value in fields.items():
        if not key.startswith(_COUNTER_PREFIX):
            continue
        series_name = key.removeprefix(_COUNTER_PREFIX)
        if not series_name:
            raise ValueError('counter field requires a series name')
        try:
            series[series_name] = float(value)
        except ValueError:
            raise ValueError(f'{key} must be numeric, got {value!r}') from None
    return series


def _custom_args(custom: str) -> dict[str, object]:
    """Lift ``key=value`` tokens out of custom text; keep leftover as ``message``."""
    if not custom:
        return {}
    fields = _loose_kv(custom)
    args: dict[str, object] = dict(fields)
    if not fields:
        args['message'] = custom
    return args


def chrome_trace_events(forest: Forest) -> list[TraceEvent]:
    """Translate ``forest`` into a flat list of ``chrometrace.TraceEvent``.

    Pure (no I/O): emits process/thread name metadata, one event per span, and
    instant events for each trace event, plus counters only for explicit
    ``counter.<series>`` fields. Ready to feed a :class:`chrometrace.TraceSink`
    or to inspect in tests via ``to_dict()``.
    """
    pids = _IdAllocator()
    tids = _IdAllocator()
    seen_pid: set[int] = set()
    seen_tid: set[tuple[int, int]] = set()
    out: list[TraceEvent] = []

    def lane(context: GroupedContext, task: str) -> tuple[int, int]:
        """Resolve a scoped process/task lane, emitting naming metadata once."""
        run_context = context.get('run', {})
        system = run_context.get('system')
        session = run_context.get('session')
        if session is not None:
            if system is None:
                raise ValueError(
                    'invalid trace context: run.session requires run.system; '
                    'legacy traces are not supported'
                )
            process_key: object = ('session', system, session)
            process_name = session
        elif system is not None:
            process_key = ('system', system)
            process_name = system
        else:
            process_key = ('unattributed',)
            process_name = _UNATTRIBUTED_PROCESS

        pid = pids.get(process_key)
        tid = tids.get((process_key, task))
        if pid not in seen_pid:
            out.append(
                TraceEvent.process_name(process_id=pid, process_name=process_name)
            )
            seen_pid.add(pid)
        if (pid, tid) not in seen_tid:
            out.append(
                TraceEvent.thread_name(process_id=pid, thread_id=tid, thread_name=task)
            )
            seen_tid.add((pid, tid))
        return pid, tid

    for span in forest.spans.values():
        out.append(_span_event(span, lane))
    for event in forest.events:
        out.extend(_event_events(event, lane))
    return out


def _span_event(span: SpanNode, lane: _Lane) -> TraceEvent:
    pid, tid = lane(span.context, span.task)
    args: dict[str, object] = {
        'span': span.id,
        'target': span.target,
        **flatten_context(span.context),
        **_custom_args(span.custom),
    }
    if span.parent_id is not None:
        args['parent'] = span.parent_id
    start_us = span.enter_ts * _US_PER_MS
    if span.duration_ms is not None:
        return TraceEvent.complete(
            name=span.name,
            timestamp_us=start_us,
            duration_us=span.duration_ms * _US_PER_MS,
            process_id=pid,
            thread_id=tid,
            categories=[span.target],
            args=args,
        )
    # Unclosed span: open-ended begin (the viewer extends it to the trace end).
    return TraceEvent.duration_begin(
        name=span.name,
        timestamp_us=start_us,
        process_id=pid,
        thread_id=tid,
        categories=[span.target],
        args=args,
    )


def _event_events(event: EventNode, lane: _Lane) -> list[TraceEvent]:
    pid, tid = lane(event.context, event.task)
    timestamp_us = event.ts * _US_PER_MS
    args: dict[str, object] = {
        'target': event.target,
        **flatten_context(event.context),
        **_custom_args(event.custom),
    }
    out = [
        TraceEvent(
            name=event.name,
            event_type=TraceEventType.INSTANT,
            timestamp_us=timestamp_us,
            process_id=pid,
            thread_id=tid,
            categories=event.target,
            args=args,
            scope='t',
        )
    ]
    series = _counter_series(_loose_kv(event.custom))
    if series:
        out.append(
            TraceEvent.counter(
                name=event.name,
                timestamp_us=timestamp_us,
                process_id=pid,
                thread_id=tid,
                args=series,
            )
        )
    return out


def write_chrome_trace(forest: Forest, path: os.PathLike[str] | str) -> int:
    """Write ``forest`` to ``path`` as a Chrome Trace Event JSON array.

    Returns the number of trace events written (metadata included).
    """
    events = chrome_trace_events(forest)
    with chrometrace.TraceSink(path) as sink:
        for event in events:
            sink.add_trace_event(event)
    return len(events)
