#!/usr/bin/env python3
"""Readonly String arithmetic coercion: exact tags, guards and chunk boundaries.

Only arithmetic uses may coerce a readonly String. Exact scalar copies must
retain their original tags, signed zero and lexical spelling. No benchmarks
or project names are recognized by the optimizer.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_add, S_call, S_repeat, S_set, add, div,
                            join, make_project, mod, mul, num, op, text, var,
                            write_sb3)


def fixture(count, source, kind="arithmetic", warp=True, helper=False):
    g = Gen(["source", "x", "acc", "copy"], ["probe", "copyprobe", "checks"])
    body = [S_set("x", mod(add(mul(var("x"), var("source")), num(23)), num(65521))),
            S_set("acc", mod(add(var("acc"), var("x")), num(65521)))]
    if kind == "copy_before":
        body = [S_set("copy", var("source"))] + body
    elif kind == "copy_after":
        body += [S_set("copy", var("source"))]
    elif kind == "written":
        body = [S_set("source", add(var("source"), num(0)))] + body
    if helper:
        g.define("step", [], body, warp=True)
        body = [S_call("step")]
    g.define("kernel", [], [S_repeat(num(count), body)], warp=warp)
    g.script([S_set("source", source), S_set("x", add(num(0), num(1))),
              S_set("acc", add(num(0), num(0))), S_set("copy", add(num(0), num(0))),
              S_call("kernel"), S_add("probe", var("source")), S_add("probe", text("Z")),
              S_add("copyprobe", var("copy")), S_add("copyprobe", text("Z")),
              S_add("checks", var("x")), S_add("checks", var("acc")),
              S_add("checks", div(num(1), var("source"))),
              S_add("checks", op("data_listcontents", {"LIST": "probe"})),
              S_add("checks", op("data_listcontents", {"LIST": "copyprobe"}))])
    return make_project(g.vars, g.lists, g)


def run(binary, sb3, mode, enabled, out, label, dump=False, inline=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
                "XYO_JIT_NUMERIC_REPEAT_READONLY_STRING": str(enabled)})
    if inline:
        env["XYO_JIT_NUMERIC_REPEAT_INLINE"] = "1"
    if dump:
        env.update({"XYO_JIT_DUMP": "1", "XYO_JIT_DUMP2": "1",
                    "XYO_JIT_KEEP_VALUE_NAMES": "1"})
    p = subprocess.run([str(binary), "run", str(sb3), "--jit", mode, "--frames", "100000"],
                       env=env, cwd=ROOT, text=True, capture_output=True, timeout=60)
    (out / f"{label}.stdout").write_text(p.stdout)
    (out / f"{label}.stderr").write_text(p.stderr)
    if p.returncode:
        raise RuntimeError(f"{label}: exit {p.returncode}")
    values = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])["Bench"]
    return values["checks"], p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [text(s) for s in ("7", "0007", "+7", "7.0", "7e0", "0x7", " 7 ",
               "\u20037\u2003", "2097152", "2097153", "0", "-0", "-0.0", "-0e0",
               "-1", "0.5", "1e309", "Infinity", "-Infinity", "abc", "NaN", "", "\t", " ")]
    sources += [mul(num(-1), num(0)), div(num(1), num(2)), div(num(0), num(0)),
                div(num(1), num(0)), join(text("000000000"), text("000000007"))]
    rows = []
    for kind in ("arithmetic", "copy_before", "copy_after", "written"):
        for index, source in enumerate(sources):
            counts = (0, 1, 63, 64, 65, 129) if kind == "arithmetic" else (1, 65)
            for count in counts:
                label = f"{kind}-s{index}-n{count}"
                sb3 = out / f"{label}.sb3"
                write_sb3(sb3, fixture(count, source, kind))
                expected, _ = run(binary, sb3, "off", 0, out, label + "-interp")
                for enabled in (0, 1):
                    actual, _ = run(binary, sb3, "eager", enabled, out, label + f"-jit{enabled}")
                    if actual != expected:
                        raise RuntimeError(f"{label}: enabled={enabled}: {actual} != {expected}")
                rows.append({"case": label, "expected": expected})
    # Nonwarp loops must preserve every-iteration scheduling, not take chunks.
    for count in (1, 65, 129):
        label = f"nonwarp-{count}"
        sb3 = out / f"{label}.sb3"
        write_sb3(sb3, fixture(count, text("0007"), warp=False))
        expected, _ = run(binary, sb3, "off", 0, out, label + "-interp")
        for enabled in (0, 1):
            actual, _ = run(binary, sb3, "eager", enabled, out, label + f"-jit{enabled}")
            if actual != expected:
                raise RuntimeError(f"{label}: {actual} != {expected}")
        rows.append({"case": label, "expected": expected})
    # Interprocedural planning must include exact-copy uses and all writes;
    # otherwise a helper could hide a tag-observing use from the readonly gate.
    for kind in ("arithmetic", "copy_before", "copy_after", "written"):
        for index, source in enumerate((text("0007"), text("abc"), text("-0"), text("0.5"))):
            for count in (65, 129):
                label = f"combined-{kind}-s{index}-n{count}"
                sb3 = out / f"{label}.sb3"
                write_sb3(sb3, fixture(count, source, kind, helper=True))
                expected, _ = run(binary, sb3, "off", 0, out, label + "-interp")
                for enabled in (0, 1):
                    actual, _ = run(binary, sb3, "eager", enabled, out,
                                    label + f"-jit{enabled}", inline=True)
                    if actual != expected:
                        raise RuntimeError(f"{label}: {actual} != {expected}")
                rows.append({"case": label, "expected": expected})
    # Verify an arithmetic-only source gets the new entry guard, and sources
    # used for exact copies/written in the loop do not get that guard.
    for kind in ("arithmetic", "copy_before", "copy_after", "written"):
        sb3 = out / f"lowering-{kind}.sb3"
        write_sb3(sb3, fixture(129, text("0007"), kind))
        for enabled in (0, 1):
            _, ir = run(binary, sb3, "eager", enabled, out,
                        f"lowering-{kind}-{enabled}", dump=True)
            selected = "rep.readonly.accepted" in ir
            if selected != (enabled == 1 and kind == "arithmetic"):
                raise RuntimeError(f"unexpected readonly admission: {kind}, {enabled}")
    (out / "results.json").write_text(json.dumps(rows, indent=2))
    print(f"PASS {len(rows)} fixtures / {len(rows)*2} comparisons; readonly/copy guards verified")


if __name__ == "__main__":
    main()
