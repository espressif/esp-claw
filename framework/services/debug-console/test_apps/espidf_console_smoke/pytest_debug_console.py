# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import os
import subprocess
import time
from pathlib import Path

import pytest
import serial
from serial import SerialException


APP_DIR = Path(__file__).resolve().parent
DEFAULT_FLASH_PORT = '/dev/ttyUSB0'
DEFAULT_USB_CONSOLE_PORT = '/dev/ttyACM0'
PORT_WAIT_SECONDS = 10


@pytest.mark.parametrize(
    'config, expected_backend, port_env',
    [
        ('uart', 'uart', 'DEBUG_CONSOLE_UART_PORT'),
        ('usb_serial_jtag', 'usb_serial_jtag', 'DEBUG_CONSOLE_USB_SERIAL_JTAG_PORT'),
        ('usb_cdc', 'usb_cdc', 'DEBUG_CONSOLE_USB_CDC_PORT'),
    ],
)
def test_debug_console_espidf_backend(config: str, expected_backend: str, port_env: str) -> None:
    flash_port = os.environ.get('DEBUG_CONSOLE_FLASH_PORT') or DEFAULT_FLASH_PORT
    read_port = resolve_read_port(config, port_env, flash_port)
    build_dir = APP_DIR / f'build_{config}'
    expected_lines = [
        f'selected backend: {expected_backend}',
        'debug-console espidf backend initialized',
    ]

    flash_app(build_dir, flash_port)
    output = read_boot_output(read_port, expected_lines)

    for line in expected_lines:
        assert line in output


def resolve_read_port(config: str, port_env: str, flash_port: str) -> str:
    configured_port = os.environ.get(port_env) or os.environ.get('DEBUG_CONSOLE_PORT')
    if configured_port:
        return configured_port
    if config == 'uart':
        return flash_port
    return DEFAULT_USB_CONSOLE_PORT


def flash_app(build_dir: Path, flash_port: str) -> None:
    config = build_dir.name.removeprefix('build_')
    subprocess.run(
        [
            'idf.py',
            '-p',
            flash_port,
            '-B',
            str(build_dir),
            f'-DSDKCONFIG={build_dir / "sdkconfig"}',
            f'-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.{config}',
            'flash',
        ],
        cwd=APP_DIR,
        check=True,
    )


def read_boot_output(port: str, expected_lines: list[str]) -> str:
    wait_for_port(port)
    deadline = time.monotonic() + PORT_WAIT_SECONDS
    chunks: list[str] = []

    try:
        with serial.Serial(port=port, baudrate=115200, timeout=0.2) as stream:
            reset_target(stream)
            while time.monotonic() < deadline:
                data = stream.read(512)
                if data:
                    text = data.decode(errors='replace')
                    chunks.append(text)
                    output = ''.join(chunks)
                    if all(line in output for line in expected_lines):
                        return output
    except SerialException as error:
        raise AssertionError(f'failed to open serial port {port}: {error}') from error

    return ''.join(chunks)


def wait_for_port(port: str) -> None:
    deadline = time.monotonic() + PORT_WAIT_SECONDS
    while time.monotonic() < deadline:
        if Path(port).exists():
            return
        time.sleep(0.2)

    raise AssertionError(
        f'serial port {port} did not appear; set DEBUG_CONSOLE_FLASH_PORT and the per-backend '
        'DEBUG_CONSOLE_*_PORT env vars when flash and console use different USB devices'
    )


def reset_target(stream: serial.Serial) -> None:
    stream.dtr = False
    stream.rts = True
    time.sleep(0.1)
    stream.rts = False
    time.sleep(0.2)
