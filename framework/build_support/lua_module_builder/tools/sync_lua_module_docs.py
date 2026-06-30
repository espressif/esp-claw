#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from lua_sync_common import ComponentSource, FileSyncPlan, LuaSyncConsole, LuaSyncError, fail
from lua_sync_common import collect_build_component_sources, write_depfile, write_stamp


console = LuaSyncConsole()

# Docs are bundled into the generated builtin_lua_modules skill so they can be
# addressed at runtime via {CUR_SKILL_DIR}/scripts/docs/<name>. skill_builder
# picks them up from this source-tree location and syncs them into the image.
GENERATED_SKILL_DIR = Path(__file__).resolve().parents[1] / 'skills' / 'builtin_lua_modules'
DOCS_DEST_DIR = GENERATED_SKILL_DIR / 'scripts' / 'docs'


def is_self_component(source: ComponentSource) -> bool:
    return source.name == 'lua_module_builder'


def iter_module_doc_mappings(source: ComponentSource) -> list[tuple[str, Path]]:
    doc_path = source.root / 'README.md'
    if not doc_path.is_file():
        fail(f"Component '{source.name}' must provide README.md at {doc_path}")
    return [(f'{source.name}.md', doc_path)]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Sync Lua module markdown docs into the builtin_lua_modules skill.')
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--manifest-path', required=True)
    parser.add_argument('--stamp-path', required=True)
    parser.add_argument('--depfile', required=True)
    return parser.parse_args()


def collect_lua_module_docs(sources: list[ComponentSource], output_dir: Path, manifest_path: Path) -> FileSyncPlan:
    plan = FileSyncPlan(output_dir, manifest_path)

    for source in sources:
        if is_self_component(source):
            continue
        for output_name, doc_path in iter_module_doc_mappings(source):
            plan.add(output_name, doc_path, source.name)

    return plan


def collect_lua_module_doc_entries(sources: list[ComponentSource]) -> list[dict]:
    entries: list[dict] = []

    for source in sources:
        if is_self_component(source):
            continue
        for output_name, doc_path in iter_module_doc_mappings(source):
            entries.append({'source_name': source.name, 'doc_path': doc_path.resolve(), 'file': output_name})

    entries.sort(key=lambda entry: (str(entry['source_name']), str(entry['file'])))
    return entries


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    output_dir = DOCS_DEST_DIR.resolve()
    manifest_path = Path(args.manifest_path).resolve()
    stamp_path = Path(args.stamp_path).resolve()
    depfile_path = Path(args.depfile).resolve()

    sources = collect_build_component_sources(build_dir, name_prefix=('lua_module_', 'lua_driver_'))
    plan = collect_lua_module_docs(sources, output_dir, manifest_path)
    plan.apply()
    write_depfile(depfile_path, stamp_path, plan.input_paths)
    write_stamp(stamp_path)
    console.success(f'CLAW lua docs sync updated {plan.count} markdown files into {output_dir}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except LuaSyncError as exc:
        console.error(f'sync_lua_module_docs.py: error: {exc}')
        sys.exit(1)
