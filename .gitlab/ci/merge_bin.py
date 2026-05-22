#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import csv
import datetime
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, Tuple


def _log(msg: str) -> None:
    print(msg, file=sys.stderr)


def _resolve_example_dir() -> Path:
    example_dir_raw = os.getenv('EXAMPLE_DIR', '').strip()
    if not example_dir_raw:
        raise RuntimeError('EXAMPLE_DIR is not set')

    example_dir = Path(example_dir_raw)
    if not example_dir.is_absolute():
        example_dir = Path.cwd() / example_dir

    example_dir = example_dir.resolve()
    if not example_dir.is_dir():
        raise RuntimeError(f'EXAMPLE_DIR does not exist or is not a directory: {example_dir}')

    return example_dir


def _find_build_dirs(example_dir: Path) -> Iterable[Path]:
    build_dirs = []
    for path in example_dir.rglob('*'):
        if not path.is_dir():
            continue
        if not path.name.startswith('build'):
            continue
        rel_parts = path.relative_to(example_dir).parts
        if 1 <= len(rel_parts) <= 2:
            build_dirs.append(path)

    return sorted(build_dirs, key=lambda p: str(p.relative_to(example_dir)))


def _parse_size_to_bytes(value: str) -> int:
    text = value.strip()
    if not text:
        raise ValueError('empty size field')

    upper = text.upper()
    if upper.endswith('K'):
        return int(upper[:-1], 0) * 1024
    if upper.endswith('M'):
        return int(upper[:-1], 0) * 1024 * 1024

    return int(text, 0)


def _to_hex(value: int) -> str:
    return f'0x{value:x}'


def _git_description() -> str:
    try:
        res = subprocess.run(
            ['git', 'describe', '--tags', '--long', '--always'],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        desc = res.stdout.strip()
        if desc:
            return desc
    except Exception:
        pass

    try:
        res = subprocess.run(
            ['git', 'rev-parse', '--short', 'HEAD'],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        short_hash = res.stdout.strip()
        if short_hash:
            return short_hash
    except Exception:
        pass

    return datetime.datetime.now().strftime('%Y%m%d_%H%M%S')


def _load_flasher_json(build_dir: Path) -> Tuple[Dict, str, str]:
    flash_args = build_dir / 'flash_args'
    flasher_args_json = build_dir / 'flasher_args.json'

    if not flash_args.is_file():
        raise RuntimeError(f'missing flash_args: {flash_args}')
    if not flasher_args_json.is_file():
        raise RuntimeError(f'missing flasher_args.json: {flasher_args_json}')

    with flasher_args_json.open('r', encoding='utf-8') as fr:
        data = json.load(fr)

    flash_settings = data.get('flash_settings')
    if not isinstance(flash_settings, dict):
        raise RuntimeError('invalid flash_settings in flasher_args.json')

    flash_size = flash_settings.get('flash_size')
    if not isinstance(flash_size, str) or not flash_size.strip():
        raise RuntimeError('flash_settings.flash_size not found or invalid')

    flash_files = data.get('flash_files')
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError('flash_files not found or invalid')

    for addr, rel_file in flash_files.items():
        if not isinstance(rel_file, str) or not rel_file.strip():
            raise RuntimeError(f'flash_files[{addr!r}] is invalid')
        bin_path = build_dir / rel_file
        if not bin_path.is_file():
            raise RuntimeError(f'flash file not found: {bin_path}')

    partition_info = data.get('partition-table')
    if not isinstance(partition_info, dict):
        raise RuntimeError('partition-table entry missing in flasher_args.json')

    partition_file = partition_info.get('file')
    if not isinstance(partition_file, str) or not partition_file.strip():
        raise RuntimeError('partition-table.file missing or invalid')

    partition_bin = build_dir / partition_file
    if not partition_bin.is_file():
        raise RuntimeError(f'partition-table binary not found: {partition_bin}')

    return data, flash_size.strip(), partition_file


def _extract_nvs_info(build_dir: Path, partition_file: str) -> Tuple[str, str]:
    partition_bin = build_dir / partition_file
    with tempfile.NamedTemporaryFile(prefix='partition_', suffix='.csv', delete=False) as tf:
        csv_out = Path(tf.name)

    try:
        subprocess.run(
            ['gen_esp32part.py', str(partition_bin), str(csv_out)],
            cwd=build_dir,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        with csv_out.open('r', encoding='utf-8') as fr:
            valid_lines = [line for line in fr if line.strip() and not line.lstrip().startswith('#')]

        reader = csv.reader(valid_lines)
        for row in reader:
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name != 'nvs':
                continue

            offset = _parse_size_to_bytes(row[3].strip())
            size = _parse_size_to_bytes(row[4].strip())
            return _to_hex(offset), _to_hex(size)

        raise RuntimeError('nvs partition not found in converted partition csv')
    finally:
        try:
            csv_out.unlink(missing_ok=True)
        except Exception:
            pass


def _merge_bin(build_dir: Path, target: str, out_bin: Path) -> None:
    subprocess.run(
        [
            'esptool.py',
            '--chip',
            target,
            'merge_bin',
            '-o',
            str(out_bin),
            '@flash_args',
        ],
        cwd=build_dir,
        check=True,
    )


def _load_sdkconfig_json(build_dir: Path) -> Dict:
    sdkconfig_json = build_dir / 'config' / 'sdkconfig.json'
    if not sdkconfig_json.is_file():
        raise RuntimeError(f'missing sdkconfig.json: {sdkconfig_json}')

    with sdkconfig_json.open('r', encoding='utf-8') as fr:
        sdkconfig = json.load(fr)

    if not isinstance(sdkconfig, dict):
        raise RuntimeError(f'invalid sdkconfig.json object: {sdkconfig_json}')

    return sdkconfig


def _resolve_rev_and_suffix(sdkconfig: Dict, target: str) -> Tuple[int, str]:
    if target != 'esp32p4':
        raise RuntimeError(f'rev resolution is only supported for esp32p4, got: {target}')

    selects_rev_less_v3 = sdkconfig.get('ESP32P4_SELECTS_REV_LESS_V3')
    if selects_rev_less_v3 is True:
        return 1, '__rev1'

    return 3, '__rev3'


def _resolve_console_output(sdkconfig: Dict) -> str:
    if sdkconfig.get('ESP_CONSOLE_UART') is True:
        return 'UART'
    if sdkconfig.get('ESP_CONSOLE_USB_SERIAL_JTAG') is True:
        return 'Serial JTAG'
    return 'unknown'


def _console_output_filename_suffix(console_output: str) -> str:
    if console_output == 'UART':
        return '__uart'
    if console_output == 'Serial JTAG':
        return '__serial_jtag'
    return '__unknown'


def _write_output_json(
    out_json: Path,
    board: str,
    board_brand: str,
    chip: str,
    rev: int,
    merged_binary_name: str,
    console_output: str,
    flash_size: str,
    nvs_start: str,
    nvs_size: str,
) -> None:
    payload = {
        'board': board,
        'board_brand': board_brand,
        'chip': chip,
        'console_output': console_output,
        'merged_binary': merged_binary_name,
        'min_flash_size': flash_size,
        'nvs_info': {
            'start_addr': nvs_start,
            'size': nvs_size,
        },
    }
    if chip == 'esp32p4':
        payload['rev'] = rev

    out_json.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main() -> int:
    board = os.getenv('EXAMPLE_BOARD', '').strip()
    if not board:
        _log('EXAMPLE_BOARD is not set')
        return 1

    board_brand = os.getenv('EXAMPLE_BOARD_BRAND', '').strip() or 'others'

    target = os.getenv('EXAMPLE_TARGET', '').strip()
    if not target:
        _log('EXAMPLE_TARGET is not set')
        return 1

    try:
        example_dir = _resolve_example_dir()
    except Exception as e:
        _log(str(e))
        return 1

    build_dirs = list(_find_build_dirs(example_dir))
    if not build_dirs:
        _log(f'No build directories found under (depth<=2): {example_dir}')
        return 1

    merge_dir = Path.cwd() / 'merged_binary'
    merge_dir.mkdir(parents=True, exist_ok=True)

    git_desc = _git_description()
    success_count = 0

    for build_dir in build_dirs:
        _log(f'Processing build directory: {build_dir}')
        try:
            _, flash_size, partition_file = _load_flasher_json(build_dir)
            sdkconfig = _load_sdkconfig_json(build_dir)
            rev = 0
            filename_suffix = ''
            console_output = _resolve_console_output(sdkconfig)
            if target == 'esp32p4':
                rev, filename_suffix = _resolve_rev_and_suffix(sdkconfig, target)
            filename_suffix += _console_output_filename_suffix(console_output)
            out_basename = f'{board}__{git_desc}{filename_suffix}'
            out_bin = merge_dir / f'{out_basename}.bin'
            out_json = merge_dir / f'{out_basename}.json'
            nvs_start, nvs_size = _extract_nvs_info(build_dir, partition_file)
            _merge_bin(build_dir, target, out_bin)
            _write_output_json(
                out_json=out_json,
                board=board,
                board_brand=board_brand,
                chip=target,
                rev=rev,
                merged_binary_name=out_bin.name,
                console_output=console_output,
                flash_size=flash_size,
                nvs_start=nvs_start,
                nvs_size=nvs_size,
            )
            _log(f'Success with build directory: {build_dir}')
            _log(f'Merged binary: {out_bin}')
            _log(f'Metadata json: {out_json}')
            success_count += 1
        except Exception as e:
            _log(f'Build directory invalid: {build_dir} ({e})')

    if success_count == 0:
        _log('All build directories are invalid')
        return 1

    _log(f'Processed {success_count} build directory(s) successfully')
    return 0


if __name__ == '__main__':
    sys.exit(main())
