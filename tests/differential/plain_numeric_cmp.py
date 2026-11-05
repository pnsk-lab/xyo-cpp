#!/usr/bin/env python3
"""Scratch comparison semantics across changing list element types.

Compares interpreter, eager, and HotSpot for the same SB3 while toggling the
runtime-tag-guarded LLVM fcmp path. Includes numeric and nonnumeric elements.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_if, S_repeat, S_replace, S_set,
    eq, gt, lt, num, op, text, var, make_project, write_sb3,
)


def item(idx):
    return op("data_itemoflist", {"LIST": "values"}, INDEX=idx)


def fixture():
    entries = [
        num(-1e308), num(-128), num(-1.5), num(-0.0), num(0),
        num(1e-300), num(0.5), num(1), num(12345), num(1e308),
        op("operator_divide", NUM1=num(0), NUM2=num(0)),
        op("operator_divide", NUM1=num(1), NUM2=num(0)),
        op("operator_divide", NUM1=num(-1), NUM2=num(0)),
        text(""), text(" "), text("\t"), text("0"), text("1"),
        text("001"), text("Infinity"), text("infinity"), text("NaN"),
        text("abc"), text("ABC"), text("日本語"), text("K"),
        text("__boolean_true__"), text("__boolean_false__"),
    ]
    n = len(entries)
    g = Gen([
        "a", "b", "i", "j", "gt", "lt", "direct_gt", "direct_lt",
        "iterations",
    ], ["values"])

    inner = [
        S_set("b", item(var("j"))),
        S_if(gt(var("a"), var("b")), [S_change("gt", num(1))]),
        S_if(lt(var("a"), var("b")), [S_change("lt", num(1))]),
        # Crucial: auto selection only applies when BOTH input reporters
        # are list items, not when an item was first stored in a variable.
        S_if(gt(item(var("i")), item(var("j"))),
             [S_change("direct_gt", num(1))]),
        S_if(lt(item(var("i")), item(var("j"))),
             [S_change("direct_lt", num(1))]),
        S_change("j", num(1)),
        S_change("iterations", num(1)),
    ]
    outer = [
        S_set("a", item(var("i"))),
        S_set("j", num(1)),
        S_repeat(num(n), inner),
        S_change("i", num(1)),
    ]
    g.define("compare-all", [], [
        S_repeat(num(3), [
            S_set("i", num(1)),
            S_repeat(num(n), outer),
            # Force a type transition in the same list used by the reporter.
            S_replace("values", num(1), text("changed")),
            S_replace("values", num(2), num(9)),
        ])
    ], warp=True)
    g.script([*(S_add("values", v) for v in entries), S_call("compare-all", [])])
    # The simple generator normally rejects Boolean reporters in a value
    # socket. Scratch supports the shape, so replace these two text placeholders
    # with actual Boolean reporters after the project AST is constructed.
    for bid, block in list(g.blocks.items()):
        if block["opcode"] != "data_addtolist":
            continue
        raw = block["inputs"].get("ITEM")
        if not isinstance(raw, list) or len(raw) < 2:
            continue
        if raw[1] == [10, "__boolean_true__"]:
            block["inputs"]["ITEM"] = g.emit_value(eq(num(1), num(1)), bid, True)
        elif raw[1] == [10, "__boolean_false__"]:
            block["inputs"]["ITEM"] = g.emit_value(eq(num(1), num(2)), bid, True)
    return make_project(g.vars.keys(), g.lists.keys(), g), n


def run(binary, sb3, jit, fast, ordered=True):
    assert fast in ("off", "auto", "all")
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json", "XYO_GPU": "0",
        "XYO_PAR": "0", "XYO_PARFOR": "0",
        "XYO_JIT_FP_ORDERED_CMP": "1" if ordered else "0",
    })
    if fast != "auto":
        env["XYO_JIT_PLAIN_NUMBER_CMP"] = "1" if fast == "all" else "0"
    p = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", jit, "--frames", "300"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=75,
    )
    if p.returncode:
        raise RuntimeError(f"jit={jit} fast={fast} rc={p.returncode} "
                           f"stderr={p.stderr[-1000:]} stdout={p.stdout[-1000:]}")
    try:
        x = p.stdout
        return json.loads(x[x.index("{"):x.rindex("}")+1])["Bench"]
    except Exception as e:
        raise RuntimeError(f"Invalid output: {p.stdout[-800:]}") from e


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    path = ROOT / "build/plain-numeric-cmp-parity.sb3"
    data, n = fixture()
    write_sb3(path, data)
    reference = run(binary, path, "off", "off")
    assert reference["iterations"] == str(3 * n * n), reference
    assert reference["direct_gt"] == reference["gt"], reference
    assert reference["direct_lt"] == reference["lt"], reference
    print("interpreter: PASS (gt=%s, lt=%s, iterations=%s)" %
          (reference["direct_gt"], reference["direct_lt"], reference["iterations"]),
          flush=True)
    for jit in ("eager", "hotspot"):
        for fast in ("off", "auto", "all"):
            for ordered in (False, True):
                output = run(binary, path, jit, fast, ordered)
                if output != reference:
                    raise AssertionError((jit, fast, ordered, output, reference))
                print(f"{jit} fast={fast} ordered={ordered}: PASS", flush=True)


if __name__ == "__main__":
    main()
