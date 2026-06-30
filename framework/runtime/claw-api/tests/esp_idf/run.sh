#!/usr/bin/env bash
# Build, flash, and monitor the claw-api on-device test app.
#
# Usage:
#   ./run.sh              # build + flash + monitor (default)
#   ./run.sh build        # build only
#   ./run.sh flash        # flash + monitor
#   ./run.sh monitor      # monitor only
#
# Requires:
#   - ESP-IDF installed (IDF_PATH or ~/esp/esp-idf)
#   - Espressif Rust toolchain (`espup install`)
#   - main/test_secrets.h (copy from test_secrets.h.example)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# --- Locate and export ESP-IDF ---
if [ -z "${IDF_PATH:-}" ]; then
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        export IDF_PATH="$HOME/esp/esp-idf"
    else
        echo "ERROR: IDF_PATH not set and ~/esp/esp-idf not found" >&2
        exit 1
    fi
fi
# shellcheck source=/dev/null
. "$IDF_PATH/export.sh" >/dev/null 2>&1

# --- Check secrets ---
if [ ! -f main/test_secrets.h ]; then
    echo "ERROR: main/test_secrets.h not found." >&2
    echo "  cp main/test_secrets.h.example main/test_secrets.h" >&2
    echo "  then fill in Wi-Fi + LLM endpoint credentials." >&2
    exit 1
fi

# --- Ensure target is set (creates build/ on first run) ---
if [ ! -d build ]; then
    idf.py set-target esp32s3
fi

ACTION="${1:-all}"

case "$ACTION" in
    build)
        idf.py build
        ;;
    flash)
        idf.py flash monitor
        ;;
    monitor)
        idf.py monitor
        ;;
    all)
        idf.py build flash monitor
        ;;
    *)
        echo "Usage: $0 [build|flash|monitor|all]" >&2
        exit 1
        ;;
esac
