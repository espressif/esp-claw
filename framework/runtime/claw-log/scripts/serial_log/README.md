# serial-log

Stream complete log lines from a subprocess (e.g. `idf.py monitor`) to per-line
callbacks. ANSI color is stripped by default and the byte decoding is
configurable. Framework-agnostic and **dependency-free**: how you produce the
logs (any CLI command) and how you consume the lines (a parser, a test harness,
a file) is entirely up to you.

```text
[ any command ] --stdout(lines)--> [ LogStream ] --on_log(line)--> [ your code ]
  idf.py monitor                     strips color
  a serial reader                    decodes bytes
  cat device.log                     splits complete lines
```

## Why push, not a `for` loop

A live device emits lines over time. Instead of a blocking `for line in ...`,
you register an `on_log(line)` callback; a background thread drains stdout and
delivers each complete line, leaving your main thread free.

## Install

```bash
pip install serial-log
```

Requires Python ≥ 3.11. No runtime dependencies.

## Usage

```python
from serial_log import LogStream

def on_log(line: str) -> None:
    print(line)

# Context manager: enter starts the child, exit stops it.
with LogStream(['idf.py', 'monitor'], on_log) as stream:
    stream.wait()        # block until the child exits, or do other work
```

Explicit lifecycle and multiple callbacks:

```python
stream = LogStream('idf.py monitor')   # str is shlex-split (no shell)
stream.on_log(parser.feed)             # register BEFORE start to not miss lines
unsubscribe = stream.on_log(print)
stream.start()
...
unsubscribe()
stream.stop()
```

## Options

| Argument | Default | Meaning |
|----------|---------|---------|
| `command` | — | Program + args; a `str` is `shlex.split` (POSIX, no shell). |
| `on_log` | `None` | First callback, registered before the child starts. |
| `strip_color` | `True` | Remove ANSI color/control escapes from each line. |
| `encoding` | `'utf-8'` | Encoding used to decode the child's bytes. |
| `errors` | `'replace'` | Decode error policy; `'replace'` never raises. |
| `cwd` | `None` | Working directory for the child. |
| `env` | `None` | Environment for the child (inherits parent when `None`). |
| `merge_stderr` | `True` | Fold stderr into the same line stream. |

## Semantics

- **Complete lines only.** Splits on `\n`; a trailing `\r` is dropped, so
  `\r\n` (ESP-IDF) and `\n` (host) both work, even across read boundaries. A
  final line without a newline is emitted at EOF.
- **Backpressure, not loss.** Callbacks run synchronously on the reader thread;
  a slow callback lets the OS pipe fill and the child block — no lines dropped.
- **Failures surface.** If a callback raises, the stream stops reading,
  terminates the child, and re-raises from `wait()` (or on context-manager
  exit). Inspect `LogStream.exception` for the captured error.
- **Single-use.** Calling `start()` twice raises; create a new instance.

## Develop

```bash
uv sync
uv run pytest
uv run ruff format
```
