"""Tests for the Chrome Trace Event export."""

from __future__ import annotations

import json

from claw_trace import build_forest
from chrome_export import chrome_trace_events, write_chrome_trace
from test_tree import SPEC_EXAMPLE


def _by_phase(events, phase: str):
    return [e for e in events if e.to_dict().get('ph') == phase]


def test_spans_become_complete_events_with_duration() -> None:
    events = chrome_trace_events(build_forest(SPEC_EXAMPLE))
    completes = _by_phase(events, 'X')
    names = {e.to_dict()['name'] for e in completes}
    assert {'session', 'turn', 'agent', 'iteration_loop'} <= names

    # The session span: 2100ms enter, 58ms duration -> microseconds.
    session = next(e for e in completes if e.to_dict()['name'] == 'session')
    body = session.to_dict()
    assert body['ts'] == 2100 * 1000
    assert body['dur'] == 58 * 1000
    assert body['args']['session'] == 'session-1'


def test_subagent_args_carry_shadowed_context() -> None:
    events = chrome_trace_events(build_forest(SPEC_EXAMPLE))
    # span 5 is the tool subagent; its complete event should carry agent-2.
    agents = [
        e.to_dict() for e in _by_phase(events, 'X') if e.to_dict()['name'] == 'agent'
    ]
    shadowed = next(a for a in agents if a['args'].get('agent') == 'agent-2')
    assert shadowed['args']['session'] == 'session-1'
    assert shadowed['args']['depth'] == '1'


def test_events_become_instant_events() -> None:
    events = chrome_trace_events(build_forest(SPEC_EXAMPLE))
    instants = {e.to_dict()['name'] for e in _by_phase(events, 'I')}
    assert {'spawned', 'completion'} <= instants


def test_process_and_thread_metadata_emitted_once() -> None:
    events = chrome_trace_events(build_forest(SPEC_EXAMPLE))
    metas = _by_phase(events, 'M')
    process_names = [
        m.to_dict() for m in metas if m.to_dict()['name'] == 'process_name'
    ]
    # One session in the example -> exactly one process_name metadata event.
    assert len(process_names) == 1
    assert process_names[0]['args']['name'] == 'session-1'
    thread_names = [m.to_dict() for m in metas if m.to_dict()['name'] == 'thread_name']
    assert len(thread_names) == 1
    assert thread_names[0]['args']['name'] == 'main'


def test_numeric_event_fields_become_counter() -> None:
    log = '\n'.join(
        [
            'TRACE 10 enter <span=1 parent=none task=main span-name=iteration_loop target=t> <context=run iteration=i-0>',
            'TRACE 12 event <span=1 task=main event-name=ram target=claw_ram> free_heap=120000 min_free=90000',
            'TRACE 20 exit <span=1 task=main>',
        ]
    )
    events = chrome_trace_events(build_forest(log))
    counters = _by_phase(events, 'C')
    assert len(counters) == 1
    body = counters[0].to_dict()
    assert body['name'] == 'ram'
    assert body['args'] == {'free_heap': 120000.0, 'min_free': 90000.0}
    assert body['ts'] == 12 * 1000


def test_write_chrome_trace_produces_valid_json_array(tmp_path) -> None:
    path = tmp_path / 'trace.json'
    count = write_chrome_trace(build_forest(SPEC_EXAMPLE), path)
    assert count > 0

    data = json.loads(path.read_text(encoding='utf-8'))
    assert isinstance(data, list)
    assert len(data) == count
    # Every event has the mandatory Chrome fields.
    for entry in data:
        assert 'ph' in entry and 'name' in entry


def test_unclosed_span_becomes_duration_begin(tmp_path) -> None:
    log = 'TRACE 5 enter <span=1 parent=none task=main span-name=turn target=t> <context=run turn=1>'
    events = chrome_trace_events(build_forest(log))
    begins = _by_phase(events, 'B')
    assert len(begins) == 1
    assert begins[0].to_dict()['name'] == 'turn'
