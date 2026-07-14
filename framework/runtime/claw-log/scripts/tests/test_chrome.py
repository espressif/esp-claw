"""Tests for the Chrome Trace Event export."""

from __future__ import annotations

import json

import pytest

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


def test_system_and_session_scopes_become_distinct_processes() -> None:
    log = '\n'.join(
        [
            'TRACE 1 enter <span=1 parent=none task=orchestrator span-name=orchestrator target=t> <context=run system=agent-system>',
            'TRACE 2 enter <span=2 parent=1 task=orchestrator span-name=agent.factory target=t>',
            'TRACE 3 exit <span=2 task=orchestrator>',
            'TRACE 4 enter <span=3 parent=1 task=session-1 span-name=session target=t> <context=run system=agent-system session=session-1>',
            'TRACE 5 exit <span=3 task=session-1>',
            'TRACE 6 exit <span=1 task=orchestrator>',
        ]
    )

    bodies = [event.to_dict() for event in chrome_trace_events(build_forest(log))]
    process_names = {
        event['args']['name']: event['pid']
        for event in bodies
        if event['ph'] == 'M' and event['name'] == 'process_name'
    }

    assert set(process_names) == {'agent-system', 'session-1'}
    factory = next(event for event in bodies if event['name'] == 'agent.factory')
    session = next(event for event in bodies if event['name'] == 'session')
    assert factory['pid'] == process_names['agent-system']
    assert factory['args']['system'] == 'agent-system'
    assert session['pid'] == process_names['session-1']
    assert session['args']['system'] == 'agent-system'
    assert session['args']['session'] == 'session-1'


def test_same_session_id_in_distinct_system_scopes_does_not_merge() -> None:
    log = '\n'.join(
        [
            'TRACE 1 enter <span=1 parent=none task=session-1 span-name=session target=t> <context=run system=system-a session=session-1>',
            'TRACE 2 exit <span=1 task=session-1>',
            'TRACE 3 enter <span=2 parent=none task=session-1 span-name=session target=t> <context=run system=system-b session=session-1>',
            'TRACE 4 exit <span=2 task=session-1>',
        ]
    )

    bodies = [event.to_dict() for event in chrome_trace_events(build_forest(log))]
    sessions = [event for event in bodies if event['ph'] == 'X']

    assert len(sessions) == 2
    assert sessions[0]['pid'] != sessions[1]['pid']


def test_legacy_session_without_system_is_rejected() -> None:
    log = '\n'.join(
        [
            'TRACE 1 enter <span=1 parent=none task=session-9 span-name=session target=t> <context=run session=session-9>',
            'TRACE 2 exit <span=1 task=session-9>',
        ]
    )

    with pytest.raises(ValueError, match='run.session requires run.system'):
        chrome_trace_events(build_forest(log))


def test_record_without_system_or_session_is_unattributed() -> None:
    log = '\n'.join(
        [
            'TRACE 1 enter <span=1 parent=none task=main span-name=boot target=t>',
            'TRACE 2 exit <span=1 task=main>',
        ]
    )

    bodies = [event.to_dict() for event in chrome_trace_events(build_forest(log))]
    process_name = next(
        event
        for event in bodies
        if event['ph'] == 'M' and event['name'] == 'process_name'
    )

    assert process_name['args']['name'] == 'unattributed'


def test_lifecycle_numeric_fields_remain_instant_event_args() -> None:
    log = '\n'.join(
        [
            'TRACE 10 enter <span=1 parent=none task=main span-name=iteration_loop target=t> <context=run system=agent-system iteration=i-0>',
            'TRACE 11 event <span=1 task=main event-name=arguments target=t> argument_bytes=120 completed=1',
            'TRACE 12 event <span=1 task=main event-name=completed target=t> replace_count=2 count=3',
            'TRACE 20 exit <span=1 task=main>',
        ]
    )
    events = chrome_trace_events(build_forest(log))
    assert _by_phase(events, 'C') == []

    instants = {
        event.to_dict()['name']: event.to_dict() for event in _by_phase(events, 'I')
    }
    assert instants['arguments']['args']['argument_bytes'] == '120'
    assert instants['arguments']['args']['completed'] == '1'
    assert instants['completed']['args']['replace_count'] == '2'
    assert instants['completed']['args']['count'] == '3'


def test_explicit_counter_fields_become_counter() -> None:
    log = '\n'.join(
        [
            'TRACE 10 enter <span=1 parent=none task=main span-name=iteration_loop target=t> <context=run system=agent-system iteration=i-0>',
            'TRACE 12 event <span=1 task=main event-name=ram target=claw_ram> counter.free_heap=120000 counter.min_free=90000 sample=1',
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


def test_explicit_counter_value_must_be_numeric() -> None:
    log = '\n'.join(
        [
            'TRACE 10 enter <span=1 parent=none task=main span-name=iteration_loop target=t> <context=run system=agent-system>',
            'TRACE 12 event <span=1 task=main event-name=ram target=claw_ram> counter.free_heap=unknown',
            'TRACE 20 exit <span=1 task=main>',
        ]
    )

    with pytest.raises(ValueError, match='counter.free_heap must be numeric'):
        chrome_trace_events(build_forest(log))


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
    log = 'TRACE 5 enter <span=1 parent=none task=main span-name=turn target=t> <context=run system=agent-system turn=1>'
    events = chrome_trace_events(build_forest(log))
    begins = _by_phase(events, 'B')
    assert len(begins) == 1
    assert begins[0].to_dict()['name'] == 'turn'
