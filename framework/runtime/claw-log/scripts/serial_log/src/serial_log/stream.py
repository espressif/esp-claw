"""Stream complete log lines from a subprocess to per-line callbacks.

The single public type is :class:`LogStream`. It spawns a child process (e.g.
``idf.py monitor``) and drains its stdout on a background thread, delivering one
complete, newline-stripped line at a time to every registered callback. ANSI
color escapes are removed by default and the byte decoding is configurable.

The model is intentionally **push, not pull**: a live device emits lines over
time, so callers register an ``on_log(line)`` callback rather than iterating a
blocking ``for`` loop, leaving the main thread free.
"""

from __future__ import annotations

import re
import shlex
import subprocess
import threading
from collections.abc import Callable, Mapping, Sequence
from types import TracebackType
from typing import Optional

# Per-line callback: receives one complete, decoded, newline-stripped line.
LogCallback = Callable[[str], None]

# Cancels a previously registered callback. Calling it twice is a no-op.
Unsubscribe = Callable[[], None]

# One raw read() per iteration is delivered as soon as the OS has bytes, which
# keeps delivery latency low for an interactive monitor.
_READ_SIZE = 4096

# CSI / SGR escape sequences (color, cursor moves, line clears) an interactive
# monitor or a TTY-aware logger may emit. Stripping all of CSI — not only the
# ``m`` color terminator — keeps the delivered text clean.
_ANSI_RE = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def _strip_ansi(text: str) -> str:
    """Remove ANSI CSI escape sequences from ``text``."""
    return _ANSI_RE.sub('', text)


class LogStream:
    """Spawn a subprocess and push each complete stdout line to callbacks.

    The child is started by :meth:`start` (or by entering the object as a
    context manager) and torn down by :meth:`stop`. A background reader thread
    splits stdout into complete lines on ``\\n`` (a trailing ``\\r`` is dropped,
    so ``\\r\\n`` is handled), decodes them, optionally strips ANSI color, and
    invokes every registered callback synchronously in registration order.

    Callbacks run on the reader thread; a slow callback applies natural
    backpressure (the OS pipe fills and the child blocks) rather than dropping
    lines. If a callback raises, the stream stops reading, terminates the child,
    and re-raises the exception from :meth:`wait` (or on context-manager exit).

    Register callbacks **before** :meth:`start` to avoid missing the first
    lines; the constructor's ``on_log`` argument is the convenient way to do so.

    Args:
        command: Program and arguments. A ``str`` is split with :func:`shlex.split`
            (POSIX rules, no shell); a sequence is used as-is.
        on_log: Optional first callback, registered before the child starts.
        strip_color: Remove ANSI color/control escapes from each line (default
            ``True``).
        encoding: Text encoding used to decode the child's bytes.
        errors: Decode error policy passed to :meth:`bytes.decode` (``'replace'``
            never raises on malformed bytes).
        cwd: Working directory for the child process.
        env: Environment for the child process (inherits the parent's when
            ``None``).
        merge_stderr: Fold the child's stderr into the same line stream
            (default ``True``); when ``False`` stderr is discarded.

    Examples:
        >>> def on_log(line: str) -> None:
        ...     print(line)
        >>> with LogStream(['idf.py', 'monitor'], on_log) as stream:  # doctest: +SKIP
        ...     stream.wait()
    """

    def __init__(
        self,
        command: Sequence[str] | str,
        on_log: Optional[LogCallback] = None,
        *,
        strip_color: bool = True,
        encoding: str = 'utf-8',
        errors: str = 'replace',
        cwd: Optional[str] = None,
        env: Optional[Mapping[str, str]] = None,
        merge_stderr: bool = True,
    ) -> None:
        self._command: list[str] = (
            shlex.split(command) if isinstance(command, str) else list(command)
        )
        self._strip_color = strip_color
        self._encoding = encoding
        self._errors = errors
        self._cwd = cwd
        self._env = dict(env) if env is not None else None
        self._merge_stderr = merge_stderr

        self._callbacks: list[LogCallback] = []
        if on_log is not None:
            self._callbacks.append(on_log)

        self._lock = threading.Lock()
        self._process: Optional[subprocess.Popen[bytes]] = None
        self._reader: Optional[threading.Thread] = None
        self._started = False
        self._exception: Optional[Exception] = None

    def on_log(self, callback: LogCallback) -> Unsubscribe:
        """Register an additional per-line callback.

        Returns a function that unregisters the callback; calling it more than
        once is harmless. Registering after :meth:`start` may miss early lines.
        """
        with self._lock:
            self._callbacks.append(callback)

        def unsubscribe() -> None:
            with self._lock:
                if callback in self._callbacks:
                    self._callbacks.remove(callback)

        return unsubscribe

    def start(self) -> 'LogStream':
        """Spawn the child process and begin streaming on a background thread.

        Returns ``self`` for chaining. Raises :class:`RuntimeError` if already
        started (a :class:`LogStream` is single-use).
        """
        if self._started:
            raise RuntimeError('LogStream already started; create a new instance')
        self._started = True

        stderr = subprocess.STDOUT if self._merge_stderr else subprocess.DEVNULL
        self._process = subprocess.Popen(
            self._command,
            stdout=subprocess.PIPE,
            stderr=stderr,
            cwd=self._cwd,
            env=self._env,
            bufsize=0,  # unbuffered: deliver bytes as soon as the child writes
        )
        self._reader = threading.Thread(
            target=self._run, name='serial-log-reader', daemon=True
        )
        self._reader.start()
        return self

    def stop(self, timeout: Optional[float] = None) -> Optional[int]:
        """Terminate the child (if running) and join the reader thread.

        Idempotent. Returns the child's exit code, or ``None`` if it never
        started. Does not re-raise a callback exception (use :meth:`wait` or the
        context manager for that); inspect :attr:`exception` if needed.
        """
        process = self._process
        if process is not None and process.poll() is None:
            process.terminate()
        reader = self._reader
        if reader is not None and reader.is_alive():
            reader.join(timeout)
        if process is not None:
            return process.poll()
        return None

    def wait(self, timeout: Optional[float] = None) -> Optional[int]:
        """Block until the child exits (its stdout reaches EOF), then return its
        exit code.

        Re-raises any exception a callback raised on the reader thread. Returns
        ``None`` if the stream was never started.
        """
        reader = self._reader
        if reader is not None:
            reader.join(timeout)
        process = self._process
        returncode = process.wait(timeout) if process is not None else None
        if self._exception is not None:
            raise self._exception
        return returncode

    @property
    def returncode(self) -> Optional[int]:
        """The child's exit code, or ``None`` while it is still running or was
        never started."""
        return self._process.poll() if self._process is not None else None

    @property
    def exception(self) -> Optional[Exception]:
        """The exception a callback raised on the reader thread, if any."""
        return self._exception

    def __enter__(self) -> 'LogStream':
        return self.start()

    def __exit__(
        self,
        exc_type: Optional[type[BaseException]],
        exc: Optional[BaseException],
        traceback: Optional[TracebackType],
    ) -> None:
        self.stop()
        # Surface a callback failure even when the caller never called wait(),
        # unless an exception is already propagating from the with-block.
        if exc_type is None and self._exception is not None:
            raise self._exception

    def _run(self) -> None:
        """Reader-thread entry point: drain stdout, dispatch lines, capture any
        callback failure and tear the child down so :meth:`wait` can re-raise."""
        try:
            self._read_loop()
        except Exception as error:  # callback failure or read error
            self._exception = error
            process = self._process
            if process is not None and process.poll() is None:
                process.terminate()

    def _read_loop(self) -> None:
        process = self._process
        if process is None or process.stdout is None:
            return
        stream = process.stdout
        buffer = b''
        while True:
            chunk = stream.read(_READ_SIZE)
            if not chunk:
                break
            buffer += chunk
            buffer = self._drain_lines(buffer)
        # Emit a trailing line that lacks a final newline.
        if buffer:
            self._dispatch(buffer)

    def _drain_lines(self, buffer: bytes) -> bytes:
        """Emit every complete (``\\n``-terminated) line in ``buffer`` and return
        the incomplete remainder. Splitting on ``\\n`` only (with a trailing
        ``\\r`` dropped per line) is safe across read-chunk boundaries."""
        start = 0
        while True:
            newline = buffer.find(b'\n', start)
            if newline == -1:
                break
            self._dispatch(buffer[start:newline])
            start = newline + 1
        return buffer[start:]

    def _dispatch(self, raw_line: bytes) -> None:
        line = raw_line.rstrip(b'\r').decode(self._encoding, self._errors)
        if self._strip_color:
            line = _strip_ansi(line)
        with self._lock:
            callbacks = tuple(self._callbacks)
        for callback in callbacks:
            callback(line)
