from pathlib import Path
import csv
import subprocess
import sys

Import("env")


def parse_size(value):
    text = value.strip()
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def storage_partition_size(project_dir):
    partitions = project_dir / "partitions.csv"
    with partitions.open(newline="") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#")):
            if len(row) >= 5 and row[0].strip() == "storage":
                return parse_size(row[4])
    raise RuntimeError("storage partition not found in partitions.csv")


def generate_storage_image(*args, **kwargs):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    image_dir = project_dir / "fatfs_image"
    out_file = build_dir / "storage.bin"
    build_dir.mkdir(parents=True, exist_ok=True)

    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    if not framework_dir:
        raise RuntimeError("framework-espidf package is not installed")
    generator = Path(framework_dir) / "components" / "fatfs" / "wl_fatfsgen.py"

    try:
        import construct  # noqa: F401
    except ImportError as exc:
        raise RuntimeError("Install virtualenv dependencies first: .venv/bin/python -m pip install -r requirements.txt") from exc

    cmd = [
        sys.executable,
        str(generator),
        str(image_dir),
        "--long_name_support",
        "--use_default_datetime",
        "--partition_size",
        str(storage_partition_size(project_dir)),
        "--output_file",
        str(out_file),
        "--sector_size",
        "4096",
    ]
    subprocess.check_call(cmd)


generate_storage_image()
env.AddPreAction("upload", generate_storage_image)
