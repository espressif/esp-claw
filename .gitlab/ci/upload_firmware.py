#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""
Upload packaged firmware tar.gz files to the version-tool Worker API.
Reads firmware_output/*.tar.gz + *.json pairs produced by package_firmware.py.
"""

import json
import os
import sys
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError


def _log(msg: str) -> None:
    print(msg, file=sys.stderr)


def main() -> int:
    upload_url = os.getenv('FIRMWARE_API_URL', '').strip()
    api_key = os.getenv('FIRMWARE_API_KEY', '').strip()

    if not upload_url:
        _log('FIRMWARE_API_URL is not set')
        return 1
    if not api_key:
        _log('FIRMWARE_API_KEY is not set')
        return 1

    output_dir = Path.cwd() / 'firmware_output'
    if not output_dir.is_dir():
        _log(f'firmware_output directory not found: {output_dir}')
        return 1

    json_files = sorted(output_dir.glob('*.json'))
    if not json_files:
        _log('No metadata JSON files found in firmware_output/')
        return 1

    success_count = 0

    for json_path in json_files:
        tar_path = json_path.with_suffix('.tar.gz')
        if not tar_path.is_file():
            stem = json_path.stem
            tar_path = output_dir / f'{stem}.tar.gz'
            if not tar_path.is_file():
                _log(f'Skip {json_path.name}: matching tar.gz not found')
                continue

        with json_path.open('r', encoding='utf-8') as f:
            metadata = json.load(f)

        _log(f'Uploading {tar_path.name} ({metadata.get("board_name", "?")} / {metadata.get("console_output", "?")})')

        try:
            _upload_multipart(upload_url, api_key, tar_path, metadata)
            success_count += 1
            _log('  OK')
        except Exception as e:
            _log(f'  FAILED: {e}')

    if success_count == 0:
        _log('No uploads succeeded')
        return 1

    _log(f'Uploaded {success_count}/{len(json_files)} firmware package(s)')
    return 0


def _upload_multipart(url: str, api_key: str, tar_path: Path, metadata: dict) -> None:
    boundary = '----FirmwareUploadBoundary7MA4YWxkTrZu0gW'
    body_parts = []

    metadata_json = json.dumps(metadata, ensure_ascii=False)
    body_parts.append(
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="metadata"\r\n'
        f'Content-Type: application/json\r\n'
        f'\r\n'
        f'{metadata_json}\r\n'
    )

    tar_data = tar_path.read_bytes()
    body_parts.append(
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="file"; filename="{tar_path.name}"\r\n'
        f'Content-Type: application/gzip\r\n'
        f'\r\n'
    )

    body = b''
    for part in body_parts[:-1]:
        body += part.encode('utf-8')
    body += body_parts[-1].encode('utf-8')
    body += tar_data
    body += f'\r\n--{boundary}--\r\n'.encode('utf-8')

    req = Request(url, data=body, method='POST')
    req.add_header('Authorization', f'Bearer {api_key}')
    req.add_header('Content-Type', f'multipart/form-data; boundary={boundary}')

    try:
        with urlopen(req, timeout=120) as resp:
            resp_body = resp.read().decode('utf-8')
            if resp.status != 200:
                raise RuntimeError(f'HTTP {resp.status}: {resp_body}')
    except HTTPError as e:
        resp_body = e.read().decode('utf-8', errors='replace')
        raise RuntimeError(f'HTTP {e.code}: {resp_body}') from None


if __name__ == '__main__':
    sys.exit(main())
