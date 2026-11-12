#!/usr/bin/env python3
"""Fail fast when native continuation CTests depend on uncommitted sources.

This runs before CMake configuration in GitHub Actions. In a local dirty
working tree, --allow-untracked provides a non-mutating inventory check.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "tests" / "CMakeLists.txt"
OSR016_CMAKE = ROOT / "tests" / "differential" / "osr016_fractional_boundary_tests.cmake"
TEST_NAME = re.compile(
    r"add_test\(\s*NAME\s+"
    r"(xyo-nested-warp-[\w-]+|xyo-native-simple-deopt-parity)\b"
    r"(.*?)\)",
    re.DOTALL,
)
HARNESS = re.compile(
    r"add_executable\(\s*(xyo-nested-warp-[\w-]+-smoke)"
    r"\s+platform/([\w_]+\.cpp)\s*\)"
)
PYTHON_SOURCE = re.compile(r"differential/([\w-]+\.py)")


def required_files() -> tuple[set[Path], int, int, int]:
    cmake = CMAKE.read_text(encoding="utf-8")
    osr016 = OSR016_CMAKE.read_text(encoding="utf-8")

    tests = TEST_NAME.findall(cmake + "\n" + osr016)
    if len(tests) < 38:
        raise RuntimeError(f"Expected at least 38 native continuation CTests, found {len(tests)}")

    harnesses = HARNESS.findall(cmake)
    if len(harnesses) < 9:
        raise RuntimeError(f"Expected at least 9 native harnesses, found {len(harnesses)}")

    source_files = {
        ROOT / "tests" / "platform" / source
        for _, source in harnesses
    }
    scripts = {
        ROOT / "tests" / "differential" / name
        for _, body in tests
        for name in PYTHON_SOURCE.findall(body)
    }
    # OSR-016 expands its command via the _xyo_osr016_script CMake variable.
    scripts.update(
        ROOT / "tests" / "differential" / name
        for name in PYTHON_SOURCE.findall(osr016)
    )
    if len(scripts) < 22:
        raise RuntimeError(f"Expected at least 22 Python scripts, found {len(scripts)}")

    source_files.update(scripts)
    source_files.add(CMAKE)
    source_files.add(OSR016_CMAKE)
    source_files.add(Path(__file__).resolve())
    source_files.add(Path(__file__).with_name('test_native_continuation_checkout.py'))
    return source_files, len(tests), len(harnesses), len(scripts)


def committed_files() -> set[str]:
    # Only HEAD is published by actions/checkout. An index-staged but
    # uncommitted fixture must still be rejected by this audit.
    result = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", "-z", "HEAD"],
        cwd=ROOT,
    )
    return {
        item.decode("utf-8", errors="surrogateescape")
        for item in result.split(b"\0") if item
    }


def modified_files(paths: set[str]) -> set[str]:
    output = subprocess.check_output(
        ["git", "diff", "--name-only", "-z", "HEAD", "--", *sorted(paths)],
        cwd=ROOT,
    )
    return {
        name.decode("utf-8", errors="surrogateescape")
        for name in output.split(b"\0") if name
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--allow-uncommitted", "--allow-untracked",
        dest="allow_uncommitted", action="store_true",
        help="Diagnostic mode for a dirty local tree; never used in CI.",
    )
    args = parser.parse_args()

    sources, test_count, harness_count, script_count = required_files()
    tracked = committed_files()
    missing = []
    untracked = []
    required_names = {path.relative_to(ROOT).as_posix() for path in sources}
    dirty = modified_files(required_names) & tracked
    for path in sorted(sources):
        relative = path.relative_to(ROOT).as_posix()
        if not path.is_file():
            missing.append(relative)
        elif relative not in tracked:
            untracked.append(relative)

    print(
        f"Native continuation sources: {test_count} tests, "
        f"{harness_count} C++ harnesses, {script_count} Python scripts"
    )
    for relative in missing:
        print(f"MISSING: {relative}", file=sys.stderr)
    for relative in untracked:
        print(f"UNTRACKED: {relative}", file=sys.stderr)
    for relative in sorted(dirty):
        print(f"UNCOMMITTED_CHANGE: {relative}", file=sys.stderr)

    if missing:
        return 1
    if (untracked or dirty) and not args.allow_uncommitted:
        print(
            "CI requires all source dependencies to be committed in HEAD.",
            file=sys.stderr,
        )
        return 1

    print(
        "PASS: required files exist"
        + (" (uncommitted files allowed locally)"
           if untracked or dirty else " and match HEAD")
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"Source audit failed: {error}", file=sys.stderr)
        raise SystemExit(1)
