#!/usr/bin/env python3
"""Emscripten HotSpot: generated WASM runs and matches Scratch interpreter."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench" / "algos"))
from gen_algo_bench import (Gen, S_set, S_change, S_repeat, S_until,
                            S_if, S_ifelse, S_call, S_add, S_del,
                            add, div, mul, mod, lt, gt, eq, op, num, var, text,
                            length, make_project, write_sb3)

ROOT = Path(__file__).resolve().parents[2]

def run(node, loader, runtime, sb3, mode, initial=None, allow_frame_limit=False):
    env = dict(os.environ)
    env.update(
        XYO_DUMPVARS="json", XYO_HOT_INVOCATIONS="1",
        XYO_JIT_VERBOSE="1", XYO_PAR="0", XYO_JIT_WARP_CLOCK="0",
        XYO_EXIT_ON_DONE="1",
    )
    if initial:
        env.update(initial)
    result = subprocess.run(
        [node, str(loader), str(runtime), "run", str(sb3),
         "--frames", "20", "--jit", mode],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=60)
    reached_frame_limit = (allow_frame_limit and result.returncode == 2
                           and "frame limit reached" in result.stdout)
    if result.returncode != 0 and not reached_frame_limit:
        raise AssertionError(
            f"{mode} exited {result.returncode}: {result.stdout[-1000:]}"
            f"\n{result.stderr[-1500:]}")
    start = result.stdout.find("{")
    if start < 0:
        raise AssertionError(f"missing variable dump: {result.stdout}")
    values = json.JSONDecoder().raw_decode(result.stdout[start:])[0]
    if "Bench" not in values:
        raise AssertionError(f"missing Bench variables: {result.stdout}")
    return json.dumps(values["Bench"], sort_keys=True), result.stdout + result.stderr


def run_native(binary, sb3, mode, allow_frame_limit=False):
    """Same Scratch fixture executed through the actual native LLVM JIT."""
    env = dict(os.environ, XYO_DUMPVARS="json", XYO_HOT_INVOCATIONS="1",
               XYO_JIT_VERBOSE="1", XYO_PAR="0", XYO_JIT_WARP_CLOCK="0",
               XYO_EXIT_ON_DONE="1", XYO_JIT_ASYNC="0")
    proc = subprocess.run(
        [str(binary.resolve()), "run", str(sb3), "--frames", "20",
         "--jit", mode],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
    frame_exit = (allow_frame_limit and proc.returncode == 2 and
                  "frame limit reached" in proc.stdout)
    if proc.returncode and not frame_exit:
        raise AssertionError(
            f"LLVM {mode} exited {proc.returncode}: {proc.stderr[-1100:]}")
    start = proc.stdout.find("{")
    if start < 0:
        raise AssertionError(f"missing native variables: {proc.stdout}")
    variables = json.JSONDecoder().raw_decode(proc.stdout[start:])[0]
    return json.dumps(variables["Bench"], sort_keys=True)


def fixture(directory, kind):
    gen = Gen(["x", "y"], ["items"] if kind == "numeric_list" else [])
    if kind == "simple":
        gen.script([
            S_set("x", num(7)),
            S_set("y", mul(add(var("x"), num(5)), num(3))),
            S_set("x", add(var("x"), num(1))),
        ])
    elif kind == "string":
        gen.script([S_set("y", add(var("x"), num(5)))])
    elif kind == "conditionals":
        gen.script([
            S_set("x", num(4)),
            S_set("y", num(0)),
            S_if(lt(var("x"), num(5)), [S_set("y", add(var("y"), num(10)))]),
            S_ifelse(gt(var("x"), num(5)),
                     [S_set("y", num(-1))],
                     [S_set("y", add(var("y"), num(2)))]),
            S_if(eq(var("x"), num(4)), [S_set("y", add(var("y"), num(3)))]),
            S_if(op("operator_and",
                    OPERAND1=lt(var("x"), num(6)),
                    OPERAND2=op("operator_not", OPERAND=gt(var("x"), num(7)))),
                 [S_set("y", add(var("y"), num(20)))]),
            S_if(op("operator_or", OPERAND1=gt(var("x"), num(8)),
                    OPERAND2=eq(var("x"), num(4))),
                 [S_set("y", add(var("y"), num(40)))]),
        ])
    elif kind == "nan_compare":
        gen.script([
            S_set("x", num(11)),
            S_if(lt(div(num(0), num(0)), num(1)),
                 [S_set("x", num(111))]),
            S_set("y", num(92)),
        ])
    elif kind == "repeat":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(8), [S_set("x", add(var("x"), num(1)))]),
            S_set("y", var("x")),
        ])
    elif kind in ("mathops", "mathops_edge", "mathops_unknown", "mathops_loop"):
        math = lambda name, x: op("operator_mathop", {"OPERATOR": name}, NUM=x)
        if kind == "mathops":
            steps = [S_set("y", num(0))]
            for name, value in [
                ("abs", -12.5), ("floor", -2.8), ("ceiling", -2.8),
                ("sqrt", 13), ("sin", 37), ("cos", 42), ("tan", 47),
                ("asin", 0.4), ("acos", 0.4), ("atan", 6),
                ("ln", 4), ("log", 10), ("e ^", -2), ("10 ^", 2)]:
                steps.extend([
                    S_set("x", num(value)),
                    S_change("y", math(name, var("x"))),
                ])
            gen.script(steps)
        elif kind == "mathops_edge":
            gen.script([
                S_set("x", num(-1)),
                S_set("y", math("sqrt", var("x"))),
                S_set("x", math("tan", num(90))),
            ])
        elif kind == "mathops_unknown":
            gen.script([
                S_set("x", num(12)),
                S_set("y", math("not-a-valid-math-menu", var("x"))),
            ])
        else:
            gen.script([
                S_set("x", num(11)),
                S_repeat(num(4), [
                    S_change("x", math("sin", var("x"))),
                    S_set("y", mod(var("x"), num(3))),
                ]),
                S_set("y", math("cos", var("x"))),
            ])
    elif kind == "repeat_if":
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(3)),
            S_repeat(var("y"), [
                S_if(lt(var("x"), num(2)), [S_change("x", num(2))]),
                S_ifelse(eq(var("x"), num(2)),
                         [S_change("x", num(3))],
                         [S_change("x", num(1))]),
            ]),
            S_set("y", var("x")),
        ])
    elif kind == "repeat_zero":
        gen.script([
            S_set("x", num(50)),
            S_repeat(num(0), [S_set("x", num(1000))]),
            S_set("y", add(var("x"), num(2))),
        ])
    elif kind == "repeat_fraction":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(2.5), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
    elif kind == "repeat_negative":
        gen.script([
            S_set("x", num(10)),
            S_repeat(num(-0.5), [S_set("x", num(100))]),
            S_set("y", var("x")),
        ])
    elif kind == "repeat_nested":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(3), [
                S_repeat(num(2), [S_change("x", num(1))]),
            ]),
            S_set("y", var("x")),
        ])
    elif kind == "until":
        gen.script([
            S_set("x", num(0)),
            S_until(gt(var("x"), num(3)), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
    elif kind == "while":
        gen.script([
            S_set("x", num(0)),
            S_until(lt(var("x"), num(4)), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
        for block in gen.blocks.values():
            if block["opcode"] == "control_repeat_until":
                block["opcode"] = "control_while"
    elif kind == "until_dynamic":
        gen.script([
            S_set("x", num(0)),
            S_until(gt(var("x"), num(20)), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
        gen.script([
            S_repeat(num(2), [S_change("y", num(1))]),
            S_set("x", text("25")),
        ])
    elif kind == "while_dynamic":
        gen.script([
            S_set("x", num(0)),
            S_until(lt(var("x"), num(20)), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
        for block in gen.blocks.values():
            if block["opcode"] == "control_repeat_until":
                block["opcode"] = "control_while"
                break
        gen.script([
            S_repeat(num(2), [S_change("y", num(1))]),
            S_set("x", text("25")),
        ])
    elif kind == "numeric_list":
        gen.script([
            S_set("x", num(0)),
            S_add("items", num(10)),
            S_add("items", num(20)),
            S_set("y", length("items")),
            S_del("items", num(1)),
            S_set("x", length("items")),
        ])
    elif kind == "unsupported_string":
        gen.script([
            S_set("x", text("hello")),
            S_set("y", op("operator_join",
                           STRING1=var("x"), STRING2=text("!"))),
        ])
    elif kind == "unsupported_procedure":
        gen.define("work", [], [
            S_repeat(num(2), [S_change("x", num(1))])],
            warp=False)
        gen.script([
            S_set("x", num(0)),
            S_call("work"),
            S_set("y", var("x")),
        ])
    elif kind == "unsupported_wait":
        hat = gen.script([S_set("x", num(1))])
        end = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=end)
        gen.blocks[end]["next"] = wait
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        after = gen.new("data_setvariableto", parent=wait,
                        fields={"VARIABLE": gen.field("VARIABLE", "y")})
        gen.blocks[wait]["next"] = after
        gen.blocks[after]["inputs"]["VALUE"] = gen.emit_value(num(88), after)
    elif kind in ("broadcast", "broadcast_wait"):
        # Two Scratch event entry points: an interpreter broadcast sender
        # and a numeric WASM HotSpot receiver. Both must preserve event
        # dispatch and broadcast-and-wait join semantics.
        sender = gen.script([])
        event = gen.new("event_broadcastandwait" if kind == "broadcast_wait"
                        else "event_broadcast", parent=sender)
        gen.blocks[sender]["next"] = event
        gen.blocks[event]["inputs"]["BROADCAST_INPUT"] = [
            1, [11, "go", "b1"]]
        receiver = gen.script([
            S_repeat(num(3), [S_change("x", num(1))]),
            S_set("y", add(var("x"), num(10))),
        ])
        gen.blocks[receiver]["opcode"] = "event_whenbroadcastreceived"
        gen.blocks[receiver]["fields"]["BROADCAST_OPTION"] = ["go", "b1"]
    elif kind == "forever":
        gen.script([
            S_set("y", num(6)),
            S_repeat(num(500), [S_set("x", num(42))]),
        ])
        for block in gen.blocks.values():
            if block["opcode"] == "control_repeat":
                block["opcode"] = "control_forever"
                block["inputs"].pop("TIMES", None)
    elif kind in ("for_each", "for_each_zero", "for_each_fraction",
                  "for_each_dynamic", "for_each_type_bail", "for_each_mixed"):
        gen = Gen(["x", "y", "i"], [])
        count = num(0) if kind == "for_each_zero" else (
            num(2.5) if kind == "for_each_fraction" else
            var("y") if kind == "for_each_dynamic" else num(5))
        steps = [
            S_set("x", num(0)),
            S_set("y", num(4)),
            S_repeat(count, [S_change("x", var("i"))]),
            S_set("y", var("i")),
        ]
        if kind == "for_each_mixed":
            steps += [
                S_repeat(num(2), [S_change("x", num(100))]),
                S_set("y", add(var("x"), var("i"))),
            ]
        gen.script(steps)
        for block in gen.blocks.values():
            if block["opcode"] == "control_repeat":
                block["opcode"] = "control_for_each"
                block["fields"]["VARIABLE"] = gen.field("VARIABLE", "i")
                block["inputs"]["VALUE"] = block["inputs"].pop("TIMES")
                break
        if kind == "for_each_dynamic":
            gen.script([
                S_repeat(num(2), [S_change("y", num(1))]),
                S_set("y", num(2)),
            ])
        if kind == "for_each_type_bail":
            gen.script([
                S_repeat(num(2), [S_change("y", num(1))]),
                S_set("i", text("bad-iteration-index")),
            ])
    elif kind == "multiple_repeat":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(2), [S_change("x", num(1))]),
            S_change("x", num(3)),
            S_repeat(num(4), [S_change("x", num(2))]),
            S_set("y", var("x")),
        ])
    elif kind == "multiple_zero":
        gen.script([
            S_set("x", num(7)),
            S_repeat(num(0), [S_set("x", num(999))]),
            S_change("x", num(2)),
            S_repeat(num(1), [S_change("x", num(3))]),
            S_set("y", var("x")),
        ])
    elif kind == "multiple_mixed":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(2), [S_change("x", num(1))]),
            S_change("x", num(1)),
            S_until(gt(var("x"), num(5)), [S_change("x", num(1))]),
            S_set("y", var("x")),
            S_repeat(num(2), [S_change("x", num(1))]),
            S_set("y", add(var("x"), var("y"))),
        ])
    elif kind == "multiple_modulo":
        # Regression: moving slot IDs before lowering the loop body once
        # made env.h's fmod scratch area alias the 'y' Scratch variable.
        gen = Gen(["x", "y", "z"], [])
        gen.script([
            S_set("x", num(3)),
            S_set("y", num(10)),
            S_set("z", num(0)),
            S_repeat(num(2), [S_set("z", mod(var("y"), var("x")))]),
            S_repeat(num(2), [S_set("z", mod(var("z"), var("x")))]),
            S_set("y", add(var("y"), num(5))),
        ])
    elif kind == "fractional_modulo":
        gen = Gen(["x", "y", "z"], [])
        gen.script([
            S_set("x", num(-10)),
            S_set("y", num(-0.02)),
            S_repeat(num(2), [S_set("z", mod(var("x"), var("y")))]),
            S_set("x", var("z")),
        ])
    elif kind == "multiple_mid_bail":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(2), [S_change("x", num(1))]),
            S_set("y", num(100)),
            S_repeat(num(8), [S_change("x", num(1))]),
            S_set("y", var("x")),
        ])
        gen.script([
            S_repeat(num(4), [S_change("y", num(1))]),
            S_set("y", text("uncompiled-guard")),
            S_set("x", num(0)),
        ])
    elif kind == "repeat_mid_bail":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(12), [S_change("x", num(1))]),
            S_set("y", num(77)),
        ])
        # Concurrent interpreter script sets a *boolean* value after the
        # WASM loop has committed some iterations. Heap strings are now
        # supported by the transactional host numeric helper, so using
        # "invalid-number" here no longer exercises the type guard.
        gen.script([
            S_repeat(num(2), [S_change("y", num(1))]),
            S_set("y", text("uncompiled-guard")),
            S_set("x", num(0)),
        ])
    elif kind == "repeat_suffix_bail":
        gen.script([
            S_set("x", num(0)),
            S_repeat(num(8), [S_change("x", num(1))]),
            S_set("y", add(var("x"), num(5))),
        ])
        gen.script([
            S_repeat(num(7), [S_change("y", num(1))]),
            S_set("y", text("uncompiled-guard")),
            S_set("x", num(0)),
        ])
    if kind in ("repeat_mid_bail", "repeat_suffix_bail",
                "multiple_mid_bail"):
        # The generator normally rejects Boolean reporters in numeric value
        # sockets. Construct this valid Scratch VM input deliberately: a
        # Boolean false is a true nonnumeric Variable type, unlike strings
        # which the new WASM host helper can now parse without deoptimizing.
        candidate = [
            (bid, b) for bid, b in gen.blocks.items()
            if b["opcode"] == "data_setvariableto"
            and b["fields"].get("VARIABLE", [None, None])[1] == "v_x"
            and b["inputs"].get("VALUE") == [1, [4, "0"]]
        ]
        assert candidate, (kind, candidate)
        # The last x assignment belongs to the second concurrent script.
        bid, b = candidate[-1]
        b["inputs"]["VALUE"] = gen.emit_value(
            eq(num(1), num(2)), bid, bool_slot=True)
    project = make_project(list(gen.vars.keys()), list(gen.lists.keys()), gen)
    if kind in ("broadcast", "broadcast_wait"):
        project["targets"][0]["broadcasts"]["b1"] = "go"
    if kind == "string":
        project["targets"][1]["variables"]["v_x"][1] = "7"
    file = directory / f"{kind}.sb3"
    write_sb3(str(file), project)
    return file

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("--node", default="node")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--llvm", type=Path,
                        help="Compare each fixture against the native LLVM JIT")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hot-wasm-", dir=args.out) as tmp:
        out = Path(tmp)
        for kind in ("simple", "conditionals", "mathops", "mathops_edge",
                     "mathops_unknown", "mathops_loop", "string", "nan_compare",
                      "repeat", "repeat_if", "repeat_zero",
                     "repeat_fraction", "repeat_negative", "repeat_nested",
                     "repeat_mid_bail", "repeat_suffix_bail",
                       "multiple_repeat", "multiple_zero", "multiple_mixed",
                       "multiple_mid_bail", "multiple_modulo",
                      "for_each", "for_each_zero", "for_each_fraction",
                      "for_each_dynamic", "for_each_type_bail", "for_each_mixed",
                      "fractional_modulo",
                      "until", "while", "until_dynamic", "while_dynamic",
                      "forever", "broadcast", "broadcast_wait",
                     "numeric_list", "unsupported_string",
                     "unsupported_procedure", "unsupported_wait"):
            project = fixture(out, kind)
            interp, _ = run(args.node, ROOT / "web/xyo-node.mjs",
                            args.runtime.resolve(), project, "off",
                            allow_frame_limit=kind == "forever")
            hot, log = run(args.node, ROOT / "web/xyo-node.mjs",
                           args.runtime.resolve(), project, "hotspot",
                           allow_frame_limit=kind == "forever")
            if hot != interp:
                raise AssertionError(
                    f"{kind} mismatch:\ninterp={interp}\nhotspot={hot}")
            if args.llvm:
                llvm = run_native(args.llvm, project, "hotspot",
                                  allow_frame_limit=kind == "forever")
                if llvm != interp:
                    raise AssertionError(
                        f"{kind} native LLVM mismatch:\n"
                        f"interp={interp}\nllvm={llvm}")
            compiled = ("xyo-wasm-hot: compiled script=" in log or
                        "xyo-wasm-hot: hybrid script=" in log)
            committed = "xyo-wasm-hot: committed" in log
            if kind in ("simple", "conditionals", "mathops", "mathops_edge",
                        "mathops_unknown", "mathops_loop", "broadcast",
                        "broadcast_wait", "numeric_list") and not (
                        compiled and committed):
                raise AssertionError(f"{kind}: script never executed in HotSpot WASM")
            if kind == "string" and (not compiled or committed):
                raise AssertionError(
                    "string coercion guard must still reject invalid values")
            if kind == "nan_compare" and not (compiled and committed):
                raise AssertionError(
                    "tagged Scratch comparison must compile without NaN bailout")
            # An unsupported *block* now executes in the real interpreter,
            # without preventing adjacent numeric statements from being
            # compiled by the hybrid region scheduler.
            hybrid = "xyo-wasm-hot: hybrid script=" in log
            hybrid_required = kind in (
                "repeat_nested", "unsupported_procedure",
                "unsupported_wait")
            if hybrid_required and not (hybrid and compiled and committed):
                raise AssertionError(
                    f"{kind}: hybrid must compile and preserve "
                    "interpreter-only nested effects")
            if kind == "unsupported_string" and not (compiled and committed):
                raise AssertionError(
                    "string join must compile and commit with typed Value ABI")
            if (kind.startswith("repeat") and kind != "repeat_nested" or
                kind.startswith("multiple_") or kind.startswith("for_each") or kind == "fractional_modulo" or
                kind in ("until", "while", "until_dynamic",
                         "while_dynamic", "forever")) and not (
                compiled and committed):
                raise AssertionError(f"{kind}: loop never executed in HotSpot WASM")
            if kind == "repeat_mid_bail" and "resume interp repeat remaining=" not in log:
                raise AssertionError("mid-loop bailout did not restore repeat counter")
            # Exact tagged booleans now compile without a mandatory deopt.
            # If deoptimization occurs, it must still identify loop 1.
            if kind == "multiple_mid_bail" and (
                "resume interp repeat remaining=" in log and
                "loop=1" not in log):
                raise AssertionError("second loop bailout restored wrong counter")
            # The induction variable is overwritten with a numeric index
            # before each iteration; an unrelated string mutation need not
            # cause a bailout once the tagged ABI is supported.
            if kind in ("multiple_repeat", "multiple_zero", "multiple_mixed") and (
                "loop=1" not in log):
                raise AssertionError(f"{kind}: second loop never resumed in WASM")
            if kind == "repeat_suffix_bail" and "resume interp suffix remaining=" not in log:
                raise AssertionError("suffix bailout did not resume at the suffix")
            if kind in ("repeat", "repeat_if", "until", "while", "forever") and (
                "phase=1" not in log):
                raise AssertionError("loop did not run across scheduler checkpoints")
            if kind in ("until_dynamic", "while_dynamic") and (
                "resume interp repeat remaining=" not in log):
                raise AssertionError(f"{kind}: type bailout did not resume at loop")
            label = "WASM" if committed else "transactional interpreter fallback"
            print(f"PASS {kind}: {label} parity" +
                  (" + LLVM" if args.llvm else ""))
    # Test the optional synchronous Binaryen.js host optimizer interface.
    # The callback is intentionally identity-only: this verifies dispatch
    # and no-subprocess execution, not Binaryen's optimization quality.
    host_js = """
import {createRequire} from 'node:module';
import {resolve} from 'node:path';
const createXyo = createRequire(import.meta.url)(resolve(process.argv[1]));
const memory = new WebAssembly.Memory({initial:256,maximum:32768});
let calls = 0;
await createXyo({
  wasmMemory: memory,
  arguments: ['run', process.argv[2], '--jit', 'hotspot', '--frames', '10'],
  xyoOptimizeWasm: bytes => { calls++; return bytes; },
});
if (calls !== 1) throw Error('optimizer hook was not invoked once: ' + calls);
console.error('optimizer_hook_calls=' + calls);
"""
    env = dict(os.environ, XYO_DUMPVARS="json",
               XYO_HOT_INVOCATIONS="1", XYO_JIT_VERBOSE="1",
               XYO_WASM_HOT_EMITTER="binaryen", XYO_EXIT_ON_DONE="1")
    with tempfile.TemporaryDirectory(prefix="hot-wasm-hook-", dir=args.out) as tmp:
        project = fixture(Path(tmp), "simple")
        result = subprocess.run(
            [args.node, "--input-type=module", "-e", host_js,
             str(args.runtime.resolve()), str(project.resolve())],
            cwd=ROOT, env=env, text=True, capture_output=True, timeout=60)
        if result.returncode or "optimizer_hook_calls=1" not in result.stderr or (
            "emitter=binaryen-host" not in result.stderr):
            raise AssertionError(result.stdout[-900:] + result.stderr[-1400:])
        print("PASS host Binaryen callback routing (identity smoke)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
