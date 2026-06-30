"""Tests for :class:`serial_log.LogStream`.

Each test drives a short Python child process (via ``sys.executable -c ...``) so
the byte stream is deterministic and no real device is needed. ``wait()`` is
always called before asserting so the reader thread has fully drained.
"""

from __future__ import annotations

import sys

import pytest

from serial_log import LogStream


def _py(code: str) -> list[str]:
    """A child command that runs ``code`` with the current interpreter."""
    return [sys.executable, '-c', code]


def _collect(command: list[str], **kwargs) -> list[str]:
    """Run ``command`` to completion, returning every dispatched line."""
    lines: list[str] = []
    stream = LogStream(command, lines.append, **kwargs)
    stream.start()
    stream.wait()
    return lines


def test_captures_complete_lines_in_order() -> None:
    lines = _collect(_py('print("first"); print("second"); print("third")'))
    assert lines == ['first', 'second', 'third']


def test_strips_ansi_color_by_default() -> None:
    code = r'print("\x1b[32mgreen\x1b[0m and \x1b[1mbold\x1b[0m")'
    lines = _collect(_py(code))
    assert lines == ['green and bold']


def test_keeps_ansi_when_strip_color_disabled() -> None:
    code = r'print("\x1b[32mgreen\x1b[0m")'
    lines = _collect(_py(code), strip_color=False)
    assert lines == ['\x1b[32mgreen\x1b[0m']


def test_handles_crlf_line_endings() -> None:
    code = r'import sys; sys.stdout.buffer.write(b"a\r\nb\r\nc\r\n")'
    lines = _collect(_py(code))
    assert lines == ['a', 'b', 'c']


def test_emits_trailing_line_without_newline() -> None:
    code = r'import sys; sys.stdout.write("done\npartial")'
    lines = _collect(_py(code))
    assert lines == ['done', 'partial']


def test_split_across_read_chunks_does_not_break_lines() -> None:
    # A line longer than one read() still arrives intact, and a CRLF straddling
    # a chunk boundary is not mistaken for an empty line.
    code = (
        r'import sys; '
        r'sys.stdout.buffer.write(b"x" * 10000 + b"\r\n" + b"tail\r\n")'
    )
    lines = _collect(_py(code))
    assert lines == ['x' * 10000, 'tail']


def test_custom_encoding() -> None:
    code = r'import sys; sys.stdout.buffer.write("café\n".encode("latin-1"))'
    lines = _collect(_py(code), encoding='latin-1')
    assert lines == ['café']


def test_merge_stderr_true_includes_stderr() -> None:
    code = (
        r'import sys; '
        r'sys.stdout.write("out\n"); sys.stdout.flush(); '
        r'sys.stderr.write("err\n")'
    )
    lines = _collect(_py(code), merge_stderr=True)
    assert set(lines) == {'out', 'err'}


def test_merge_stderr_false_discards_stderr() -> None:
    code = (
        r'import sys; '
        r'sys.stdout.write("out\n"); sys.stdout.flush(); '
        r'sys.stderr.write("err\n")'
    )
    lines = _collect(_py(code), merge_stderr=False)
    assert lines == ['out']


def test_multiple_callbacks_and_unsubscribe() -> None:
    seen_a: list[str] = []
    seen_b: list[str] = []
    stream = LogStream(_py('print("only")'), seen_a.append)
    unsubscribe = stream.on_log(seen_b.append)
    unsubscribe()
    unsubscribe()  # second call is a no-op
    stream.start()
    stream.wait()
    assert seen_a == ['only']
    assert seen_b == []


def test_wait_returns_child_exit_code() -> None:
    stream = LogStream(_py('import sys; print("bye"); sys.exit(3)'))
    stream.start()
    assert stream.wait() == 3
    assert stream.returncode == 3


def test_string_command_is_shlex_split() -> None:
    command = f'{sys.executable} -c \'print("hi")\''
    lines: list[str] = []
    stream = LogStream(command, lines.append)
    stream.start()
    stream.wait()
    assert lines == ['hi']


def test_callback_exception_propagates_from_wait() -> None:
    def boom(_line: str) -> None:
        raise ValueError('callback failed')

    # The child keeps emitting; the raising callback must stop it and surface.
    code = r'import time; print("trigger", flush=True); time.sleep(30)'
    stream = LogStream(_py(code), boom)
    stream.start()
    with pytest.raises(ValueError, match='callback failed'):
        stream.wait()
    assert isinstance(stream.exception, ValueError)
    assert stream.returncode is not None  # child was terminated


def test_double_start_raises() -> None:
    stream = LogStream(_py('print("x")'))
    stream.start()
    stream.wait()
    with pytest.raises(RuntimeError, match='already started'):
        stream.start()


def test_context_manager_happy_path() -> None:
    lines: list[str] = []
    with LogStream(_py('print("ctx")'), lines.append) as stream:
        stream.wait()
    assert lines == ['ctx']


def test_context_manager_surfaces_callback_exception() -> None:
    def boom(_line: str) -> None:
        raise ValueError('ctx failure')

    with pytest.raises(ValueError, match='ctx failure'):
        with LogStream(_py('print("trigger")'), boom) as stream:
            # No explicit wait(): __exit__ must still surface the failure.
            stream._reader.join()  # type: ignore[union-attr]
