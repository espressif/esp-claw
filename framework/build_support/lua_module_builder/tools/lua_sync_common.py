#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO


class LuaSyncError(RuntimeError):
    pass


COLOR_GREEN = '\033[32m'
COLOR_RED = '\033[31m'
COLOR_YELLOW = '\033[33m'
COLOR_CYAN = '\033[36m'
COLOR_RESET = '\033[0m'


@dataclass(frozen=True)
class ComponentSource:
    name: str
    root: Path


class LuaSyncConsole:
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


def fail(message: str) -> None:
    raise LuaSyncError(message)


def load_json_file(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        fail(f'Missing JSON file: {path}')
    except json.JSONDecodeError as exc:
        fail(f'Invalid JSON in {path}: {exc}')


def load_build_component_info(build_dir: Path) -> dict[str, dict]:
    project_description_path = build_dir / 'project_description.json'
    data = load_json_file(project_description_path)
    if not isinstance(data, dict):
        fail(f'Project description must be a JSON object: {project_description_path}')

    component_info = data.get('build_component_info')
    if not isinstance(component_info, dict):
        fail(f"Project description does not contain a valid 'build_component_info' object: {project_description_path}")
    return component_info


def collect_build_component_sources(build_dir: Path, name_prefix: str | tuple[str, ...] | None = None) -> list[ComponentSource]:
    component_info = load_build_component_info(build_dir)
    sources: list[ComponentSource] = []

    for component_name, info in component_info.items():
        if not isinstance(component_name, str):
            continue
        if name_prefix and not component_name.startswith(name_prefix):
            continue
        if not isinstance(info, dict):
            fail(f"Component info for '{component_name}' must be a JSON object.")

        raw_dir = info.get('dir')
        if not isinstance(raw_dir, str) or not raw_dir:
            fail(f"Component '{component_name}' does not declare a valid directory in project_description.json.")
        sources.append(ComponentSource(component_name, Path(raw_dir).resolve()))

    sources.sort(key=lambda item: item.name)
    return sources


def load_synced_files_manifest(path: Path) -> set[str]:
    if not path.exists():
        return set()

    data = load_json_file(path)
    if not isinstance(data, dict):
        fail(f'Manifest must be a JSON object: {path}')
    return {str(item) for item in data.get('synced_files', [])}


def write_synced_files_manifest(path: Path, synced_files: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    manifest_data = {'synced_files': sorted(synced_files)}
    path.write_text(json.dumps(manifest_data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


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


class FileSyncPlan:
    def __init__(self, output_dir: Path, manifest_path: Path) -> None:
        self.output_dir = output_dir
        self.manifest_path = manifest_path
        self._copy_map: dict[str, Path] = {}
        self._source_map: dict[str, str] = {}

    def add(self, output_name: str, source_path: Path, owner: str) -> None:
        previous_source = self._source_map.get(output_name)
        if previous_source:
            fail(f"Duplicate synced file '{output_name}' between {previous_source} and {owner} ({source_path})")

        self._copy_map[output_name] = source_path.resolve()
        self._source_map[output_name] = owner

    def apply(self) -> None:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        previous_files = load_synced_files_manifest(self.manifest_path)

        for old_file in previous_files:
            if old_file not in self._copy_map:
                stale_path = self.output_dir / old_file
                if stale_path.exists():
                    stale_path.unlink()

        # Copy the current managed files and keep stale outputs out of the FATFS image.
        for filename, source_path in self._copy_map.items():
            target_path = self.output_dir / filename
            target_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_path, target_path)

        write_synced_files_manifest(self.manifest_path, list(self._copy_map.keys()))

    @property
    def input_paths(self) -> list[Path]:
        return list(self._copy_map.values())

    @property
    def count(self) -> int:
        return len(self._copy_map)
