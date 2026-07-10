#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

from skill_sync_common import SkillSyncConsole


SKILL_FRONTMATTER_RE = re.compile(r'\A---\s*\n(?P<meta>\{.*?\})\s*\n---\s*(?:\n|$)', re.DOTALL)
SKILL_DOCUMENT_NAME = 'SKILL.md'
console = SkillSyncConsole()


class SkillSyncError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Sync component skill markdown files into the application FATFS image.')
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--skill-output-dir', required=True)
    parser.add_argument('--stamp-path', required=True)
    parser.add_argument('--depfile', required=True)
    return parser.parse_args()


def fail(message: str) -> None:
    raise SkillSyncError(message)


def load_json_file(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        fail(f'Missing JSON file: {path}')
    except json.JSONDecodeError as exc:
        fail(f'Invalid JSON in {path}: {exc}')


def load_manifest(path: Path) -> dict:
    if not path.exists():
        return {'component_files': []}

    data = load_json_file(path)
    if not isinstance(data, dict):
        fail(f'Manifest must be a JSON object: {path}')
    return {'component_files': [str(item) for item in data.get('component_files', [])]}


def write_stamp(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('ok\n', encoding='utf-8')


def escape_depfile_path(path: Path) -> str:
    return str(path).replace('\\', '\\\\').replace(' ', '\\ ')


def write_depfile(depfile_path: Path, stamp_path: Path, input_paths: list[Path]) -> None:
    unique_inputs = sorted({path.resolve() for path in input_paths}, key=lambda item: str(item))
    dependencies = ' '.join(escape_depfile_path(path) for path in unique_inputs)
    target = escape_depfile_path(stamp_path.name)
    depfile_path.parent.mkdir(parents=True, exist_ok=True)
    depfile_path.write_text(f'{target}: {dependencies}\n', encoding='utf-8')


def parse_skill_meta(skill_path: Path) -> dict:
    text = skill_path.read_text(encoding='utf-8')
    match = SKILL_FRONTMATTER_RE.match(text)
    if not match:
        fail(f'{skill_path} must start with JSON frontmatter delimited by ---.')

    try:
        meta = json.loads(match.group('meta'))
    except json.JSONDecodeError as exc:
        fail(f'Invalid frontmatter JSON in {skill_path}: {exc}')

    if not isinstance(meta, dict):
        fail(f'Frontmatter in {skill_path} must decode to a JSON object.')
    return meta


def require_skill_id(skill_path: Path, meta: dict) -> str:
    skill_id = meta.get('name')
    if not isinstance(skill_id, str) or not skill_id:
        fail(f'{skill_path} frontmatter must contain a non-empty string name.')
    return skill_id


def load_build_component_info(build_dir: Path) -> dict[str, dict]:
    project_description_path = build_dir / 'project_description.json'
    data = load_json_file(project_description_path)
    if not isinstance(data, dict):
        fail(f'Project description must be a JSON object: {project_description_path}')

    component_info = data.get('build_component_info')
    if not isinstance(component_info, dict):
        fail(f"Project description does not contain a valid 'build_component_info' object: {project_description_path}")
    return component_info


def collect_skill_sources(build_dir: Path) -> list[tuple[str, Path]]:
    component_info = load_build_component_info(build_dir)
    sources: list[tuple[str, Path]] = []

    for component_name, info in sorted(component_info.items()):
        if not isinstance(component_name, str) or not component_name:
            continue
        if not isinstance(info, dict):
            fail(f"Component info for '{component_name}' must be a JSON object.")

        raw_dir = info.get('dir')
        if not isinstance(raw_dir, str) or not raw_dir:
            fail(f"Component '{component_name}' does not declare a valid directory in project_description.json.")

        sources.append((component_name, Path(raw_dir).resolve()))

    return sources


def collect_component_skills(sources: list[tuple[str, Path]]) -> dict[str, Path]:
    copy_map: dict[str, Path] = {}
    skill_id_sources: dict[str, str] = {}
    skill_file_sources: dict[str, str] = {}

    for source_name, source_root in sources:
        skills_dir = source_root / 'skills'
        if not skills_dir.is_dir():
            continue

        for skill_dir in sorted(skills_dir.iterdir()):
            if not skill_dir.is_dir():
                continue

            skill_path = skill_dir / SKILL_DOCUMENT_NAME
            if not skill_path.is_file():
                fail(f'{skill_dir} must contain {SKILL_DOCUMENT_NAME}.')

            resolved_path = skill_path.resolve()
            skill_id = require_skill_id(skill_path, parse_skill_meta(skill_path))
            if skill_dir.name != skill_id:
                fail(f'{skill_path} parent directory must match skill id {skill_id}.')

            previous_skill_id_source = skill_id_sources.get(skill_id)
            if previous_skill_id_source:
                fail(f"Duplicate skill id '{skill_id}' between {previous_skill_id_source} and {source_name} ({resolved_path})")

            # Copy the whole skill directory so references, scripts, and assets remain available in the FATFS image.
            for source_file in sorted(path for path in skill_dir.rglob('*') if path.is_file()):
                skill_file = str(Path(skill_id) / source_file.relative_to(skill_dir))
                previous_skill_file_source = skill_file_sources.get(skill_file)
                if previous_skill_file_source:
                    fail(f"Duplicate skill file '{skill_file}' between {previous_skill_file_source} and {source_name} ({source_file.resolve()})")
                copy_map[skill_file] = source_file.resolve()
                skill_file_sources[skill_file] = f'{source_name} ({source_file.resolve()})'

            skill_id_sources[skill_id] = f'{source_name} ({resolved_path})'

    return copy_map


def count_synced_skills(copy_map: dict[str, Path]) -> int:
    return len({Path(filename).parts[0] for filename in copy_map if Path(filename).parts})


def sync_markdown_files(skill_output_dir: Path, manifest_path: Path, manifest: dict, copy_map: dict[str, Path]) -> None:
    skill_output_dir.mkdir(parents=True, exist_ok=True)

    for old_file in manifest.get('component_files', []):
        if old_file not in copy_map:
            stale_path = skill_output_dir / old_file
            if stale_path.exists():
                stale_path.unlink()

    # Keep the output directory aligned with the current set of component and main skill files.
    for filename, source_path in copy_map.items():
        target_path = skill_output_dir / filename
        target_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target_path)

    manifest_data = {'component_files': sorted(copy_map.keys())}
    manifest_path.write_text(json.dumps(manifest_data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    skill_output_dir = Path(args.skill_output_dir).resolve()
    stamp_path = Path(args.stamp_path).resolve()
    depfile_path = Path(args.depfile).resolve()
    manifest_path = build_dir / 'skill_builder_manifest.json'
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(manifest_path)

    sources = collect_skill_sources(build_dir)
    copy_map = collect_component_skills(sources)
    sync_markdown_files(skill_output_dir, manifest_path, manifest, copy_map)
    write_depfile(depfile_path, stamp_path, list(copy_map.values()))
    write_stamp(stamp_path)
    console.success(f'CLAW skill sync updated {count_synced_skills(copy_map)} skills into {skill_output_dir}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SkillSyncError as exc:
        console.error(f'sync_component_skills.py: error: {exc}')
        sys.exit(1)
