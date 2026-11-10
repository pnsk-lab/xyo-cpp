#!/usr/bin/env python3
"""Check the complete native OSR CTest inventory and run-time file dependencies.

Used after an isolated target-only build, before the expensive regression run.
All required CTest case names are frozen; additions require an explicit review.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

EXPECTED = frozenset("""\
xyo-native-simple-deopt-parity
xyo-nested-warp-args-osr-refusal
xyo-nested-warp-args-preserved-osr
xyo-nested-warp-argument-gc
xyo-nested-warp-argument-reads
xyo-nested-warp-argument-times
xyo-nested-warp-bounded-inner-deopt
xyo-nested-warp-bounded-inner-deopt-fast
xyo-nested-warp-branch-osr
xyo-nested-warp-deep-osr
xyo-nested-warp-direct-arguments-osr
xyo-nested-warp-dynamic-inner-deopt
xyo-nested-warp-dynamic-inner-deopt-active
xyo-nested-warp-dynamic-inner-deopt-active-fast
xyo-nested-warp-dynamic-inner-deopt-fast
xyo-nested-warp-fractional-boundary
xyo-nested-warp-fractional-boundary-fast
xyo-nested-warp-fractional-boundary-late
xyo-nested-warp-fractional-boundary-late-fast
xyo-nested-warp-inflight-body-edit
xyo-nested-warp-inflight-body-edit-fast
xyo-nested-warp-inflight-deopt
xyo-nested-warp-inner-yield-deopt
xyo-nested-warp-inner-yield-deopt-fast
xyo-nested-warp-parent-repeat-osr
xyo-nested-warp-quad-yield-deopt
xyo-nested-warp-quad-yield-deopt-fast
xyo-nested-warp-quad-yield-fractional
xyo-nested-warp-quad-yield-fractional-fast
xyo-nested-warp-silent-edit
xyo-nested-warp-source-proof
xyo-nested-warp-stack-osr
xyo-nested-warp-topology-failclosed
xyo-nested-warp-topology-failclosed-fast
xyo-nested-warp-triple-yield-deopt
xyo-nested-warp-triple-yield-deopt-fast
xyo-nested-warp-triple-yield-fractional
xyo-nested-warp-triple-yield-fractional-fast
""".split())


def selected_test(name: str) -> bool:
    return name == "xyo-native-simple-deopt-parity" or name.startswith(
        "xyo-nested-warp-"
    )


def audit(tests: list[dict], *, check_paths: bool = True) -> list[str]:
    subset = [test for test in tests if selected_test(test["name"])]
    found = [test["name"] for test in subset]
    found_set = set(found)
    errors = []
    for name in sorted(EXPECTED - found_set):
        errors.append(f"Missing CTest: {name}")
    for name in sorted(found_set - EXPECTED):
        errors.append(f"Unreviewed CTest: {name}")
    if len(found) != len(found_set):
        errors.append("Duplicate native OSR CTest name")

    if check_paths:
        for test in subset:
            command = test.get("command", [])
            if not command:
                errors.append(f"No command: {test['name']}")
                continue
            # --out destinations are intentionally not created yet. The
            # interpreter, referenced fixture scripts and native binaries
            # must already be materialized by the selective CI build.
            for index, argument in enumerate(command):
                path = Path(argument)
                required = (index == 0 or path.suffix in {".py", ".sb3"}
                            or path.name == "xyo"
                            or path.name.endswith("-smoke"))
                if required and path.is_absolute() and not path.is_file():
                    errors.append(f"Missing dependency: {test['name']}: {path}")
    return errors


def self_test() -> None:
    case = [{"name": name, "command": [sys.executable, __file__]}
            for name in EXPECTED]
    assert not audit(case)
    assert any("Missing CTest" in e for e in audit(case[:-1]))
    assert any("Unreviewed CTest" in e for e in audit(
        case + [{"name": "xyo-nested-warp-unexpected", "command": [sys.executable]}]
    ))
    assert any("Missing dependency" in e for e in audit(
        [{**case[0], "command": ["/definitely/missing/osr-tool"]}] + case[1:]
    ))
    assert any("Duplicate" in e for e in audit(case + case[:1]))
    print("PASS: native OSR CI inventory-guard self tests")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build-osr"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0

    result = subprocess.run(
        ["ctest", "--test-dir", str(args.build_dir), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    tests = json.loads(result.stdout)["tests"]
    errors = audit(tests)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    regular = len(tests) - len(EXPECTED)
    print(f"PASS: {len(EXPECTED)} native OSR tests registered; "
          f"{regular} other CTests; all native commands/fixtures exist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
