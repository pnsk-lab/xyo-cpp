#!/usr/bin/env python3
"""Preflight the five LLVM 18/nanosvg CTests before launching them.

CMake can register tests whose binaries were not built with a selective
target invocation. A missing test may otherwise appear as Not Run and the
purpose of testing LLVM 18's *native* ORC execution can be missed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REQUIRED_TESTS = frozenset({
    "xyo-render-tests",
    "xyo-jit-parfor-partition-policy",
    "xyo-jit-lazy-parfor-callback",
    "xyo-jit-lazy-parfor-partition",
    "xyo-jit-lazy-parfor-longrun",
})


def validate_tests(tests: list[dict], build_dir: Path) -> list[str]:
    by_name: dict[str, dict] = {}
    for test in tests:
        name = test.get("name")
        if name in REQUIRED_TESTS:
            if name in by_name:
                raise ValueError(f"duplicate CTest registration: {name}")
            by_name[name] = test
    missing = REQUIRED_TESTS - by_name.keys()
    if missing:
        raise ValueError(f"missing registered CTests: {sorted(missing)}")

    checked = []
    for name, test in sorted(by_name.items()):
        command = test.get("command")
        if not isinstance(command, list) or not command:
            raise ValueError(f"{name}: no runnable command")
        binary = Path(command[0])
        if not binary.is_absolute():
            binary = (build_dir / binary).resolve()
        if not binary.is_file():
            raise ValueError(f"{name}: executable is not built: {binary}")

        if name in {"xyo-jit-lazy-parfor-callback", "xyo-jit-lazy-parfor-partition"}:
            if "--native" not in command:
                raise ValueError(f"{name}: missing --native argument")
            index = command.index("--native")
            if index + 1 >= len(command):
                raise ValueError(f"{name}: missing native executable")
            native = Path(command[index + 1])
            if not native.is_file():
                raise ValueError(f"{name}: native xyo CLI not built: {native}")
            if native.name != "xyo":
                raise ValueError(f"{name}: wrong native executable: {native.name}")
        checked.append(f"{name}: {binary}")
    return checked


class PreflightUnitTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        native = Path(self.temp_dir.name) / "xyo"
        native.write_bytes(b"fixture-binary")
        self.tests = [{
            "name": name,
            "command": (
                [str(Path(sys.executable)), "--native", str(native)]
                if name.startswith("xyo-jit-lazy") else
                [str(Path(sys.executable))]
            ),
        } for name in sorted(REQUIRED_TESTS)]

    def test_valid_inventory(self):
        self.assertEqual(
            len(validate_tests(self.tests, Path("."))),
            len(REQUIRED_TESTS),
        )

    def test_missing_registration(self):
        with self.assertRaisesRegex(ValueError, "missing registered"):
            validate_tests(self.tests[:-1], Path("."))

    def test_longrun_registration_required(self):
        excluded = [
            test for test in self.tests
            if test["name"] != "xyo-jit-lazy-parfor-longrun"
        ]
        with self.assertRaisesRegex(ValueError, "xyo-jit-lazy-parfor-longrun"):
            validate_tests(excluded, Path("."))

    def test_duplicate_registration(self):
        with self.assertRaisesRegex(ValueError, "duplicate CTest"):
            validate_tests(self.tests + [self.tests[0]], Path("."))

    def test_missing_binary(self):
        broken = json.loads(json.dumps(self.tests))
        broken[0]["command"][0] = "/path/that/does/not/exist"
        with self.assertRaisesRegex(ValueError, "not built"):
            validate_tests(broken, Path("."))

    def test_missing_native(self):
        broken = json.loads(json.dumps(self.tests))
        for test in broken:
            if test["name"] == "xyo-jit-lazy-parfor-partition":
                test["command"] = test["command"][:1]
        with self.assertRaisesRegex(ValueError, "missing --native"):
            validate_tests(broken, Path("."))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(PreflightUnitTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        if not result.wasSuccessful():
            raise SystemExit(1)
        return
    if args.build_dir is None:
        parser.error("--build-dir is required")
    build = args.build_dir.resolve(strict=True)
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    for checked in validate_tests(json.loads(proc.stdout)["tests"], build):
        print(f"PASS {checked}")
    print("PASS all five LLVM 18 CTests registered and runnable")


if __name__ == "__main__":
    main()
