"""Glue tool: capture a live TRACE log from a monitor command, export Chrome trace.

This package wires three independent pieces together and owns no parsing or
streaming logic of its own:

- ``serial_log.LogStream`` runs the monitor command and pushes complete lines,
- ``claw_trace`` parses each line and reconstructs the span forest,
- ``chrome_export`` writes the forest to Chrome Trace Event JSON.

The CLI entry point lives in :mod:`trace_capture.__main__` (``claw-trace-capture``).
"""

from __future__ import annotations
