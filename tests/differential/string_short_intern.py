#!/usr/bin/env python3
"""Verify dynamic short-string interning against generated Scratch SB3."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from string_storage_modes import fixture


def run(binary, sb3, storage, leaf, jit, intern, cli=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_PAR": "0", "XYO_GPU": "0",
                "XYO_STRING_STORAGE": storage, "XYO_STRING_LEAF": leaf,
                "XYO_STRING_SHORT_INTERN": "0" if cli else
                    ("1" if intern else "0")})
    cmd = [str(binary), "run", str(sb3), "--jit", jit, "--frames", "10"]
    if cli:
        cmd.append("--string-short-intern=on")
    p = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                       capture_output=True, timeout=60)
    if p.returncode:
        raise RuntimeError(
            f"intern={intern}/{leaf}/{storage}/{jit}: {p.stderr[-1800:]}")
    payload = p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1]
    return json.loads(payload)["Bench"]


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-short-intern-") as work:
        sb3 = Path(work) / "short-intern-parity.sb3"
        expected = fixture(sb3)
        cases = 0
        for leaf in ("utf16", "latin1"):
            for storage in ("rope", "flat", "append-log"):
                for jit in ("off", "eager", "hotspot"):
                    for intern in (False, True):
                        result = run(binary, sb3, storage, leaf, jit, intern)
                        actual = {k: result[k] for k in expected}
                        assert actual == expected, (leaf, storage, jit, intern,
                                                    actual, expected)
                        cases += 1
                result = run(binary, sb3, storage, leaf, "eager", True,
                             cli=True)
                actual = {k: result[k] for k in expected}
                assert actual == expected, (leaf, storage, "cli", actual)
                cases += 1
        print(f"short interning Scratch parity: {cases} combinations passed")


if __name__ == "__main__":
    main()
