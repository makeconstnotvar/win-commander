#!/usr/bin/env python3
"""Create deterministic real-filesystem fixtures for the Q1 large-folder gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import random
import subprocess
import sys
import time
from pathlib import Path


SCHEMA_VERSION = 1
SUPPORTED_COUNTS = (0, 10_000, 100_000)
DEFAULT_COUNTS = SUPPORTED_COUNTS
DEFAULT_SEED = 42


def fixture_name(count: int) -> str:
    if count == 0:
        return "q1-empty"
    return f"q1-{count // 1000}k"


def filename_for(index: int, count: int, permutation: list[int]) -> str:
    value = permutation[index]
    prefix = fixture_name(count)
    if index != 0 and index % 997 == 0:
        return f".{prefix}-hidden-{value}.dat"
    if index != 0 and index % 991 == 0:
        return f"{prefix}-данные-{value}.dat"
    return f"{prefix}-item-{value}.dat"


def target_prefix_for(count: int) -> str:
    return fixture_name(count) if count == 0 else f"{fixture_name(count)}-"


def filesystem_type(path: Path) -> str:
    try:
        df_output = subprocess.run(
            ["/bin/df", "-P", str(path)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        mount_point = df_output[-1].split(maxsplit=5)[-1]
        disk_info = subprocess.run(
            ["/usr/sbin/diskutil", "info", mount_point],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        for line in disk_info:
            if line.strip().startswith("Type (Bundle):"):
                return line.split(":", 1)[1].strip()
        return "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def expected_manifest(directory: Path, count: int, seed: int) -> dict[str, object]:
    permutation = list(range(count))
    random.Random(seed + count).shuffle(permutation)
    digest = hashlib.sha256()
    first_name = ""
    for index in range(count):
        name = filename_for(index, count, permutation)
        if index == 0:
            first_name = name
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
    stat_result = directory.stat()
    return {
        "schema_version": SCHEMA_VERSION,
        "seed": seed,
        "count": count,
        "filename_sha256": digest.hexdigest(),
        "target_prefix": target_prefix_for(count),
        "first_created_name": first_name,
        "device": stat_result.st_dev,
        "filesystem": filesystem_type(directory),
    }


def manifest_path_for(directory: Path) -> Path:
    return directory.parent / f"{directory.name}.fixture-manifest.json"


def existing_fixture_is_valid(directory: Path, expected: dict[str, object]) -> bool:
    manifest_path = manifest_path_for(directory)
    try:
        actual = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    keys = ("schema_version", "seed", "count", "filename_sha256", "device")
    if any(actual.get(key) != expected.get(key) for key in keys):
        return False
    try:
        entries = sum(1 for _ in os.scandir(directory))
    except OSError:
        return False
    return entries == expected["count"]


def populate(directory: Path, count: int, seed: int) -> dict[str, object]:
    directory.mkdir(parents=True, exist_ok=True)
    expected = expected_manifest(directory, count, seed)
    if existing_fixture_is_valid(directory, expected):
        expected["path"] = str(directory.resolve())
        expected["status"] = "reused"
        manifest_path_for(directory).write_text(
            json.dumps(expected, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return expected

    unexpected = list(directory.iterdir())
    if unexpected:
        raise RuntimeError(
            f"refusing to replace non-matching fixture at {directory}; remove that exact directory first"
        )

    permutation = list(range(count))
    random.Random(seed + count).shuffle(permutation)
    started = time.monotonic()
    directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for index in range(count):
            name = filename_for(index, count, permutation)
            file_fd = os.open(
                name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                0o600,
                dir_fd=directory_fd,
            )
            os.close(file_fd)
    finally:
        os.close(directory_fd)

    expected["path"] = str(directory.resolve())
    expected["status"] = "created"
    expected["creation_seconds"] = round(time.monotonic() - started, 6)
    expected["created_at_epoch_seconds"] = int(time.time())
    expected["host"] = platform.node()
    manifest_path = manifest_path_for(directory)
    manifest_path.write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return expected


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("/tmp/win-commander-q1-large-folders"),
        help="parent directory for fixture folders",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument(
        "--counts",
        type=int,
        nargs="+",
        default=DEFAULT_COUNTS,
        choices=SUPPORTED_COUNTS,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    results = []
    try:
        for count in args.counts:
            results.append(populate(args.root / fixture_name(count), count, args.seed))
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps({"fixtures": results}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
