#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""
Package firmware build outputs into per-board tar.gz archives with metadata JSON.
Replaces merge_bin.py — no binary merging, preserves individual partition files.
"""

import csv
import datetime
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple


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


def _find_build_dirs(example_dir: Path):
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


def _git_refs() -> str:
    tag = os.getenv('CI_COMMIT_TAG', '').strip()
    if tag:
        return tag

    try:
        res = subprocess.run(
            ['git', 'describe', '--tags', '--long', '--always'],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        desc = res.stdout.strip()
        if desc:
            return desc
    except Exception:
        pass

    try:
        res = subprocess.run(
            ['git', 'rev-parse', '--short', 'HEAD'],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        short_hash = res.stdout.strip()
        if short_hash:
            return short_hash
    except Exception:
        pass

    return datetime.datetime.now().strftime('%Y%m%d_%H%M%S')


def _git_commit_timestamp() -> str:
    try:
        res = subprocess.run(
            ['git', 'log', '-1', '--format=%cI'],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        ts = res.stdout.strip()
        if ts:
            return ts
    except Exception:
        pass
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def _load_flasher_json(build_dir: Path) -> Tuple[Dict, str]:
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

    return data, flash_size.strip()


def _load_sdkconfig_json(build_dir: Path) -> Dict:
    sdkconfig_json = build_dir / 'config' / 'sdkconfig.json'
    if not sdkconfig_json.is_file():
        raise RuntimeError(f'missing sdkconfig.json: {sdkconfig_json}')

    with sdkconfig_json.open('r', encoding='utf-8') as fr:
        sdkconfig = json.load(fr)

    if not isinstance(sdkconfig, dict):
        raise RuntimeError(f'invalid sdkconfig.json object: {sdkconfig_json}')

    return sdkconfig


def _resolve_console_output(sdkconfig: Dict) -> str:
    if sdkconfig.get('ESP_CONSOLE_UART') is True:
        return 'UART'
    if sdkconfig.get('ESP_CONSOLE_USB_SERIAL_JTAG') is True:
        return 'Serial JTAG'
    return 'unknown'


def _resolve_min_psram_size_mb(sdkconfig: Dict) -> int:
    if sdkconfig.get('SPIRAM') is not True:
        return 0
    if sdkconfig.get('SPIRAM_XIP_FROM_PSRAM') is True:
        return 8
    return 4


def _extract_partition_table(build_dir: Path, flasher_data: Dict) -> List[Dict]:
    partition_info = flasher_data.get('partition-table')
    if not isinstance(partition_info, dict):
        raise RuntimeError('partition-table entry missing in flasher_args.json')

    partition_file = partition_info.get('file')
    if not isinstance(partition_file, str) or not partition_file.strip():
        raise RuntimeError('partition-table.file missing or invalid')

    partition_bin = build_dir / partition_file
    if not partition_bin.is_file():
        raise RuntimeError(f'partition-table binary not found: {partition_bin}')

    with tempfile.NamedTemporaryFile(prefix='partition_', suffix='.csv', delete=False) as tf:
        csv_out = Path(tf.name)

    try:
        subprocess.run(
            ['gen_esp32part.py', str(partition_bin), str(csv_out)],
            cwd=build_dir, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        with csv_out.open('r', encoding='utf-8') as fr:
            valid_lines = [line for line in fr if line.strip() and not line.lstrip().startswith('#')]

        entries = []
        reader = csv.reader(valid_lines)
        for row in reader:
            if len(row) < 5:
                continue
            entries.append({
                'name': row[0].strip(),
                'type': row[1].strip(),
                'subtype': row[2].strip(),
                'offset': row[3].strip(),
                'size': row[4].strip(),
            })
        return entries
    finally:
        try:
            csv_out.unlink(missing_ok=True)
        except Exception:
            pass


def _extract_nvs_info(partition_table: List[Dict]) -> Tuple[Optional[str], Optional[str]]:
    for entry in partition_table:
        if entry['name'] == 'nvs':
            offset = _parse_size_to_bytes(entry['offset'])
            size = _parse_size_to_bytes(entry['size'])
            return _to_hex(offset), _to_hex(size)
    return None, None


def _resolve_application_name(example_dir: Path) -> str:
    app_name = os.getenv('EXAMPLE_APP_NAME', '').strip()
    if app_name:
        return app_name
    return example_dir.name


def _create_tarball(build_dir: Path, flasher_data: Dict, out_tar: Path) -> None:
    flash_files = flasher_data.get('flash_files', {})
    flash_args = build_dir / 'flash_args'
    flasher_json = build_dir / 'flasher_args.json'

    with tarfile.open(out_tar, 'w:gz') as tar:
        tar.add(flasher_json, arcname='flasher_args.json')
        tar.add(flash_args, arcname='flash_args')

        for _addr, rel_file in flash_files.items():
            file_path = build_dir / rel_file
            if file_path.is_file():
                tar.add(file_path, arcname=rel_file)


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

    output_dir = Path.cwd() / 'firmware_output'
    output_dir.mkdir(parents=True, exist_ok=True)

    refs = _git_refs()
    commit_ts = _git_commit_timestamp()
    application = _resolve_application_name(example_dir)
    success_count = 0

    for build_dir in build_dirs:
        _log(f'Processing build directory: {build_dir}')
        try:
            flasher_data, flash_size = _load_flasher_json(build_dir)
            sdkconfig = _load_sdkconfig_json(build_dir)
            console_output = _resolve_console_output(sdkconfig)
            partition_table = _extract_partition_table(build_dir, flasher_data)
            nvs_start, nvs_size = _extract_nvs_info(partition_table)

            flash_files = flasher_data.get('flash_files', {})
            flash_settings = flasher_data.get('flash_settings', {})

            safe_console = console_output.replace(' ', '_').lower()
            basename = f'{board}__{safe_console}'
            out_tar = output_dir / f'{basename}.tar.gz'
            out_json = output_dir / f'{basename}.json'

            _create_tarball(build_dir, flasher_data, out_tar)

            metadata = {
                'refs': refs,
                'application': application,
                'chip': target,
                'brand': board_brand,
                'board_name': board,
                'console_output': console_output,
                'commit_timestamp': commit_ts,
                'partition_table': partition_table,
                'flash_files': flash_files,
                'flash_settings': flash_settings,
                'min_flash_size': flash_size,
                'min_psram_size': _resolve_min_psram_size_mb(sdkconfig),
            }
            if nvs_start and nvs_size:
                metadata['nvs_info'] = {
                    'start_addr': nvs_start,
                    'size': nvs_size,
                }

            out_json.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

            _log(f'  tar.gz: {out_tar}')
            _log(f'  metadata: {out_json}')
            success_count += 1
        except Exception as e:
            _log(f'  Build directory invalid: {build_dir} ({e})')

    if success_count == 0:
        _log('All build directories are invalid')
        return 1

    _log(f'Packaged {success_count} build(s) successfully')
    return 0


if __name__ == '__main__':
    sys.exit(main())
