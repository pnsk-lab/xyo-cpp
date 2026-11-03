#!/usr/bin/env python3
"""Differential test of generic warp modulo==0 against TurboWarp."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("scratchbench_probe", HERE / "scratchbench_probe.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

VALUES = [
    "0", "1", "2", "3", "-1", "-2", "0.5", "-1.5", "2147483647",
    "2147483648", "0 / 0", "1 / 0", '"0"', '"abc"', '" "',
]



def main():
    # Start with the ordinary isolated ScratchBench project, then use its
    # standard runner and logging harness. Leave vendored sources untouched.
    mod.build("primes")
    src = mod.OUT / "source" / "mod_edges"
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(mod.OUT / "source" / "primes", src)
    file = src / "Stage" / "algos.fractch"
    text = file.read_text()
    start = text.index("def @k_algo_primes() warp {")
    end = text.index("def @suite_algos()", start)

    body = [
        "def @k_algo_primes() warp {",
        "  b_n = 0;",
        "  b_t0 = daysSince2000();",
        "  until (daysSince2000() - b_t0) * 86400000 >= b_budget {",
        "    k_acc = 0;",
    ]
    for i, lhs in enumerate(VALUES):
        body.append(f"    k_i = {lhs};")
        for j, rhs in enumerate(VALUES):
            body.append(f"    k_j = {rhs};")
            body.append(f"    if k_i % k_j == 0 {{ k_acc += {1 + 31 * i + j}; }}")
    body.extend([
        "    b_n += 1;",
        "  }",
        "  b_t1 = daysSince2000();",
        "  b_chk = k_acc;",
        "}",
        "",
    ])
    file.write_text(text[:start] + "\n".join(body) + "\n" + text[end:])
    dest = mod.OUT / "projects" / "mod_edges.sb3"
    subprocess.run([str(mod.FRACTCH), "check", str(src)], cwd=mod.SB,
                   check=True, capture_output=True)
    subprocess.run([str(mod.FRACTCH), "to", str(dest), "from", str(src)],
                   cwd=mod.SB, check=True, capture_output=True)

    envs = [
        ("xyo", {}, "xyo"),
        ("turbowarp", {}, "tw"),
    ]
    results = {}
    for name, overrides, mode in envs:
        cmd = ([str(mod.XYO), "run", str(dest), "--print-list", "output",
                "--fps", "30"] if mode == "xyo" else
               ["node", str(mod.SB / "runtimes/node-vm.mjs"),
                "turbowarp", str(dest)])
        env = dict(os.environ)
        env.update(overrides)
        env["XYO_JIT_MODE"] = "eager"
        env["BENCH_COMPILER"] = "1"
        p = subprocess.run(cmd, cwd=mod.SB, env=env, capture_output=True,
                           text=True, timeout=180)
        print(name, "exit", p.returncode, "tail", p.stdout[-500:],
              "stderr", p.stderr[-500:], flush=True)
        assert p.returncode == 0, name
        row = [line for line in p.stdout.splitlines()
               if "algo_primes |" in line]
        assert row and "DONE" in p.stdout, name
        results[name] = row[-1].split(" | ")[-1].strip()
    print("Checksums", results, "all_match=", len(set(results.values())) == 1)
    assert len(set(results.values())) == 1


if __name__ == "__main__":
    main()
