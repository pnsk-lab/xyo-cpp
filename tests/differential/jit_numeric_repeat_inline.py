#!/usr/bin/env python3
"""Parity/lowering tests for opt-in numeric-repeat zeroarg warp-helper expansion.

Usage: python3 tests/differential/jit_numeric_repeat_inline.py --binary PATH --out NEW_DIR
Generates small SB3 fixtures with the algo-bench builder, not benchmark timings.
Covers pure helpers/current initialized frames, not exhaustive procedure ABI.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_if, S_repeat, S_set, add, arg, div,
    eq, make_project, mod, mul, num, op, text, var, write_sb3,
)


class FixtureGen(Gen):
    def emit_stmt(self, statement, parent=None):
        if statement[0] == "wait":
            bid = self.new("control_wait", parent=parent)
            self.blocks[bid]["inputs"]["DURATION"] = self.emit_value(num(0), bid)
            return bid
        return super().emit_stmt(statement, parent)


def fixture(count, source, warp, shape):
    g = FixtureGen(["src", "a", "b", "x"], ["probe", "checks", "effects"])
    update = S_set("x", mod(add(mul(var("x"), num(17)), num(23)), num(65521)))
    helper = [S_set("a", var("src")), S_change("a", num(0.5)), update]
    helper_warp, parameters, call_args = True, [], []
    if shape == "copy_inside":
        helper = [S_set("a", var("src")), S_set("b", var("a")), update]
    elif shape == "nonwarp_helper":
        helper_warp = False
    elif shape in ("parameterized", "parameter_unused"):
        parameters = ["value"]
        if shape == "parameterized":
            helper = [S_set("a", add(arg("value"), num(1))), update]
            call_args = [num(3)]
        # Also reject an unused declared parameter with no supplied inputs.
    elif shape == "list_write":
        helper = [S_add("effects", var("x")), update]
    elif shape == "condition":
        helper = [S_if(eq(var("x"), num(1)), [S_change("a", num(1))]), update]
    elif shape == "yield":
        helper = [("wait",), update]
    elif shape == "nested_call":
        g.define("leaf", [], [S_change("a", num(1))], warp=True)
        helper = [S_call("leaf"), update]
    elif shape in ("statements20", "statements21"):
        helper = [S_change("a", num(0.5))] * int(shape[len("statements"):])
    g.define("helper", parameters, helper, warp=helper_warp)
    g.define("empty", [], [], warp=True)
    body = [S_call("helper", call_args)]
    if shape == "empty_call":
        body = [S_call("empty"), S_call("helper"), S_call("empty")]
    elif shape == "multiple_calls":
        # Writes on both sides expose stale or reordered helper carriers.
        body = [S_call("helper"), S_set("b", var("a")),
                S_call("helper"), S_change("a", num(0.25))]
    elif shape in ("visits64", "visits65"):
        # Empty calls consume visits without adding numeric statements.
        visits = int(shape[len("visits"):])
        body = [S_call("empty")] * (visits - 1) + [S_change("a", num(0.5))]
    g.define("kernel", [], [S_repeat(num(count), body)], warp=warp)
    g.script([
        S_set("src", source), S_set("a", source), S_set("b", source),
        S_set("x", add(num(0), num(1))), S_call("kernel"),
        S_add("checks", var("src")), S_add("checks", var("a")),
        S_add("checks", var("b")), S_add("checks", var("x")),
        S_add("checks", div(num(1), var("a"))),
        S_add("checks", div(num(1), var("b"))),
    ] + [statement for name in ("src", "a", "b") for statement in (
        # listcontents observes string-vs-number tags; reciprocal observes -0.
        S_add("probe", var(name)), S_add("probe", text("Z")),
        S_add("checks", op("data_listcontents", {"LIST": "probe"})),
    )])
    return make_project(g.vars, g.lists, g)


def run(binary, sb3, mode, enabled, out, label, timeout, dump=False):
    # Do not reuse the copy test's runner, which clears the INLINE environment.
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
                "XYO_PARFOR": "0", "XYO_JIT_NUMERIC_REPEAT_SSA": "1",
                "XYO_JIT_NUMERIC_REPEAT_COPY": "1",
                "XYO_JIT_NUMERIC_REPEAT_INT": "0",
                "XYO_JIT_NUMERIC_REPEAT_FLOAT": "0"})
    if enabled is not None:
        env["XYO_JIT_NUMERIC_REPEAT_INLINE"] = str(enabled)
    if dump:
        env.update({"XYO_JIT_DUMP": "1", "XYO_JIT_DUMP2": "1"})
    command = [str(binary), "run", str(sb3), "--jit", mode, "--frames", "100000"]
    record = {"command": command, "mode": mode, "inline": enabled}
    try:
        p = subprocess.run(command, cwd=ROOT, env=env, text=True,
                           capture_output=True, timeout=timeout)
        stdout, stderr = p.stdout, p.stderr
        record["returncode"] = p.returncode
    except subprocess.TimeoutExpired as error:
        def decoded(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else (value or "")
        stdout, stderr = decoded(error.stdout), decoded(error.stderr)
        record["error"] = f"timeout after {timeout}s"
    (out / f"{label}.stdout").write_text(stdout)
    (out / f"{label}.stderr").write_text(stderr)
    try:
        if "error" in record or record["returncode"]:
            raise RuntimeError(record.get("error", f"exit {record.get('returncode')}"))
        data = json.loads(stdout[stdout.index("{"):stdout.rindex("}") + 1])
        record["result"] = data["Bench"]
    except (ValueError, KeyError, RuntimeError) as error:
        record["error"] = str(error)
    (out / f"{label}.json").write_text(json.dumps(record, indent=2))
    if "error" in record:
        raise RuntimeError(f"{label}: {record['error']}")
    return record["result"], stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--timeout", type=float, default=60, help="seconds per process")
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        ap.error("--binary must be an existing executable")
    if not args.timeout > 0:
        ap.error("--timeout must be positive")
    out.mkdir(parents=True, exist_ok=False)
    rows = []
    summary = {"binary": str(binary), "cases": rows, "status": "running"}

    def check(label, count, source, warp, shape, lowering=None, default=False):
        sb3 = out / f"{label}.sb3"
        write_sb3(sb3, fixture(count, source, warp, shape))
        row = {"case": label, "count": count, "warp": warp, "shape": shape,
               "status": "running"}
        rows.append(row)
        expected, _ = run(binary, sb3, "off", 0, out, label + "-interp", args.timeout)
        row["expected"] = expected
        for enabled in ((0, 1, None) if default else (0, 1)):
            actual, ir = run(binary, sb3, "eager", enabled, out,
                             label + f"-jit{enabled}", args.timeout,
                             dump=lowering is not None)
            if actual != expected:
                raise RuntimeError(f"{label} inline={enabled}: {actual} != {expected}")
            if lowering is not None:
                want = lowering and enabled == 1
                found = "rep.numeric.fast" in ir
                if found != want:
                    raise RuntimeError(f"{label} inline={enabled}: rep.numeric.fast={found}, expected {want}")
        row["status"] = "pass"
        if lowering is not None:
            row["inline_on_lowering"] = lowering

    try:
        sources = [add(num(0), num(7)), div(num(1), num(2)), mul(num(-1), num(0)),
                   text("7"), text("0017"), text("abc"), text(""), text("NaN"),
                   text("Infinity"), div(num(0), num(0))]
        for shape in ("helper_body", "empty_call", "multiple_calls", "copy_inside"):
            for warp in (True, False):
                for index, source in enumerate(sources):
                    for count in (0, 1, 63, 64, 65, 129):
                        label = f"{shape}-w{int(warp)}-s{index}-n{count}"
                        check(label, count, source, warp, shape)
        numeric = add(num(0), num(7))
        # Exactly one repeat per fixture: no nested eligible loop contributes an
        # unrelated marker. INT/FLOAT off isolate boxed rep.numeric.fast IR.
        for shape in ("helper_body", "empty_call", "multiple_calls", "copy_inside",
                      "statements20", "visits64"):
            check("lowering-" + shape, 129, numeric, True, shape,
                  lowering=True, default=(shape == "helper_body"))
        for shape in ("nonwarp_helper", "parameterized", "parameter_unused",
                      "list_write", "condition", "yield", "nested_call",
                      "statements21", "visits65"):
            check("rejected-" + shape, 65, numeric, True, shape, lowering=False)
        # Omit recursion: a rejected optimizer must not run an infinite workload.
        summary["status"] = "pass"
    except Exception as error:
        summary["status"] = "fail"
        summary["error"] = str(error)
        raise
    finally:
        (out / "results.json").write_text(json.dumps(summary, indent=2))
    print(f"PASS {len(rows)} fixtures / {len(rows) * 2 + 1} eager comparisons; "
          "INLINE default OFF, explicit ON/OFF lowering and rejection verified")


if __name__ == "__main__":
    main()
