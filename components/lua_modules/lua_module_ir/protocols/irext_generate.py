#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Generate JSON protocol codesets and a pulse oracle from the pinned IRext new_ac binaries.

The IRext model mirrors libirdecode (non apply-table build) and is checked pulse for pulse
against the library built from the pinned core checkout. Each codeset is then described as
template + disjoint bit fields + checks by black-box comparison of model payloads, and every
legal state is re-encoded from the emitted JSON with a mirror of the C encoder.
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter, defaultdict
from itertools import product
from pathlib import Path


TAG_IDS = (1, 2, 3, 4, 5, 6, 7, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 41, 42, 43, 44, 45, 46, 47, 48)
MODE_NAMES = ('cool', 'heat', 'auto', 'fan', 'dry')
FAN_NAMES = ('auto', 'low', 'medium', 'high')
ID_RE = re.compile(r'^([a-z][a-z0-9_]*)\.irext_([1-9][0-9]*)$')
MAX_U16 = 0xFFFF
# Binaries shared by more brands are generic codes and skipped unless the brand lifts the cap.
MAX_SHARED_BRANDS = 3
NO_SHARED_LIMIT = None
# (key, brand_id, database name, max brands sharing one binary)
BRAND_SELECTION = (
    ('gree', 4, '格力', MAX_SHARED_BRANDS),
    ('midea', 9, '美的', MAX_SHARED_BRANDS),
    ('haier', 5, '海尔', MAX_SHARED_BRANDS),
    ('hisense', 6, '海信', MAX_SHARED_BRANDS),
    ('daikin', 84, '大金', MAX_SHARED_BRANDS),
    ('mitsubishi_electric', 1052, '三菱电机', MAX_SHARED_BRANDS),
    ('panasonic', 85, '松下', MAX_SHARED_BRANDS),
    ('lg', 882, 'LG 乐金', MAX_SHARED_BRANDS),
    ('chigo', 11, '志高', NO_SHARED_LIMIT),
    ('kelon', 8, '科龙', MAX_SHARED_BRANDS),
    ('changhong', 2, '长虹', MAX_SHARED_BRANDS),
    ('galanz', 86, '格兰仕', MAX_SHARED_BRANDS),
    ('chunlan', 3, '春兰', MAX_SHARED_BRANDS),
)
BRAND_DISPLAY = {
    'gree': 'Gree', 'midea': 'Midea', 'haier': 'Haier', 'hisense': 'Hisense', 'daikin': 'Daikin',
    'mitsubishi_electric': 'Mitsubishi Electric', 'panasonic': 'Panasonic', 'lg': 'LG',
    'chigo': 'Chigo', 'kelon': 'Kelon', 'changhong': 'Changhong', 'galanz': 'Galanz', 'chunlan': 'Chunlan',
}
# Database candidates per brand; the manifest pins an ordered subset of them.
SELECTION_PER_BRAND = 5

CARRIER_HZ = 38000
TEMP_BASE_C = 16
TEMP_COUNT = 15
DECI = 10
# ir_fields.h enums
MODE_ENUM = {'auto': 0, 'cool': 1, 'dry': 2, 'heat': 3, 'fan': 4}
FAN_ENUM = {'auto': 0, 'low': 2, 'medium': 3, 'high': 4}
SWING_AUTO = 1
# IRext function ids 1..7, ir_fields.h key enum = id - 1, libirdecode key_code per id
KEY_NAMES = ('power', 'mode', 'temp_up', 'temp_down', 'fan', 'swing_v', 'swing_v_fix')
KEY_CODES = (0, 1, 2, 3, 9, 10, 11)
F_POWER, F_MODE, F_TEMP_UP, F_TEMP_DOWN, F_FAN, F_SWING, F_FIX = range(1, 8)
FIELD_MAX_WIDTH = 32
STATE_KEY = {'mode': 'mode', 'temperature': 'temp', 'fan': 'fan', 'swing_v': 'swing', 'key': 'key'}
ENUM_DEDUP_FIELDS = ('mode', 'fan', 'swing_v')


class GenerationError(Exception):
    pass


class ACStatus(ctypes.Structure):
    _fields_ = [
        ('power', ctypes.c_int),
        ('temperature', ctypes.c_int),
        ('mode', ctypes.c_int),
        ('swing', ctypes.c_int),
        ('fan', ctypes.c_int),
        ('display', ctypes.c_ubyte),
        ('sleep', ctypes.c_ubyte),
        ('timer', ctypes.c_ubyte),
        ('change_direction', ctypes.c_ubyte),
    ]


def fail(message):
    raise GenerationError(message)


# ---------------------------------------------------------------------------
# Pinned sources

def run_git(repo, *args):
    try:
        return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(f"{repo}: git {' '.join(args)} failed: {exc}")


def verify_repo(repo, expected_commit, label):
    if not repo.is_dir():
        fail(f'{label} repository does not exist: {repo}')
    if run_git(repo, 'rev-parse', 'HEAD') != expected_commit:
        fail(f'{label} repository is not at pinned commit {expected_commit}')
    if run_git(repo, 'status', '--porcelain', '--untracked-files=all'):
        fail(f'{label} repository is not clean: {repo}')


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def require_dict_keys(value, required, where):
    if not isinstance(value, dict):
        fail(f'{where}: expected object')
    missing = required - value.keys()
    extra = value.keys() - required
    if missing or extra:
        fail(f'{where}: invalid keys; missing={sorted(missing)}, extra={sorted(extra)}')


def load_manifest(path):
    try:
        manifest = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f'cannot read manifest {path}: {exc}')
    top_keys = {'core_commit', 'core_version', 'database_commit', 'database_sha256', 'archive_sha256', 'profiles'}
    require_dict_keys(manifest, top_keys, 'manifest')
    for key in ('core_commit', 'database_commit'):
        if not isinstance(manifest[key], str) or not re.fullmatch(r'[0-9a-f]{40}', manifest[key]):
            fail(f'manifest.{key}: expected lowercase 40-character commit')
    for key in ('database_sha256', 'archive_sha256'):
        if not isinstance(manifest[key], str) or not re.fullmatch(r'[0-9a-f]{64}', manifest[key]):
            fail(f'manifest.{key}: expected lowercase SHA-256')
    if not isinstance(manifest['core_version'], str) or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', manifest['core_version']):
        fail('manifest.core_version: expected semantic numeric version')
    profiles = manifest['profiles']
    if not isinstance(profiles, list) or not profiles:
        fail('manifest.profiles: expected a non-empty list')
    required = {'id', 'brand', 'brand_id', 'remote_index', 'remote', 'priority', 'md5', 'sha256'}
    brands = Counter()
    seen = {'id': set(), 'remote_index': set(), 'md5': set()}
    for index, profile in enumerate(profiles):
        where = f'manifest.profiles[{index}]'
        require_dict_keys(profile, required, where)
        match = ID_RE.fullmatch(profile['id']) if isinstance(profile['id'], str) else None
        if not match or match.group(1) != profile['brand'] or match.group(2) != profile['remote']:
            fail(f'{where}: id, brand, and remote do not agree')
        if profile['brand'] not in BRAND_DISPLAY:
            fail(f"{where}: unknown brand {profile['brand']}")
        for key in ('brand_id', 'remote_index', 'priority'):
            if type(profile[key]) is not int or profile[key] < 0:
                fail(f'{where}.{key}: expected non-negative integer')
        if not isinstance(profile['md5'], str) or not re.fullmatch(r'[0-9a-f]{32}', profile['md5']):
            fail(f'{where}.md5: expected lowercase MD5')
        if not isinstance(profile['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', profile['sha256']):
            fail(f'{where}.sha256: expected lowercase SHA-256')
        for key, values in seen.items():
            if profile[key] in values:
                fail(f'{where}: duplicate global {key} {profile[key]}')
            values.add(profile[key])
        brands[profile['brand']] += 1
    if max(brands.values()) > SELECTION_PER_BRAND:
        fail(f'manifest.profiles: expected at most {SELECTION_PER_BRAND} entries per brand, got {dict(brands)}')
    return manifest


def verify_database(manifest, database_repo):
    database = database_repo / 'db/irext_db_20260929_sqlite3.db'
    archive = database_repo / 'binaries/irext-binaries_20260929.zip'
    if not database.is_file() or file_sha256(database) != manifest['database_sha256']:
        fail(f'SQLite artifact is missing or does not match manifest SHA-256: {database}')
    if not archive.is_file() or file_sha256(archive) != manifest['archive_sha256']:
        fail(f'binary archive is missing or does not match manifest SHA-256: {archive}')
    binaries = {}
    with sqlite3.connect(f'file:{database}?mode=ro', uri=True) as connection, zipfile.ZipFile(archive) as bundle:
        selected_by_index = {}
        selected_md5 = set()
        manifest_by_brand = defaultdict(list)
        for profile in manifest['profiles']:
            manifest_by_brand[profile['brand']].append(profile)
        for brand, brand_id, brand_name, max_shared in BRAND_SELECTION:
            brand_row = connection.execute('SELECT name, category_id, status FROM brand WHERE id = ?', (brand_id,)).fetchone()
            if brand_row != (brand_name, 1, 1):
                fail(f'{brand}: brand mapping mismatch for id {brand_id}: {brand_row!r}')
            selected = []
            rows = connection.execute(
                'SELECT id, remote, remote_map, sub_cate, priority, binary_md5 '
                "FROM remote_index WHERE category_id = 1 AND brand_id = ? AND protocol = 'new_ac' "
                "AND status = 1 AND remote != '2807' ORDER BY priority, id",
                (brand_id,),
            )
            for remote_index, remote, remote_map, sub_cate, priority, binary_md5 in rows:
                if sub_cate != 1 or remote_map != f'new_ac_{remote}' or not binary_md5 or binary_md5 in selected_md5:
                    continue
                cross_brand_count = connection.execute(
                    'SELECT COUNT(DISTINCT brand_id) FROM remote_index '
                    "WHERE category_id = 1 AND protocol = 'new_ac' AND status = 1 AND binary_md5 = ?",
                    (binary_md5,),
                ).fetchone()[0]
                if cross_brand_count < 1 or (max_shared is not None and cross_brand_count > max_shared):
                    continue
                member = f'irext-binaries_20260929/irda_{remote_map}.bin'
                try:
                    info = bundle.getinfo(member)
                    binary = bundle.read(info)
                except KeyError:
                    continue
                if info.is_dir() or info.filename != member or hashlib.md5(binary).hexdigest() != binary_md5:
                    continue
                selected.append(remote_index)
                selected_md5.add(binary_md5)
                selected_by_index[remote_index] = binary
                if len(selected) == SELECTION_PER_BRAND:
                    break
            ordered = sorted(manifest_by_brand[brand], key=lambda item: (item['priority'], item['remote_index']))
            expected = [item['remote_index'] for item in ordered]
            if [index for index in selected if index in expected] != expected:
                fail(f'{brand}: manifest remote_index list is not an ordered subset of the database selection')
        for profile in manifest['profiles']:
            row = connection.execute(
                'SELECT category_id, brand_id, protocol, remote, remote_map, status, sub_cate, priority, binary_md5 '
                'FROM remote_index WHERE id = ?', (profile['remote_index'],)
            ).fetchone()
            expected = (1, profile['brand_id'], 'new_ac', profile['remote'], f"new_ac_{profile['remote']}", 1, 1, profile['priority'], profile['md5'])
            if row != expected:
                fail(f"{profile['id']}: remote_index {profile['remote_index']} mapping mismatch; got {row!r}")
            binary = selected_by_index[profile['remote_index']]
            if hashlib.md5(binary).hexdigest() != profile['md5'] or hashlib.sha256(binary).hexdigest() != profile['sha256']:
                fail(f"{profile['id']}: binary digest mismatch")
            binaries[profile['id']] = binary
    return binaries


# ---------------------------------------------------------------------------
# IRext binary parsing

def ascii_tag(data, tag_id, profile_id):
    if data is None:
        return None
    try:
        return data.decode('ascii')
    except UnicodeDecodeError:
        fail(f'{profile_id}: tag {tag_id} is not ASCII')


def split_tags(binary, profile_id):
    header_size = 1 + len(TAG_IDS) * 2
    if len(binary) < header_size or binary[0] != len(TAG_IDS):
        fail(f'{profile_id}: binary does not have the required 29-tag header')
    offsets = struct.unpack_from('<29H', binary, 1)
    payload_size = len(binary) - header_size
    valid = [(index, offset) for index, offset in enumerate(offsets) if offset != MAX_U16]
    previous = -1
    for index, offset in valid:
        if offset >= payload_size or offset <= previous:
            fail(f'{profile_id}: tag {TAG_IDS[index]} offset {offset} is invalid')
        previous = offset
    tags = {}
    for position, (index, offset) in enumerate(valid):
        end = valid[position + 1][1] if position + 1 < len(valid) else payload_size
        tags[TAG_IDS[index]] = binary[header_size + offset:header_size + end]
    return tags


def parse_uint(text, where, minimum=0, maximum=MAX_U16):
    if not re.fullmatch(r'-?[0-9]+', text):
        fail(f'{where}: expected decimal integer')
    value = int(text)
    if value < minimum or value > maximum:
        fail(f'{where}: {value} is outside [{minimum}, {maximum}]')
    return value


def parse_csv_uint(text, where, minimum=0, maximum=MAX_U16, max_count=None):
    parts = text.split(',') if text else []
    if not parts or any(not part for part in parts) or (max_count is not None and len(parts) > max_count):
        fail(f'{where}: invalid list length')
    return [parse_uint(part, where, minimum, maximum) for part in parts]


def parse_hex(data, tag_id, profile_id):
    text = ascii_tag(data, tag_id, profile_id)
    if not text or len(text) % 2 or not re.fullmatch(r'[0-9A-Fa-f]+', text):
        fail(f'{profile_id}: tag {tag_id} must be non-empty even-length hexadecimal')
    return bytes.fromhex(text)


def parse_segments(raw, count, width, where, exact=True):
    result = []
    offset = 0
    for index in range(count):
        if offset >= len(raw):
            result.extend([[] for _ in range(count - index)])
            break
        length = raw[offset]
        offset += 1
        if offset + length > len(raw) or length % width:
            fail(f'{where}: segment {index} is malformed')
        result.append(list(raw[offset:offset + length]))
        offset += length
    if exact and offset != len(raw):
        fail(f'{where}: trailing bytes after {count} segments')
    return result


def validate_segments(segments, width, payload_len, where):
    for index, segment in enumerate(segments):
        for offset in range(0, len(segment), width):
            if width == 2 and segment[offset] >= payload_len:
                fail(f'{where}: segment {index} byte position is outside default payload')
            if width == 3:
                start, end, _value = segment[offset:offset + 3]
                if start >= end or end > payload_len * 8 or end - start > 8 or (end - 1) // 8 - start // 8 > 1:
                    fail(f'{where}: segment {index} bit range [{start}, {end}) is invalid')


def parse_temperature(raw, width, where):
    if raw[0] == len(raw) - 1:
        body = raw[1:]
        if not body or len(body) % width:
            fail(f'{where}: malformed dynamic temperature descriptor')
        segments = []
        for index in range(TEMP_COUNT):
            segment = list(body)
            for offset in range(width - 1, len(segment), width):
                segment[offset] *= index
                # libirdecode truncates the type1 step to UINT8, so step 0xFF subtracts one per degree.
                if width == 2:
                    segment[offset] &= 0xFF
                elif segment[offset] > 0xFF:
                    fail(f'{where}: expanded temperature value overflows one byte')
            segments.append(segment)
        return {'kind': 'dynamic', 'values': segments}
    return {'kind': 'static', 'values': parse_segments(raw, TEMP_COUNT, width, where)}


def parse_functions(raw, width, where):
    """Mirrors parse_function_1/2: at most 7 segments, ids above 7 never applied."""
    functions = {}
    offset = 0
    for _ in range(7):
        if offset >= len(raw):
            break
        length = raw[offset]
        offset += 1
        if length < 1 or offset + length > len(raw) or (length - 1) % width:
            fail(f'{where}: function segment is malformed')
        function_id = raw[offset]
        if 1 <= function_id <= 7:
            if function_id in functions:
                fail(f'{where}: duplicate function id {function_id}')
            functions[function_id] = list(raw[offset + 1:offset + length])
        offset += length
    return functions


def parse_swing_info(text, profile_id):
    if text is None:
        return {'type': 'normal', 'count': 2}
    if text == '0':
        return {'type': 'not_specified', 'count': 0}
    if text == '1':
        return {'type': 'swing_only', 'count': 1}
    values = text.split(',')
    if len(values) < 2 or any(not re.fullmatch(r'[0-9]+', value) for value in values):
        fail(f'{profile_id}: tag 46 has invalid swing information')
    return {'type': 'normal', 'count': len(values)}


def parse_forbidden(text, profile_id, mode):
    result = {'enabled': True, 'all_temperature': False, 'temperatures': [], 'all_fan': False, 'fans': []}
    if not text:
        return result
    if text.upper() == 'NA':
        result['enabled'] = False
        return result
    seen = set()
    for token in text.split('|'):
        match = re.fullmatch(r'([TS])(?:&([0-9]+(?:,[0-9]+)*))?', token)
        if not match or match.group(1) in seen:
            fail(f'{profile_id}: {mode} forbidden constraint is malformed: {text}')
        kind, values = match.groups()
        seen.add(kind)
        if values is None:
            result['all_temperature' if kind == 'T' else 'all_fan'] = True
        elif kind == 'T':
            result['temperatures'] = parse_csv_uint(values, f'{profile_id}: {mode} temperatures', 16, 30, 15)
        else:
            result['fans'] = parse_csv_uint(values, f'{profile_id}: {mode} fans', 0, 3, 4)
    return result


def parse_checksums(text, payload_len, profile_id):
    if text is None:
        return []
    result = []
    for index, encoded in enumerate(text.split('|')):
        if not encoded or len(encoded) % 2 or not re.fullmatch(r'[0-9A-Fa-f]+', encoded):
            fail(f'{profile_id}: checksum {index} is malformed')
        raw = bytes.fromhex(encoded)
        if raw[0] + 1 != len(raw) or len(raw) < 5 or raw[1] not in range(1, 9):
            fail(f'{profile_id}: checksum {index} has invalid length or type')
        item = {'type': raw[1]}
        if raw[1] <= 4:
            start, end, target = raw[2:5]
            if start >= end or end > payload_len or target >= payload_len or len(raw) > 6:
                fail(f'{profile_id}: checksum {index} byte bounds are invalid')
            item.update({'start': start, 'end': end, 'target': target, 'plus': raw[5] if len(raw) == 6 else 0})
        else:
            target, plus, positions = raw[2], raw[3], list(raw[4:])
            if target >> 1 >= payload_len or not positions or any(position >= payload_len * 2 for position in positions):
                fail(f'{profile_id}: checksum {index} half-byte bounds are invalid')
            item.update({'target_half_byte': target, 'plus': plus, 'positions': positions})
        result.append(item)
    return result


def parse_position_list(text, payload_len, profile_id, label, value_parser):
    if text is None:
        return {}
    result = {}
    for item in text.split('|'):
        parts = item.split('&')
        if len(parts) != 2:
            fail(f'{profile_id}: malformed {label} entry')
        position = parse_uint(parts[0], f'{profile_id}: {label} position', -1, payload_len - 1)
        if position in result:
            fail(f'{profile_id}: duplicate {label} position')
        result[position] = value_parser(parts[1], f'{profile_id}: {label}')
    if len(result) > 16:
        fail(f'{profile_id}: too many {label} entries')
    return result


def parse_protocol(binary, profile_id):
    tags = split_tags(binary, profile_id)
    text = {tag_id: ascii_tag(tags.get(tag_id), tag_id, profile_id) for tag_id in TAG_IDS}
    if any(text[tag_id] is None for tag_id in (2, 3, 22)):
        fail(f'{profile_id}: missing required tag')
    if 28 in tags:
        fail(f'{profile_id}: solo functions (tag 28) are not supported')
    boot = parse_csv_uint(text[1], f'{profile_id}: boot timings', 1, MAX_U16, 16) if text[1] is not None else []
    zero = parse_csv_uint(text[2], f'{profile_id}: zero timings', 1, MAX_U16, 2)
    one = parse_csv_uint(text[3], f'{profile_id}: one timings', 1, MAX_U16, 2)
    if len(zero) != 2 or len(one) != 2:
        fail(f'{profile_id}: zero and one timings require two values')
    default_raw = parse_hex(tags[22], 22, profile_id)
    if default_raw[0] + 1 != len(default_raw):
        fail(f'{profile_id}: default payload length prefix is invalid')
    payload = list(default_raw[1:])
    n = len(payload)
    swing = parse_swing_info(text[46], profile_id)
    patches = {'type1': {}, 'type2': {}}
    layouts = (
        ('type1', 2, {21: 'power', 23: 'temperature', 24: 'mode', 25: 'fan', 26: 'swing', 29: 'function'}),
        ('type2', 3, {30: 'temperature', 31: 'mode', 32: 'fan', 33: 'swing', 34: 'function'}),
    )
    counts = {'power': 2, 'mode': 5, 'fan': 4, 'swing': swing['count']}
    for kind, width, names in layouts:
        for tag_id, name in names.items():
            # libirdecode loads swing patches only for NORMAL swing, see ir_ac_control.c.
            if tag_id not in tags or (name == 'swing' and swing['type'] != 'normal'):
                continue
            raw = parse_hex(tags[tag_id], tag_id, profile_id)
            where = f'{profile_id}: {name} {kind}'
            if name == 'temperature':
                value = parse_temperature(raw, width, where)
                segments = value['values']
            elif name == 'function':
                value = parse_functions(raw, width, where)
                segments = list(value.values())
            else:
                value = segments = parse_segments(raw, counts[name], width, where)
            validate_segments(segments, width, n, where)
            patches[kind][name] = value
    repeat = parse_uint(text[47], f'{profile_id}: repeat', 1, 255) if text[47] is not None else 1
    return {
        'timings': {
            'boot_us': boot, 'zero_us': zero, 'one_us': one,
            'endian': parse_uint(text[6], f'{profile_id}: endian', 0, 1) if text[6] is not None else 0,
            'last_bit': parse_uint(text[7], f'{profile_id}: last bit', 0, 1) if text[7] is not None else 0,
        },
        'default_payload': payload,
        'patches': patches,
        'swing': swing,
        'checksum': parse_checksums(text[27], n, profile_id),
        'delay': parse_position_list(text[4], n, profile_id, 'delay', lambda value, where: parse_csv_uint(value, where, 1, MAX_U16, 8)),
        'bit_widths': {n - 1 if position == -1 else position: bits for position, bits in parse_position_list(
            text[48], n, profile_id, 'bit width', lambda value, where: parse_uint(value, where, 1, 8)).items()},
        'repeat': repeat,
        'mode_forbidden': {name: parse_forbidden(text[41 + index], profile_id, name) for index, name in enumerate(MODE_NAMES)},
    }


# ---------------------------------------------------------------------------
# IRext reference model

def apply_type1(payload, segment, additive):
    for offset in range(0, len(segment), 2):
        position, value = segment[offset:offset + 2]
        payload[position] = (payload[position] + value) & 0xFF if additive else value


def apply_type2(payload, segment, additive):
    for offset in range(0, len(segment), 3):
        start, end, raw_value = segment[offset:offset + 3]
        high_position = start >> 3
        low_position = (end - 1) >> 3
        internal_start = start - high_position * 8
        internal_end = end - low_position * 8
        bit_count = end - start
        if high_position == low_position:
            mask = ((0xFF << (8 - internal_start)) | (0xFF >> internal_end)) & 0xFF
            origin = payload[low_position]
            shift = 8 - internal_end
            if additive:
                value = (origin & mask) | (((((origin & (~mask & 0xFF)) >> shift) + raw_value) << shift) & (~mask & 0xFF))
            else:
                value = (origin & mask) | ((raw_value << (8 - internal_start - bit_count)) & (~mask & 0xFF))
            payload[low_position] = value & 0xFF
            continue
        origin_high = payload[high_position]
        origin_low = payload[low_position]
        mask_high = (0xFF << (8 - internal_start)) & 0xFF
        mask_low = 0xFF >> internal_end
        current = (((origin_high & (~mask_high & 0xFF)) << internal_end) | ((origin_low & (~mask_low & 0xFF)) >> (8 - internal_end))) & 0xFF
        if additive:
            raw_value = (raw_value + current) & 0xFF
        value_mask = 0xFF >> (8 - bit_count)
        payload[high_position] = ((origin_high & mask_high) | ((value_mask & raw_value) >> internal_end)) & 0xFF
        payload[low_position] = ((origin_low & mask_low) | ((value_mask & raw_value) << (8 - internal_end))) & 0xFF


def selected_patch(p, name):
    """libirdecode only falls back to the type2 tag when the type1 tag is absent."""
    for kind, apply in (('type1', apply_type1), ('type2', apply_type2)):
        value = p['patches'][kind].get(name)
        if value is not None:
            return value, apply
    return None, None


def segment_of(p, name, index):
    """None when the tag is absent, [] when apply_ac_* fails for this index."""
    value, _apply = selected_patch(p, name)
    if value is None:
        return None
    segments = value['values'] if name == 'temperature' else value
    if name == 'swing' and index >= p['swing']['count']:
        return []
    return segments[index] if index < len(segments) else []


def patch(p, payload, name, index):
    """None when the tag is absent, False when libirdecode's apply_ac_* fails, True when applied."""
    segment = segment_of(p, name, index)
    if not segment:
        return None if segment is None else False
    value, apply = selected_patch(p, name)
    apply(payload, segment, name == 'temperature' and value['kind'] == 'dynamic')
    return True


def has_function(p, function_id):
    return any(p['patches'][kind].get('function', {}).get(function_id) for kind in ('type1', 'type2'))


def apply_function(p, payload, function_id):
    value, apply = selected_patch(p, 'function')
    if value and value.get(function_id):
        apply(payload, value[function_id], False)


def apply_checksums(p, payload):
    for checksum in p['checksum']:
        checksum_type = checksum['type']
        inverse = checksum_type % 2 == 0
        if checksum_type <= 4:
            data = payload[checksum['start']:checksum['end']]
            value = sum(data) if checksum_type <= 2 else sum((byte >> 4) + (byte & 0x0F) for byte in data)
            value = (value + checksum['plus']) & 0xFF
            payload[checksum['target']] = (~value) & 0xFF if inverse else value
            continue
        target_half = checksum['target_half_byte']
        target = target_half >> 1
        value = sum(payload[position >> 1] >> 4 if position & 1 == 0 else payload[position >> 1] & 0x0F for position in checksum['positions'])
        value = (value + checksum['plus']) & 0xFF
        if inverse:
            value = (~value) & 0xFF
        if checksum_type in (5, 6):
            if target_half & 1:
                payload[target] = (payload[target] & 0xF0) | (value & 0x0F)
            else:
                payload[target] = (payload[target] & 0x0F) | ((value << 4) & 0xF0)
        else:
            payload[target] = value


def irext_payload(p, s, checksum=True):
    """ir_ac_control() for state s; None where libirdecode refuses to send or a swing key has no swing data."""
    payload = list(p['default_payload'])
    key = s['key']
    if not s['power']:
        patch(p, payload, 'power', 1)
    else:
        rule = p['mode_forbidden'][MODE_NAMES[s['mode']]]
        if not rule['enabled']:
            return None
        patch(p, payload, 'power', 0)
        if patch(p, payload, 'mode', s['mode']) is False:
            return None
        if not rule['all_fan'] and s['fan'] not in rule['fans']:
            if patch(p, payload, 'fan', s['fan']) is False and key == F_FAN:
                return None
        elif key == F_FAN:
            return None
        # Without NORMAL swing libirdecode sends the bare template for swing keys, so they are unsupported.
        swing_failed = patch(p, payload, 'swing', s['swing']) is False or p['swing']['type'] != 'normal'
        if swing_failed and key in (F_SWING, F_FIX) and not has_function(p, key):
            return None
        temperature_keys = (F_TEMP_UP, F_TEMP_DOWN)
        if not rule['all_temperature'] and s['temp'] + TEMP_BASE_C not in rule['temperatures']:
            if patch(p, payload, 'temperature', s['temp']) is False and key in temperature_keys:
                return None
        elif key in temperature_keys:
            return None
    apply_function(p, payload, key)
    if checksum:
        apply_checksums(p, payload)
    return payload


def irext_pulses(p, payload):
    """create_ir_frame()."""
    t = p['timings']
    pulses = list(t['boot_us'])
    for position, byte in enumerate(payload):
        bit_count = p['bit_widths'].get(position, 8)
        for bit in range(bit_count):
            shift = bit if t['endian'] else bit_count - 1 - bit
            pulses.extend(t['one_us'] if byte >> shift & 1 else t['zero_us'])
        pulses.extend(p['delay'].get(position, []))
        if position == len(payload) - 1:
            if t['last_bit'] == 0:
                pulses.append(t['one_us'][0])
            pulses.extend(p['delay'].get(-1, []))
    return pulses * p['repeat']


def make_state(power=True, mode=0, temp=0, fan=0, swing=0, key=F_POWER):
    return {'power': power, 'mode': mode, 'temp': temp, 'fan': fan, 'swing': swing, 'key': key}


OFF_STATE = make_state(power=False)


# ---------------------------------------------------------------------------
# libirdecode

def build_decoder(core_repo, build_dir, expected_version):
    try:
        subprocess.run(['cmake', '-S', str(core_repo / 'decoder'), '-B', str(build_dir)], check=True, capture_output=True, text=True)
        subprocess.run(['cmake', '--build', str(build_dir), '--target', 'irdecode'], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = exc.stderr.strip() if isinstance(exc, subprocess.CalledProcessError) and exc.stderr else str(exc)
        fail(f'failed to build pinned IRext decoder: {detail}')
    library_path = build_dir / 'libirdecode.so'
    if not library_path.is_file():
        fail('pinned decoder build did not produce libirdecode.so')
    library = ctypes.CDLL(str(library_path))
    library.get_lib_version.restype = ctypes.c_char_p
    version = library.get_lib_version()
    if not version or version.decode('ascii') != expected_version:
        fail(f'decoder version does not match manifest core_version {expected_version}')
    library.ir_binary_open.argtypes = [ctypes.c_ubyte, ctypes.c_ubyte, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_ushort]
    library.ir_binary_open.restype = ctypes.c_byte
    library.ir_decode.argtypes = [ctypes.c_ubyte, ctypes.POINTER(ctypes.c_ushort), ctypes.POINTER(ACStatus)]
    library.ir_decode.restype = ctypes.c_ushort
    library.ir_close.restype = ctypes.c_byte
    return library


class Decoder:
    """One open libirdecode session; its stdout chatter is silenced."""

    def __init__(self, library, binary, profile_id):
        self.library = library
        self.buffer = (ctypes.c_ubyte * len(binary)).from_buffer_copy(binary)
        self.output = (ctypes.c_ushort * MAX_U16)()
        self.profile_id = profile_id

    def __enter__(self):
        sys.stdout.flush()
        self.saved = os.dup(1)
        self.sink = os.open(os.devnull, os.O_WRONLY)
        os.dup2(self.sink, 1)
        self.open()
        return self

    def __exit__(self, *_exc):
        self.library.ir_close()
        os.dup2(self.saved, 1)
        os.close(self.saved)
        os.close(self.sink)

    def open(self):
        if self.library.ir_binary_open(1, 1, self.buffer, len(self.buffer)) != 0:
            fail(f'{self.profile_id}: pinned decoder rejected binary')

    def reopen(self):
        self.library.ir_close()
        self.open()

    def press(self, s, change_direction=0):
        status = ACStatus(0 if s['power'] else 1, s['temp'], s['mode'], 0, s['fan'], 0, 0, 0, change_direction)
        count = self.library.ir_decode(KEY_CODES[s['key'] - 1], self.output, ctypes.byref(status))
        return list(self.output[:count])


# ---------------------------------------------------------------------------
# Legal states and message plans

def legal_domain(p):
    """Per-mode IRext indices whose patch libirdecode applies, plus swing statuses."""
    swings = [s for s in range(p['swing']['count']) if segment_of(p, 'swing', s)]
    has_keys = any(has_function(p, function_id) for function_id in range(1, 8))
    modes = {}
    for mode, name in enumerate(MODE_NAMES):
        rule = p['mode_forbidden'][name]
        if not rule['enabled'] or segment_of(p, 'mode', mode) == []:
            continue
        temps = [] if rule['all_temperature'] else [t for t in range(TEMP_COUNT) if t + TEMP_BASE_C not in rule['temperatures'] and segment_of(p, 'temperature', t)]
        fans = [] if rule['all_fan'] else [f for f in range(len(FAN_NAMES)) if f not in rule['fans'] and segment_of(p, 'fan', f)]
        if temps and temps != list(range(temps[0], temps[-1] + 1)):
            fail(f"{p['id']}: mode {name} has sparse temperatures")
        base = make_state(mode=mode, temp=temps[0] if temps else 0, fan=fans[0] if fans else 0, swing=swings[0] if swings else 0)
        keys = [k for k in range(1, 8) if irext_payload(p, {**base, 'key': k}) is not None] if has_keys else []
        if irext_payload(p, base) is None or (has_keys and F_POWER not in keys):
            fail(f"{p['id']}: mode {name} rejects its base state")
        modes[mode] = {'temp': temps, 'fan': fans, 'key': keys, 'swing': list(swings)}
    if not modes:
        fail(f"{p['id']}: no usable mode")
    return modes


def message_states(plan):
    for mode in plan['modes']:
        legal = plan['legal'][mode]
        for temp, fan, swing, key in product(legal['temp'] or [0], legal['fan'] or [0], legal['swing'] or [0], legal['key'] or [F_POWER]):
            yield make_state(mode=mode, temp=temp, fan=fan, swing=swing, key=key)


def make_plans(modes, split_modes):
    signature = {mode: (bool(info['temp']), bool(info['fan'])) for mode, info in modes.items()}
    common = None
    if not split_modes:
        common = max(Counter(signature.values()).items(), key=lambda item: (item[1], sum(item[0])))[0]
    plans = []
    generic = [mode for mode in modes if signature[mode] == common]
    if generic:
        plans.append({'name': 'frame', 'implied': {'power': True}, 'modes': generic})
    for mode in modes:
        if mode not in generic:
            plans.append({'name': MODE_NAMES[mode], 'implied': {'power': True, 'mode': mode}, 'modes': [mode]})
    for plan in plans:
        info = modes[plan['modes'][0]]
        plan['legal'] = {mode: {name: list(values) for name, values in modes[mode].items()} for mode in plan['modes']}
        plan['fields'] = [name for name, used in (
            ('mode', len(plan['modes']) > 1), ('temperature', bool(info['temp'])), ('fan', bool(info['fan'])),
            ('swing_v', bool(info['swing'])), ('key', bool(info['key'])),
        ) if used]
    return plans


# ---------------------------------------------------------------------------
# Field encoding (IRext index <-> ir_values integer <-> profile JSON value)

def field_value(field, index):
    if field == 'mode':
        return MODE_ENUM[MODE_NAMES[index]]
    if field == 'temperature':
        return (TEMP_BASE_C + index) * DECI
    if field == 'fan':
        return FAN_ENUM[FAN_NAMES[index]]
    if field == 'swing_v':
        return SWING_AUTO + index
    return index - 1


def field_json(field, index):
    if field == 'mode':
        return MODE_NAMES[index]
    if field == 'temperature':
        return TEMP_BASE_C + index
    if field == 'fan':
        return FAN_NAMES[index]
    if field == 'swing_v':
        return 'auto' if index == 0 else f'position_{index}'
    return KEY_NAMES[index - 1]


def json_to_value(field, value):
    if field == 'power':
        return int(value)
    if field == 'mode':
        return MODE_ENUM[value]
    if field == 'temperature':
        return round(value * DECI)
    if field == 'fan':
        return FAN_ENUM[value]
    if field == 'swing_v':
        return SWING_AUTO if value == 'auto' else SWING_AUTO + int(value[len('position_'):])
    return KEY_NAMES.index(value)


def field_base_step(field):
    return (TEMP_BASE_C * DECI, DECI) if field == 'temperature' else (0, 1)


# ---------------------------------------------------------------------------
# Black-box inference

def check_target_mask(checks, length):
    mask = [0] * length
    for c in checks:
        if c.get('nibble') and not c.get('byte_target'):
            mask[c['target'] >> 1] |= 0x0F if c['target'] & 1 else 0xF0
        else:
            mask[c['target']] = 0xFF
    return mask


def mask_segments(mask):
    segments = []
    for byte, bits in enumerate(mask):
        shift = 0
        while shift < 8:
            if not bits >> shift & 1:
                shift += 1
                continue
            width = 0
            while shift + width < 8 and bits >> (shift + width) & 1:
                width += 1
            segments.append([byte, shift, width])
            shift += width
    return segments


def read_raw(segments, payload):
    raw = 0
    shift = 0
    for byte, bit, width in segments:
        raw |= (payload[byte] >> bit & ((1 << width) - 1)) << shift
        shift += width
    return raw


def json_checks(p):
    result = []
    for c in p['checksum']:
        t = c['type']
        if t <= 2:
            item = {'op': 'sum', 'target': c['target'], 'first': c['start'], 'count': c['end'] - c['start']}
        elif t <= 4:
            item = {'op': 'sum', 'target': c['target'], 'first': 2 * c['start'], 'count': 2 * (c['end'] - c['start']), 'nibble': True, 'byte_target': True}
        elif t <= 6:
            item = {'op': 'sum', 'target': c['target_half_byte'], 'units': c['positions'], 'nibble': True}
        else:
            item = {'op': 'sum', 'target': c['target_half_byte'] >> 1, 'units': c['positions'], 'nibble': True, 'byte_target': True}
        if c['plus']:
            item['bias'] = c['plus']
        if t % 2 == 0:
            item['invert'] = True
        result.append(item)
    return result


def json_layout(p):
    """create_ir_frame() as a step program; IRext pulse index parity gives the level."""
    t = p['timings']
    length = len(p['default_payload'])
    steps = []
    parity = 0
    run = 0
    for us in t['boot_us']:
        steps.append(('M' if parity == 0 else 'S') + str(us))
        parity ^= 1
    for position in range(length):
        if parity:
            fail(f"{p['id']}: data bits start on a space")
        run += p['bit_widths'].get(position, 8)
        tail = list(p['delay'].get(position, []))
        if position == length - 1:
            tail += ([t['one_us'][0]] if t['last_bit'] == 0 else []) + p['delay'].get(-1, [])
        if tail:
            steps.append(f'B{run}')
            run = 0
        for us in tail:
            steps.append(('M' if parity == 0 else 'S') + str(us))
            parity ^= 1
    if run:
        steps.append(f'B{run}')
    if p['repeat'] > 1 and parity:
        fail(f"{p['id']}: repeated frame does not end on a space")
    return {'symbols': [t['zero_us'], t['one_us']], 'steps': steps}


def message_shell(p, name, implied, template):
    length = len(template)
    message = {'name': name, 'implied': implied, 'layout': json_layout(p)}
    if p['repeat'] > 1:
        message['copies'] = p['repeat']
    message['length'] = length
    if p['timings']['endian'] == 0:
        message['msb_first'] = True
    widths = [[position, bits] for position, bits in sorted(p['bit_widths'].items()) if bits != 8]
    if widths:
        message['widths'] = widths
    message['template'] = bytes(template).hex()
    message['fields'] = []
    message['checks'] = json_checks(p)
    return message


def field_mask(p, plan, field, samples, payload):
    """OR of payload differences when only this field changes between two legal states."""
    key = STATE_KEY[field]
    mask = [0] * len(p['default_payload'])
    for s in samples:
        for value in plan['modes'] if field == 'mode' else plan['legal'][s['mode']][key]:
            other = {**s, key: value}
            legal = plan['legal'][other['mode']]
            if any(legal[k] and other[k] not in legal[k] for k in ('temp', 'fan', 'swing', 'key')):
                continue
            alternative = payload(other)
            if alternative is None:
                fail(f"{p['id']}: {plan['name']} state {other} is rejected by the model")
            mask = [m | (a ^ b) for m, a, b in zip(mask, payload(s), alternative)]
    return mask


def remove_value(plan, field, index):
    if field == 'mode':
        plan['modes'].remove(index)
        del plan['legal'][index]
        return
    for legal in plan['legal'].values():
        if index in legal[STATE_KEY[field]]:
            legal[STATE_KEY[field]].remove(index)


def drop_field(plan, field):
    """A state field without effect keeps only its first value; keys without effect all stay legal."""
    plan['fields'].remove(field)
    if field == 'mode':
        plan['modes'] = plan['modes'][:1]
        plan['legal'] = {plan['modes'][0]: plan['legal'][plan['modes'][0]]}
    elif field != 'key':
        for legal in plan['legal'].values():
            legal[STATE_KEY[field]] = legal[STATE_KEY[field]][:1]


def payload_cache(p):
    cache = p.setdefault('_payloads', {})

    def payload(s):
        key = tuple(s.values())
        if key not in cache:
            cache[key] = irext_payload(p, s, checksum=False)
        return cache[key]
    return payload


def plan_samples(plan, states):
    """States spanning mode/temperature/fan/key at the base swing, and swing/key at the base temperature/fan."""
    bases = {mode: next(s for s in states if s['mode'] == mode) for mode in plan['modes']}
    samples = [s for s in states if s['swing'] == bases[s['mode']]['swing']]
    return samples + [s for s in states if (s['temp'], s['fan']) == (bases[s['mode']]['temp'], bases[s['mode']]['fan'])]


def function_bits(p, key):
    """Payload bits the function patch of this key overwrites."""
    value, apply = selected_patch(p, 'function')
    segment = value.get(key, []) if value else []
    mask = [0] * len(p['default_payload'])
    if apply is apply_type1:
        for offset in range(0, len(segment), 2):
            mask[segment[offset]] = 0xFF
    else:
        for offset in range(0, len(segment), 3):
            for bit in range(segment[offset], segment[offset + 1]):
                mask[bit >> 3] |= 0x80 >> (bit & 7)
    return mask


def split_solo_keys(p, plans):
    """Keys whose function patch overwrites state bits get messages of their own with the key implied."""
    payload = payload_cache(p)
    targets = check_target_mask(json_checks(p), len(p['default_payload']))
    solo = set()
    for plan in plans:
        if 'key' not in plan['fields']:
            continue
        samples = plan_samples(plan, list(message_states(plan)))
        state_mask = [0] * len(targets)
        for field in plan['fields']:
            if field != 'key':
                state_mask = [a | b for a, b in zip(state_mask, field_mask(p, plan, field, samples, payload))]
        for key in {k for legal in plan['legal'].values() for k in legal['key']}:
            if any(f & m & ~t for f, m, t in zip(function_bits(p, key), state_mask, targets)):
                solo.add(key)
    if not solo:
        return plans

    def clone(plan, keys):
        legal = {mode: {**{name: list(values) for name, values in plan['legal'][mode].items()}, 'key': [k for k in plan['legal'][mode]['key'] if k in keys]} for mode in plan['modes']}
        return {mode: item for mode, item in legal.items() if item['key']}

    result = []
    for plan in plans:
        for key in [None] + sorted(solo):
            legal = clone(plan, set(range(1, 8)) - solo if key is None else {key})
            if not legal:
                continue
            modes = [mode for mode in plan['modes'] if mode in legal]
            fields = [f for f in plan['fields'] if (f != 'key' or key is None) and (f != 'mode' or len(modes) > 1)]
            name = plan['name'] if key is None else f"{plan['name']}_{KEY_NAMES[key - 1]}"
            implied = plan['implied'] if key is None else {**plan['implied'], 'key': key}
            result.append({'name': name, 'implied': implied, 'modes': modes, 'legal': legal, 'fields': fields})
    return result


def merge_solo_messages(plans, messages):
    """One message per solo key when every mode sends the same constant frame for it."""
    groups = defaultdict(list)
    for plan, message in zip(plans, messages):
        if 'key' in plan['implied']:
            groups[plan['implied']['key']].append(message)
    merged = []
    for key, group in sorted(groups.items()):
        if all(not m['fields'] and m['template'] == group[0]['template'] for m in group):
            merged.append({**group[0], 'name': KEY_NAMES[key - 1], 'implied': {'power': True, 'key': KEY_NAMES[key - 1]}})
            messages = [m for m in messages if m not in group]
    return merged + messages


def infer_plan(p, plan):
    """Returns the JSON message for one plan; duplicate enum values are removed from plan['legal']."""
    payload = payload_cache(p)
    states = list(message_states(plan))
    samples = plan_samples(plan, states)
    targets = check_target_mask(json_checks(p), len(p['default_payload']))
    masks = {field: [m & ~t & 0xFF for m, t in zip(field_mask(p, plan, field, samples, payload), targets)] for field in plan['fields']}
    fields = []
    used = [0] * len(targets)
    for field in list(plan['fields']):
        mask = masks[field]
        if any(m & u for m, u in zip(mask, used)):
            fail(f"{p['id']}: {plan['name']} field {field} overlaps another field")
        used = [m | u for m, u in zip(mask, used)]
        segments = mask_segments(mask)
        if not segments:
            drop_field(plan, field)
            continue
        if sum(segment[2] for segment in segments) > FIELD_MAX_WIDTH:
            fail(f"{p['id']}: {plan['name']} field {field} is wider than {FIELD_MAX_WIDTH} bits")
        raws = {}
        for s in states:
            raw = read_raw(segments, payload(s))
            if raws.setdefault(s[STATE_KEY[field]], raw) != raw:
                fail(f"{p['id']}: {plan['name']} field {field} value {s[STATE_KEY[field]]} has several encodings")
        if field in ENUM_DEDUP_FIELDS:
            seen = set()
            for index in sorted(raws):
                if raws[index] in seen:
                    del raws[index]
                    remove_value(plan, field, index)
                else:
                    seen.add(raws[index])
            if len(raws) < 2:
                drop_field(plan, field)
                continue
        elif field != 'key' and len(set(raws.values())) != len(raws):
            fail(f"{p['id']}: {plan['name']} field {field} has duplicate encodings")
        base, step = field_base_step(field)
        table = [None] * (max((field_value(field, index) - base) // step for index in raws) + 1)
        for index, raw in raws.items():
            table[(field_value(field, index) - base) // step] = raw
        item = {'field': field, 'segments': segments, 'raw': table}
        if base:
            item['base'] = base
        if step != 1:
            item['step'] = step
        fields.append(item)
    implied = {name: value if name == 'power' else field_json(name, value) for name, value in plan['implied'].items()}
    message = message_shell(p, plan['name'], implied, payload(next(message_states(plan))))
    message['fields'] = fields
    return message


# ---------------------------------------------------------------------------
# Mirror of the C encoder (ir_protocol.c / ir_layout.c) over the emitted JSON

def json_select(protocol, values):
    best = None
    best_count = -1
    for message in protocol['messages']:
        implied = message.get('implied', {})
        if all(values.get(field) == json_to_value(field, value) for field, value in implied.items()) and len(implied) > best_count:
            best, best_count = message, len(implied)
    return best


def json_payload(message, values):
    payload = bytearray.fromhex(message['template'])
    for f in message['fields']:
        if f['field'] not in values:
            return None
        offset = values[f['field']] - f.get('base', 0)
        step = f.get('step', 1)
        index = offset // step
        if offset < 0 or offset % step or index >= len(f['raw']) or f['raw'][index] is None:
            return None
        raw = f['raw'][index]
        for byte, shift, width in f['segments']:
            mask = ((1 << width) - 1) << shift
            payload[byte] = (payload[byte] & ~mask) | ((raw << shift) & mask)
            raw >>= width
    for c in message['checks']:
        nibble = c.get('nibble', False)
        target_nibble = nibble and not c.get('byte_target', False)
        units = c.get('units') or range(c['first'], c['first'] + c['count'])
        acc = c.get('bias', 0)
        for unit in units:
            acc += ((payload[unit >> 1] & 0x0F) if unit & 1 else payload[unit >> 1] >> 4) if nibble else payload[unit]
        value = (acc & 0xFF) ^ (0xFF if c.get('invert') else 0)
        if not target_nibble:
            payload[c['target']] = value
        elif c['target'] & 1:
            payload[c['target'] >> 1] = (payload[c['target'] >> 1] & 0xF0) | (value & 0x0F)
        else:
            payload[c['target'] >> 1] = (payload[c['target'] >> 1] & 0x0F) | ((value << 4) & 0xF0)
    widths = dict(map(tuple, message.get('widths', [])))
    for index in range(len(payload)):
        payload[index] &= (1 << widths.get(index, 8)) - 1
    return payload


def json_pulses(protocol, values):
    message = json_select(protocol, values)
    payload = json_payload(message, values) if message else None
    if payload is None:
        return None
    widths = dict(map(tuple, message.get('widths', [])))
    bits = []
    for index, byte in enumerate(payload):
        width = widths.get(index, 8)
        bits.extend(byte >> (width - 1 - b if message.get('msb_first') else b) & 1 for b in range(width))
    layout = message['layout']
    pulses = []

    def emit(level, us):
        if not pulses and not level:
            return
        if pulses and (len(pulses) - 1) % 2 == (0 if level else 1):
            pulses[-1] += us
        else:
            pulses.append(us)

    for _ in range(message.get('copies', 1)):
        bit = 0
        for step in layout['steps']:
            kind, value = step[0], int(step[1:])
            if kind == 'B':
                for _n in range(value):
                    mark, space = layout['symbols'][bits[bit]]
                    emit(1, mark)
                    emit(0, space)
                    bit += 1
            else:
                emit(kind == 'M', value)
        if bit != len(bits):
            fail('layout bit count does not match payload')
    return pulses


def json_encodes(protocol, values):
    message = json_select(protocol, values)
    return message is not None and json_payload(message, values) is not None


def limits_allow(profile, values):
    """Mirror of ir_protocol_check()."""
    mode_name = next((name for name, value in MODE_ENUM.items() if value == values.get('mode')), None)
    mode_limits = profile.get('mode_limits', {}).get(mode_name, {})
    for field, value in values.items():
        limit = mode_limits.get(field, profile['limits'].get(field))
        if limit is None:
            continue
        if isinstance(limit, list):
            if value not in [json_to_value(field, item) for item in limit]:
                return False
            continue
        low, high, step = (json_to_value(field, limit.get(name, 1)) for name in ('min', 'max', 'step'))
        if value < low or value > high or (value - low) % step:
            return False
    return True


# ---------------------------------------------------------------------------
# Profile assembly

def message_fields(messages):
    """ir_protocol_fields(): fields a message writes or implies."""
    return {f['field'] for m in messages for f in m['fields']} | {name for m in messages for name in m.get('implied', {})}


def build_limits(plans, swing_values, fields):
    exposed = defaultdict(dict)
    for plan in plans:
        for mode in plan['modes']:
            for field in ('temperature', 'fan'):
                if field in plan['fields']:
                    exposed[mode][field] = plan['legal'][mode][STATE_KEY[field]]
            if 'key' in fields:
                keys = set(exposed[mode].get('key', [])) | set(plan['legal'][mode]['key'])
                exposed[mode]['key'] = sorted(keys)
    modes = sorted({mode for plan in plans for mode in plan['modes']}, key=lambda mode: MODE_ENUM[MODE_NAMES[mode]])
    limits = {'power': [False, True], 'mode': [MODE_NAMES[mode] for mode in modes]}
    mode_limits = defaultdict(dict)
    temps = sorted({t for info in exposed.values() for t in info.get('temperature', [])})
    if temps:
        limits['temperature'] = {'min': TEMP_BASE_C + temps[0], 'max': TEMP_BASE_C + temps[-1], 'step': 1}
        for mode, info in exposed.items():
            values = info.get('temperature')
            if values and (values[0], values[-1]) != (temps[0], temps[-1]):
                mode_limits[MODE_NAMES[mode]]['temperature'] = {'min': TEMP_BASE_C + values[0], 'max': TEMP_BASE_C + values[-1]}
    for field, order in (('fan', range(len(FAN_NAMES))), ('key', range(1, 8))):
        union = [index for index in order if any(index in info.get(field, []) for info in exposed.values())]
        if not union:
            continue
        limits[field] = [field_json(field, index) for index in union]
        for mode, info in exposed.items():
            if field in info and info[field] != union:
                mode_limits[MODE_NAMES[mode]][field] = [field_json(field, index) for index in info[field]]
    if swing_values:
        limits['swing_v'] = [field_json('swing_v', index) for index in swing_values]
    return limits, dict(mode_limits), exposed, modes


def build_defaults(limits, exposed, modes):
    mode = next((m for m in modes if MODE_NAMES[m] == 'cool'), modes[0])
    defaults = {'power': True, 'mode': MODE_NAMES[mode]}
    if 'temperature' in limits:
        temps = exposed[mode].get('temperature') or range(limits['temperature']['min'] - TEMP_BASE_C, limits['temperature']['max'] - TEMP_BASE_C + 1)
        defaults['temperature'] = min(max(26, TEMP_BASE_C + min(temps)), TEMP_BASE_C + max(temps))
    if 'fan' in limits:
        fans = exposed[mode].get('fan') or [FAN_NAMES.index(name) for name in limits['fan']]
        defaults['fan'] = 'auto' if 0 in fans else FAN_NAMES[fans[0]]
    if 'swing_v' in limits:
        defaults['swing_v'] = limits['swing_v'][0]
    return defaults


# A codeset with a single frame: it is sent by the power key, with no state.
KEY_ONLY_VALUES = {'key': 'power'}
KEY_ONLY_LIMITS = {'key': ['power']}


def key_only(profile):
    return profile.get('limits') == KEY_ONLY_LIMITS and 'defaults' not in profile


def build_profile(p, entry, core_version):
    domain = legal_domain(p)
    for split_modes in (False, True):
        plans = split_solo_keys(p, make_plans(domain, split_modes))
        try:
            messages = merge_solo_messages(plans, [infer_plan(p, plan) for plan in plans])
            break
        except GenerationError:
            if split_modes:
                raise
    off = message_shell(p, 'off', {'power': False}, irext_payload(p, OFF_STATE, checksum=False))
    swing_values = sorted({s for plan in plans for legal in plan['legal'].values() for s in legal['swing']}) if any('swing_v' in plan['fields'] for plan in plans) else []
    profile = {'id': p['id'], 'category': 'air_conditioner', 'brand': BRAND_DISPLAY[entry['brand']], 'model': f"IRext {entry['remote']}"}
    if not any(message['fields'] for message in messages) and len({bytes(json_payload(m, {})) for m in messages + [off]}) == 1:
        off['name'] = 'frame'
        off['implied'] = dict(KEY_ONLY_VALUES)
        profile.update({'protocol': {'carrier_hz': CARRIER_HZ, 'messages': [off]}, 'limits': KEY_ONLY_LIMITS})
    else:
        fields = message_fields(messages + [off])
        limits, mode_limits, exposed, modes = build_limits(plans, swing_values, fields)
        defaults = build_defaults(limits, exposed, modes)
        profile['protocol'] = {'carrier_hz': CARRIER_HZ, 'messages': messages + [off]}
        profile['limits'] = {name: value for name, value in limits.items() if name in fields}
        if 'mode' in fields and mode_limits:
            profile['mode_limits'] = mode_limits
        profile['defaults'] = {name: value for name, value in defaults.items() if name in fields}
    profile['source'] = f"irext:{entry['remote']} (remote_index {entry['remote_index']}, irext core {core_version})"
    profile['maturity'] = 'experimental'
    return profile, plans


def user_values(fields, plan, s):
    """What ir_remote encodes for state s, limited to the fields its message reads (oracle JSON form)."""
    if not s['power']:
        return {'power': False}
    values = {'power': True}
    for field in ('mode', 'temperature', 'fan', 'swing_v', 'key'):
        if field in plan['fields'] or (field in ('mode', 'key') and field in fields):
            values[field] = field_json(field, s[STATE_KEY[field]])
    return values


def internal_values(json_values):
    return {field: json_to_value(field, value) for field, value in json_values.items()}


# ---------------------------------------------------------------------------
# Verification

def fix_moves_swing(p):
    """WIND_FIX changes the frame through libirdecode's swing position only for multi-position NORMAL swing."""
    return p['swing']['type'] == 'normal' and p['swing']['count'] > 1 and segment_of(p, 'swing', 0) is not None


def mode_bases(plans):
    return [next(message_states({**plan, 'modes': [mode]})) for plan in plans for mode in plan['modes']]


def verify_decoder(library, binary, p, plans):
    """Model == libirdecode for every legal state reachable from a fresh session, positions via WIND_FIX."""
    fix_moves = fix_moves_swing(p)
    compared = 0
    with Decoder(library, binary, p['id']) as decoder:
        states = [s for plan in plans for s in message_states(plan) if s['swing'] == 0 and not (fix_moves and s['key'] == F_FIX)]
        for s in states + [OFF_STATE]:
            if decoder.press(s) != irext_pulses(p, irext_payload(p, s)):
                fail(f"{p['id']}: model differs from libirdecode for {s}")
            compared += 1
        positions = sorted({swing for plan in plans for legal in plan['legal'].values() for swing in legal['swing'] if swing})
        for position in positions if fix_moves else []:
            decoder.reopen()
            bases = mode_bases(plans)
            pulses = []
            for _ in range(position):
                pulses = decoder.press({**bases[0], 'key': F_FIX}, change_direction=1)
            checks = [({**bases[0], 'swing': position, 'key': F_FIX}, pulses)]
            checks += [({**base, 'swing': position, 'key': F_POWER}, decoder.press({**base, 'key': F_POWER})) for base in bases]
            for s, actual in checks:
                if actual != irext_pulses(p, irext_payload(p, s)):
                    fail(f"{p['id']}: model differs from libirdecode for swing position {s}")
                compared += 1
    return compared


def verify_profile(profile, p, plans):
    """Every legal state re-encodes identically from the JSON; every state the limits allow encodes."""
    protocol = profile['protocol']
    if key_only(profile):
        if json_pulses(protocol, internal_values(KEY_ONLY_VALUES)) != irext_pulses(p, irext_payload(p, OFF_STATE)):
            fail(f"{p['id']}: key codeset differs from model")
        return 1
    fields = message_fields(protocol['messages'])
    if json_pulses(protocol, {'power': 0}) != irext_pulses(p, irext_payload(p, OFF_STATE)):
        fail(f"{p['id']}: JSON off frame differs from model")
    count = 1
    for plan in plans:
        for s in message_states(plan):
            values = internal_values(user_values(fields, plan, s))
            if not limits_allow(profile, values):
                fail(f"{p['id']}: limits reject legal state {s}")
            if json_pulses(protocol, values) != irext_pulses(p, irext_payload(p, s)):
                fail(f"{p['id']}: JSON encoding differs from model for {s}")
            count += 1
    names = [name for name in ('mode', 'temperature', 'fan', 'swing_v', 'key') if name in profile['limits']]
    domains = []
    for name in names:
        limit = profile['limits'][name]
        domains.append([json_to_value(name, value) for value in limit] if isinstance(limit, list) else [value * DECI for value in range(limit['min'], limit['max'] + 1)])
    for combo in product(*domains):
        values = {'power': 1, **dict(zip(names, combo))}
        if limits_allow(profile, values) and not json_encodes(protocol, values):
            fail(f"{p['id']}: limits allow {values} but it does not encode")
    defaults = internal_values({**profile['defaults'], **({'key': 'power'} if 'key' in fields else {})})
    if not limits_allow(profile, defaults) or not json_encodes(protocol, defaults):
        fail(f"{p['id']}: defaults are not encodable")
    return count


def oracle_vectors(profile, p, plans):
    """Mode bases plus one-field variations, restricted to states verified against libirdecode."""
    if key_only(profile):
        return [(dict(KEY_ONLY_VALUES), irext_pulses(p, irext_payload(p, OFF_STATE)))]
    fields = message_fields(profile['protocol']['messages'])
    fix_moves = fix_moves_swing(p)
    picked = [(None, OFF_STATE)]
    for plan in plans:
        bases = mode_bases([plan])
        picked += [(plan, base) for base in bases]
        base = bases[0]
        for field in plan['fields']:
            for value in plan['modes'] if field == 'mode' else plan['legal'][base['mode']][STATE_KEY[field]]:
                if not (field == 'key' and value == F_FIX and fix_moves):
                    picked.append((plan, {**base, STATE_KEY[field]: value}))
    vectors = []
    seen = set()
    for plan, s in picked:
        if field_mode_illegal(plan, s):
            continue
        values = user_values(fields, plan, s)
        identity = json.dumps(values, sort_keys=True)
        if identity not in seen:
            seen.add(identity)
            vectors.append((values, irext_pulses(p, irext_payload(p, s))))
    return vectors


def field_mode_illegal(plan, s):
    if plan is None:
        return False
    legal = plan['legal'][s['mode']]
    return any(legal[k] and s[k] not in legal[k] for k in ('temp', 'fan', 'swing', 'key'))


def fnv1a32(pulses):
    h = 2166136261
    for pulse in pulses:
        h = ((h ^ pulse) * 16777619) & 0xFFFFFFFF
    return h


def lua_value(value):
    if isinstance(value, bool):
        return 'true' if value else 'false'
    return json.dumps(value)


def render_oracle(manifest, rows):
    lines = [
        f"-- Generated by components/lua_modules/lua_module_ir/protocols/irext_generate.py from IRext core {manifest['core_commit']}",
        f"-- and database {manifest['database_commit']}; pulses are libirdecode output.",
        '-- {protocol_id, {values}, carrier_hz, pulse_count, fnv1a32}',
        'return {',
    ]
    for profile_id, values, pulses in rows:
        state = ', '.join(f'{key} = {lua_value(value)}' for key, value in values.items())
        lines.append(f'{{"{profile_id}", {{{state}}}, {CARRIER_HZ}, {len(pulses)}, {fnv1a32(pulses)}}},')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def generate(args):
    manifest = load_manifest(Path(__file__).with_name('irext_profiles.json'))
    verify_repo(args.core_repo, manifest['core_commit'], 'core')
    verify_repo(args.database_repo, manifest['database_commit'], 'database')
    binaries = verify_database(manifest, args.database_repo)
    profiles = []
    rows = []
    with tempfile.TemporaryDirectory(prefix='irext-decoder-') as temporary:
        library = build_decoder(args.core_repo, Path(temporary), manifest['core_version'])
        for entry in manifest['profiles']:
            p = parse_protocol(binaries[entry['id']], entry['id'])
            p['id'] = entry['id']
            profile, plans = build_profile(p, entry, manifest['core_version'])
            decoder_frames = verify_decoder(library, binaries[entry['id']], p, plans)
            json_frames = verify_profile(profile, p, plans)
            vectors = oracle_vectors(profile, p, plans)
            rows += [(entry['id'], values, pulses) for values, pulses in vectors]
            profiles.append(profile)
            fields = sorted(message_fields(profile['protocol']['messages']) - {'power'})
            print(f"{entry['id']:32s} messages={len(profile['protocol']['messages'])} fields={','.join(fields) or '-'} decoder={decoder_frames} json={json_frames} oracle={len(vectors)}", file=sys.stderr)
    return manifest, profiles, rows


def write_bundle(path, profiles):
    """Replace the IRext lines of a JSONL bundle and keep the other codesets, one per line sorted by id."""
    lines = {}
    if path.exists():
        for line in filter(str.strip, path.read_text(encoding='utf-8').splitlines()):
            codeset_id = json.loads(line)['id']
            if not ID_RE.match(codeset_id):
                lines[codeset_id] = line
    for profile in profiles:
        lines[profile['id']] = json.dumps(profile, ensure_ascii=False, separators=(',', ':'))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(''.join(lines[key] + '\n' for key in sorted(lines)), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description='Generate JSON protocol codesets from the pinned IRext manifest.')
    parser.add_argument('--core-repo', type=Path, required=True)
    parser.add_argument('--database-repo', type=Path, required=True)
    parser.add_argument('--protocol-bundle', type=Path, help='update the IRext codesets in this JSONL bundle')
    parser.add_argument('--oracle-output', type=Path, help='write the Lua pulse oracle to this path')
    args = parser.parse_args()
    try:
        manifest, profiles, rows = generate(args)
        if args.protocol_bundle:
            write_bundle(args.protocol_bundle, profiles)
        if args.oracle_output:
            args.oracle_output.parent.mkdir(parents=True, exist_ok=True)
            args.oracle_output.write_text(render_oracle(manifest, rows), encoding='utf-8')
    except GenerationError as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    except (OSError, sqlite3.Error, zipfile.BadZipFile) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
