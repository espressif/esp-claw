#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
RUNTIME_DIR="$SCRIPT_DIR/public/runtime"
BUILD_ROOT="$REPO_ROOT/build"
BUILD_DIR="$BUILD_ROOT/lua_lvgl_web_sim"
IMAGE="emscripten/emsdk:latest"
RUNTIME_FILES=(esp_claw_sim.html esp_claw_sim.js esp_claw_sim.wasm esp_claw_sim.data)

full_cleanup() {
  local file failed=0
  for file in "${RUNTIME_FILES[@]}"; do
    rm -f -- "$RUNTIME_DIR/$file" || failed=1
  done
  rm -rf -- "$SCRIPT_DIR/dist" "$SCRIPT_DIR/node_modules" || failed=1

  if ! rm -rf -- "$BUILD_DIR"; then
    if command -v docker >/dev/null 2>&1 && docker image inspect "$IMAGE" >/dev/null 2>&1; then
      docker run --rm -v "$BUILD_ROOT:/cleanup" "$IMAGE" bash -lc 'rm -rf /cleanup/lua_lvgl_web_sim' || failed=1
    fi
  fi
  if [[ -e "$BUILD_DIR" ]]; then
    echo "Error: failed to remove $BUILD_DIR" >&2
    failed=1
  else
    rmdir "$BUILD_ROOT" 2>/dev/null || true
  fi

  if command -v docker >/dev/null 2>&1; then
    if docker info >/dev/null 2>&1; then
      if docker image inspect "$IMAGE" >/dev/null 2>&1; then
        docker image rm "$IMAGE" || failed=1
      fi
    else
      echo "Error: Docker is not running; image was not removed" >&2
      failed=1
    fi
  fi

  (( failed == 0 )) || return 1
  echo "Simulator cleanup complete"
}

case "${1:-}" in
  '') ;;
  --clean)
    [[ $# -eq 1 ]] || { echo "Usage: $0 [--clean]" >&2; exit 2; }
    full_cleanup
    exit 0
    ;;
  *)
    echo "Usage: $0 [--clean]" >&2
    exit 2
    ;;
esac

command -v docker >/dev/null 2>&1 || { echo "Error: docker is required" >&2; exit 1; }
command -v pnpm >/dev/null 2>&1 || { echo "Error: pnpm is required" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "Error: Docker is not running" >&2; exit 1; }

mkdir -p "$BUILD_DIR" "$RUNTIME_DIR"

# Keep the build cache for fast subsequent runs.
docker run --rm \
  -e SIM_HOST_UID="$(id -u)" \
  -e SIM_HOST_GID="$(id -g)" \
  -e EM_CACHE=/build/.emcache \
  -v "$REPO_ROOT:/work:ro" \
  -v "$BUILD_DIR:/build" \
  -w /work \
  "$IMAGE" \
  bash -lc 'cleanup() { chown -R "$SIM_HOST_UID:$SIM_HOST_GID" /build; }; trap cleanup EXIT; trap "exit 130" INT; trap "exit 143" TERM; if ! command -v ninja >/dev/null; then apt-get update && apt-get install -y --no-install-recommends ninja-build; fi; emcmake cmake -S tools/lua_lvgl_web_sim -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && embuilder build sdl2 && emmake ninja -C /build esp_claw_sim'

cd "$SCRIPT_DIR"
pnpm install --frozen-lockfile
SIMULATOR_BUILD_DIR="$BUILD_DIR" pnpm copy-runtime

echo "Simulator: http://localhost:5173/?repo=skills-lab&ref=main&app=apps/flappybird/launcher.json"
pnpm dev
