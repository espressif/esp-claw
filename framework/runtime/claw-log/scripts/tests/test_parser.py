"""Line-parser tests against the grammar in docs/trace-format.md."""

from __future__ import annotations

import pytest

from claw_trace import ParseError, RecordType, parse, parse_line


def test_enter_with_incremental_block_and_custom() -> None:
    line = (
        'TRACE 2105 enter '
        '<span=2 parent=1 task=main span-name=turn target=claw_core::orchestrator> '
        '<context=run turn=7> message_id=m1 cause=message'
    )
    record = parse_line(line)
    assert record is not None
    assert record.type is RecordType.ENTER
    assert record.ts == 2105
    assert record.span == 2
    assert record.parent == 1
    assert record.task == 'main'
    assert record.name == 'turn'
    assert record.target == 'claw_core::orchestrator'
    assert record.context == {'run': {'turn': '7'}}
    assert record.custom == 'message_id=m1 cause=message'


def test_parent_none_becomes_none() -> None:
    record = parse_line(
        'TRACE 2100 enter <span=1 parent=none task=main span-name=session target=t> '
        '<context=run session=s-1>'
    )
    assert record is not None
    assert record.parent is None
    assert record.span == 1
    assert record.context == {'run': {'session': 's-1'}}


def test_multiple_incremental_groups() -> None:
    line = (
        'TRACE 7 enter <span=3 parent=2 task=main span-name=req target=t> '
        '<context=run agent=a-1> <context=http method=GET> url=/x'
    )
    record = parse_line(line)
    assert record is not None
    assert record.context == {
        'run': {'agent': 'a-1'},
        'http': {'method': 'GET'},
    }
    assert record.custom == 'url=/x'


def test_exit_has_only_tracing_context() -> None:
    record = parse_line('TRACE 2158 exit <span=1 task=main>')
    assert record is not None
    assert record.type is RecordType.EXIT
    assert record.span == 1
    assert record.task == 'main'
    assert record.parent is None
    assert record.context == {}
    assert record.custom == ''


def test_event_with_free_form_custom() -> None:
    line = (
        'TRACE 2150 event '
        '<span=4 task=main event-name=completion target=claw_core::iteration_loop> '
        'status=done 👋 Hello! | pipe, comma <ok>'
    )
    record = parse_line(line)
    assert record is not None
    assert record.type is RecordType.EVENT
    assert record.span == 4
    assert record.name == 'completion'
    assert record.custom == 'status=done 👋 Hello! | pipe, comma <ok>'


def test_event_span_none() -> None:
    record = parse_line(
        'TRACE 1 event <span=none task=main event-name=warn target=t> oops'
    )
    assert record is not None
    assert record.span is None


def test_transport_prefix_is_ignored() -> None:
    line = 'I (2100) claw_core: TRACE 2100 enter <span=1 parent=none task=main span-name=session target=t>'
    record = parse_line(line)
    assert record is not None
    assert record.ts == 2100
    assert record.name == 'session'


def test_custom_starting_with_angle_is_not_incremental() -> None:
    # Second '<...>' has a non-inherited key, so it is custom text, not a block.
    line = 'TRACE 10 enter <span=1 parent=none task=main span-name=x target=t> <note=hi> foo=bar'
    record = parse_line(line)
    assert record is not None
    assert record.context == {}
    assert record.custom == '<note=hi> foo=bar'


def test_non_trace_line_returns_none() -> None:
    assert parse_line('I (123) wifi: connected') is None
    assert parse_line('') is None


@pytest.mark.parametrize(
    'line',
    [
        'TRACE notanint enter <span=1 parent=none task=main span-name=x target=t>',
        'TRACE 1 bogus <span=1 task=main>',
        'TRACE 1 enter span=1 parent=none task=main',  # missing '<...>' block
        'TRACE 1 enter <span=1 parent=none span-name=x target=t>',  # missing task
        'TRACE 1 enter <span=1 parent=none task=main target=t>',  # missing span-name
    ],
)
def test_malformed_record_raises(line: str) -> None:
    with pytest.raises(ParseError):
        parse_line(line)


def test_parse_skips_non_records() -> None:
    text = '\n'.join(
        [
            'boot ok',
            'TRACE 1 exit <span=1 task=main>',
            'random noise',
        ]
    )
    records = list(parse(text))
    assert len(records) == 1
    assert records[0].span == 1
