#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import pathlib
import sys

import requests


def load_json(path: pathlib.Path) -> dict:
    with path.open("r", encoding="utf-8") as file_obj:
        return json.load(file_obj)


def main() -> int:
    default_config_path = (
        pathlib.Path(__file__).resolve().parent.parent
        / "local_backup"
        / "espclaw_config_latest.json"
    )

    parser = argparse.ArgumentParser(
        description="Restore ESPCLAW /api/config from a JSON backup"
    )
    parser.add_argument(
        "--device",
        default="http://localhost",
        help="Device base URL (e.g., http://192.168.0.100)",
    )
    parser.add_argument(
        "--config",
        default=str(default_config_path),
        help="Path to config backup JSON",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only print keys that would be updated",
    )
    args = parser.parse_args()

    cfg_path = pathlib.Path(args.config)
    if not cfg_path.exists():
        print(f"Config file not found: {cfg_path}")
        return 1

    new_cfg = load_json(cfg_path)
    base = args.device.rstrip("/")

    try:
        current = requests.get(f"{base}/api/config", timeout=10).json()
    except Exception as exc:
        print(f"Failed to read current config from device: {exc}")
        return 1

    merged = dict(current)
    merged.update(new_cfg)

    changed_keys = [
        key for key in merged.keys() if merged.get(key) != current.get(key)
    ]
    print("Changed keys:", ", ".join(sorted(changed_keys)) if changed_keys else "(none)")

    if args.dry_run:
        print("Dry run only. No changes sent.")
        return 0

    try:
        response = requests.post(f"{base}/api/config", json=merged, timeout=15)
        print("POST /api/config status:", response.status_code)
        print(response.text)
        if response.status_code != 200:
            return 1
    except Exception as exc:
        print(f"Failed to write config to device: {exc}")
        return 1

    print("Config restore sent. Reboot device to apply core changes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())