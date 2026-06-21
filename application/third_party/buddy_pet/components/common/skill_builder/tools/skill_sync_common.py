#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import os
import sys
from typing import TextIO


COLOR_GREEN = '\033[32m'
COLOR_RED = '\033[31m'
COLOR_YELLOW = '\033[33m'
COLOR_CYAN = '\033[36m'
COLOR_RESET = '\033[0m'


class SkillSyncConsole:
    def _colorize(self, text: str, color: str, stream: TextIO) -> str:
        if os.environ.get('NO_COLOR'):
            return text
        return f'{color}{text}{COLOR_RESET}'

    def _write(self, message: str, color: str, stream: TextIO) -> None:
        print(self._colorize(message, color, stream), file=stream)

    def info(self, message: str) -> None:
        self._write(message, COLOR_CYAN, sys.stdout)

    def success(self, message: str) -> None:
        self._write(message, COLOR_GREEN, sys.stdout)

    def warning(self, message: str) -> None:
        self._write(message, COLOR_YELLOW, sys.stderr)

    def error(self, message: str) -> None:
        self._write(message, COLOR_RED, sys.stderr)
