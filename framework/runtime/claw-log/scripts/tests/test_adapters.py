"""Tests for the per-line adapter hook and the built-in adapters."""

from __future__ import annotations

from claw_trace import (
    build_forest,
    chain,
    keep_after_marker,
    parse,
    parse_line,
    strip_ansi,
)

# A realistic device line: ESP_LOG transport prefix in front of the record.
PREFIXED = 'I (2153) claw_core::iteration_loop: TRACE 2153 exit <span=4 task=main>'


def test_default_handles_transport_prefix_without_adapter() -> None:
    record = parse_line(PREFIXED)
    assert record is not None
    assert record.ts == 2153
    assert record.span == 4
    # raw keeps the full original line, prefix included.
    assert record.raw == PREFIXED


def test_strip_ansi_lets_colored_line_parse() -> None:
    colored = '\x1b[32mI (2153) claw_core: \x1b[0mTRACE 2153 exit \x1b[1m<span=4 task=main>\x1b[0m'
    # Without the adapter the embedded escapes corrupt the tokens.
    assert parse_line(colored, adapter=strip_ansi) is not None
    record = parse_line(colored, adapter=strip_ansi)
    assert record is not None
    assert record.span == 4
    assert record.task == 'main'
    # raw is preserved un-adapted (still has the escapes).
    assert '\x1b[' in record.raw


def test_adapter_returning_none_skips_line() -> None:
    def drop_warnings(line: str):
        return None if line.startswith('W ') else line

    text = '\n'.join(
        [
            'W (1) tag: TRACE 1 exit <span=1 task=main>',  # dropped by adapter
            'I (2) tag: TRACE 2 exit <span=2 task=main>',  # kept
        ]
    )
    records = list(parse(text, adapter=drop_warnings))
    assert [r.span for r in records] == [2]


def test_keep_after_marker_trims_prefix() -> None:
    adapter = keep_after_marker()
    assert adapter('garbage TRACE 1 exit <span=1 task=main>') == (
        'TRACE 1 exit <span=1 task=main>'
    )
    # No marker -> dropped.
    assert adapter('no marker here') is None


def test_chain_composes_and_short_circuits() -> None:
    adapter = chain(strip_ansi, keep_after_marker())
    record = parse_line(
        '\x1b[31mprefix \x1b[0mTRACE 9 exit <span=9 task=main>', adapter=adapter
    )
    assert record is not None
    assert record.span == 9
    # Short-circuit: ANSI stripped, but no marker afterwards -> None.
    assert parse_line('\x1b[31mjust color\x1b[0m', adapter=adapter) is None


def test_build_forest_forwards_adapter() -> None:
    text = '\n'.join(
        [
            'I (1) t: TRACE 1 enter <span=1 parent=none task=main span-name=s target=t> <context=run session=x>',
            'I (2) t: TRACE 2 exit <span=1 task=main>',
        ]
    )
    forest = build_forest(text, adapter=strip_ansi)
    assert [root.id for root in forest.roots] == [1]
    assert forest.spans[1].duration_ms == 1
