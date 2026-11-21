#!/usr/bin/env python3
"""Build and run endpoint/runtime regressions on a host with GCC and cJSON sources."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
MODULES = HERE.parents[1]
REPO = MODULES.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, default=(
        REPO / "application/edge_agent/managed_components/espressif__cjson/cJSON"))
    args = parser.parse_args()
    if not (args.cjson_dir / "cJSON.c").exists():
        parser.error("Supply --cjson-dir pointing to the directory containing cJSON.c and cJSON.h")
    sources = [
        HERE / "test_endpoint.c",
        MODULES / "claw_endpoint/src/claw_endpoint.c",
        MODULES / "claw_endpoint/src/claw_endpoint_static.c",
        MODULES / "claw_utils/src/claw_utils_string.c",
        MODULES / "claw_http/src/claw_http.c",
        MODULES / "claw_core/src/llm/claw_llm_runtime.c",
        MODULES / "claw_core/src/llm/claw_llm_http_transport.c",
        *sorted((MODULES / "claw_core/src/llm/backends").glob("*.c")),
        args.cjson_dir / "cJSON.c",
    ]
    includes = [HERE / "include", args.cjson_dir]
    includes += [MODULES / name / "include" for name in ("claw_endpoint", "claw_utils", "claw_http")]
    includes += [MODULES / "claw_core/src"]
    with tempfile.TemporaryDirectory(prefix="claw-endpoint-test-") as temporary:
        binary = Path(temporary) / "test_endpoint"
        cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-include", str(HERE / "include/host_compat.h")]
        cmd += ["-I" + str(p) for p in includes]
        cmd += [str(p) for p in sources] + ["-lm", "-o", str(binary)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
