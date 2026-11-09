#!/usr/bin/env python3
"""Validate Latin-1 leaves against UTF-16 across Scratch join and JIT modes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from string_storage_modes import fixture


def run(binary, sb3, mode, leaf, jit, cli=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_PAR": "0",
                "XYO_GPU": "0", "XYO_STRING_STORAGE": mode,
                "XYO_STRING_LEAF": "utf16" if cli else leaf})
    cmd = [str(binary), "run", str(sb3), "--jit", jit, "--frames", "10"]
    if cli:
        cmd += [f"--string-leaf={leaf}"]
    p = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                       capture_output=True, timeout=60)
    if p.returncode:
        raise RuntimeError(f"{leaf}/{mode}/{jit}: {p.stderr[-1800:]}")
    payload = p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1]
    return json.loads(payload)["Bench"]


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-string-leaf-") as work:
        sb3 = Path(work) / "latin1-parity.sb3"
        expected = fixture(sb3)
        for leaf in ("utf16", "latin1"):
            for mode in ("rope", "flat", "append-log"):
                for jit in ("off", "eager", "hotspot"):
                    output = run(binary, sb3, mode, leaf, jit)
                    actual = {k: output[k] for k in expected}
                    assert actual == expected, (leaf, mode, jit, actual)
                    print(f"PASS {leaf} {mode} {jit}", flush=True)
                if leaf == "latin1":
                    output = run(binary, sb3, mode, leaf, "eager", cli=True)
                    actual = {k: output[k] for k in expected}
                    assert actual == expected, ("CLI", mode, actual)
                    print(f"PASS {leaf} {mode} CLI override", flush=True)


if __name__ == "__main__":
    main()
