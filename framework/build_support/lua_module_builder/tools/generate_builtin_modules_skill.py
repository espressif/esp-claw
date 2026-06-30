#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from lua_sync_common import ComponentSource, LuaSyncConsole, LuaSyncError, collect_build_component_sources, write_depfile, write_stamp


console = LuaSyncConsole()
from sync_lua_module_docs import collect_lua_module_doc_entries


GENERATED_SKILL_ID = 'builtin_lua_modules'
GENERATED_SKILL_FILE = 'SKILL.md'
GENERATED_SKILL_META = {
    'name': 'builtin_lua_modules',
    'description': 'Built-in Lua module documentation.',
    'metadata': {
        'cap_groups': ['cap_lua'],
        'manage_mode': 'readonly',
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Generate the builtin Lua modules skill from lua_module docs.')
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--stamp-path', required=True)
    parser.add_argument('--depfile', required=True)
    return parser.parse_args()


def collect_lua_module_test_script_entries(sources: list[ComponentSource]) -> list[dict]:
    entries: list[dict] = []

    for source in sources:
        test_dir = source.root / 'test'
        if not test_dir.is_dir():
            continue

        for script_path in sorted(test_dir.rglob('*.lua')):
            if not script_path.is_file():
                continue
            relative_path = script_path.relative_to(test_dir)
            entries.append({'source_name': source.name, 'script_path': script_path.resolve(), 'file': str(relative_path)})

    entries.sort(key=lambda entry: (str(entry['source_name']), str(entry['file'])))
    return entries


def collect_lua_module_skill_entries(sources: list[ComponentSource]) -> list[dict]:
    entries_by_source: dict[str, dict] = {}

    for doc_entry in collect_lua_module_doc_entries(sources):
        source_name = str(doc_entry['source_name'])
        entry = entries_by_source.setdefault(source_name, {'source_name': source_name, 'docs': [], 'doc_paths': [], 'tests': [], 'test_paths': []})
        entry['docs'].append(str(doc_entry['file']))
        entry['doc_paths'].append(Path(doc_entry['doc_path']))

    for test_entry in collect_lua_module_test_script_entries(sources):
        source_name = str(test_entry['source_name'])
        entry = entries_by_source.setdefault(source_name, {'source_name': source_name, 'docs': [], 'doc_paths': [], 'tests': [], 'test_paths': []})
        entry['tests'].append(str(test_entry['file']))
        entry['test_paths'].append(Path(test_entry['script_path']))

    return sorted(entries_by_source.values(), key=lambda entry: str(entry['source_name']))


def render_table_cell(values: list[str]) -> str:
    if not values:
        return '-'
    return '<br>'.join(f'`{value}`' for value in values)


def render_generated_skill(entries: list[dict]) -> str:
    lines = ['---', json.dumps(GENERATED_SKILL_META, indent=2), '---', '', '# Builtin Lua Modules', '']
    if not entries:
        lines.extend(['No compiled lua_module/lua_driver component currently provides markdown docs or test scripts.', ''])
        return '\n'.join(lines)

    lines.extend(['To read documentation for a module, call `read_file("{CUR_SKILL_DIR}/scripts/docs/<Doc file path>")`.',
                  'To read a module test script, call `read_file("{CUR_SKILL_DIR}/scripts/builtin/test/<Test script path>")`.',
                  'Read all the files you need in one go as much as possible.'])
    lines.extend(['Do not fabricate functions that are not documented.', ''])
    lines.extend(['| Module | Doc file path | Test script path |', '| --- | --- | --- |'])
    for entry in entries:
        # Keep this generated skill as a compact index. The full docs and test scripts are synced into FATFS directories.
        lines.append(f'| `{entry["source_name"]}` | {render_table_cell(entry["docs"])} | {render_table_cell(entry["tests"])} |')
    lines.append('')

    return '\n'.join(lines)


def write_generated_skill(skill_output_dir: Path, entries: list[dict]) -> None:
    content = render_generated_skill(entries)
    generated_skill_dir = skill_output_dir / GENERATED_SKILL_ID
    generated_skill_dir.mkdir(parents=True, exist_ok=True)
    (generated_skill_dir / GENERATED_SKILL_FILE).write_text(content, encoding='utf-8')


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    skill_output_dir = Path(__file__).resolve().parents[1] / 'skills'
    stamp_path = Path(args.stamp_path).resolve()
    depfile_path = Path(args.depfile).resolve()

    sources = collect_build_component_sources(build_dir, name_prefix=('lua_module_', 'lua_driver_'))
    entries = collect_lua_module_skill_entries(sources)
    write_generated_skill(skill_output_dir, entries)
    input_paths = [path for entry in entries for path in entry['doc_paths'] + entry['test_paths']]
    write_depfile(depfile_path, stamp_path, input_paths)
    write_stamp(stamp_path)
    console.success(f'CLAW lua modules skill generation updated {len(entries)} module entries into {skill_output_dir}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except LuaSyncError as exc:
        console.error(f'generate_builtin_modules_skill.py: error: {exc}')
        sys.exit(1)
