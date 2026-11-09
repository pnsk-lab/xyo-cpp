#!/usr/bin/env python3
"""End-to-end verification of LLVM-free WASM output (Builtin and Binaryen).

Binaryen is optional in CI. When wasm-opt is available, its output is
executed in a real JS WebAssembly engine and compared with the builtin output.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench" / "algos"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_repeat, S_if, S_ifelse, add, mul, mod, num, var, op, lt, gt,
    make_project, write_sb3
)

JS_CHECK = r"""
import fs from 'node:fs';
const binary = fs.readFileSync(process.argv[1]);
const meta = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
if (meta.entry !== 'f' || meta.memory.module !== 'env' ||
    meta.memory.name !== 'memory' ||
    meta.slots.join(',') !== 'v_x,v_y' ||
    !WebAssembly.validate(binary)) process.exit(2);
const memory = new WebAssembly.Memory({initial: meta.memory.minPages});
const {instance} = await WebAssembly.instantiate(binary, {env: {memory}});
const status = instance.exports.f(64);
const slots = new Float64Array(memory.buffer, 64, 2);
if (status !== 0 || slots[0] !== 1000 || slots[1] !== 2000) process.exit(3);
console.log('PASS', meta.backend, binary.length, slots[0], slots[1]);
"""

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    node = shutil.which("node")
    if not node:
        print("SKIP wasm backend: Node.js unavailable")
        return 0
    env = os.environ.copy()
    tool = env.get("XYO_WASM_OPT") or shutil.which("wasm-opt")
    if tool:
        env["XYO_WASM_OPT"] = str(tool)
    native = env.get("XYO_TEST_BINARYEN_NATIVE") == "1"

    with tempfile.TemporaryDirectory(prefix="binaryen-e2e-", dir=args.out) as temp:
        directory = Path(temp)
        g = Gen(["x", "y"], [])
        top_id = g.script([
            S_set("x", num(0)),
            S_repeat(num(1000), [S_set("x", add(var("x"), num(1)))]),
            S_set("y", mul(var("x"), num(2))),
        ])
        sb3 = directory / "ok.sb3"
        write_sb3(str(sb3), make_project(["x", "y"], [], g))

        def compile_one(backend: str, extra: list[str] | None = None,
                        expected_status: int = 0,
                        engine: str | None = None) -> tuple[Path, subprocess.CompletedProcess[str]]:
            dest = directory / f"{backend}.wasm"
            cmd = [str(args.binary.resolve()), "wasm", str(sb3),
                   "-o", str(dest), "--backend", backend]
            if extra: cmd.extend(extra)
            cmd_env = {**env, "TMPDIR": str(directory)}
            if engine is not None:
                cmd_env["XYO_BINARYEN_ENGINE"] = engine
            result = subprocess.run(cmd, env=cmd_env,
                                    text=True, capture_output=True, timeout=45)
            if result.returncode != expected_status:
                raise AssertionError(f"{cmd} returned {result.returncode}: {result.stderr}")
            return dest, result

        def run_one(backend: str, engine: str | None = None) -> None:
            path, result = compile_one(
                backend, ["--script", top_id, "--opt", "1"], engine=engine)
            assert result.stdout.startswith("wasm:"), result
            meta = path.with_suffix(".wasm.json")
            output = subprocess.run(
                [node, "--input-type=module", "-e", JS_CHECK, str(path), str(meta)],
                text=True, capture_output=True, timeout=30)
            if output.returncode:
                raise AssertionError(output.stdout + output.stderr)
            print(output.stdout.strip())

        run_one("builtin")
        if tool or native:
            run_one("binaryen")
        else:
            print("SKIP Binaryen optimization: no library or wasm-opt installed")
        if native:
            run_one("binaryen", engine="library")
        if tool:
            run_one("binaryen", engine="external")

        # Multi-hat projects must not silently choose an arbitrary script.
        g.script([S_set("x", num(17))])
        write_sb3(str(sb3), make_project(["x", "y"], [], g))
        _, failed = compile_one("builtin", expected_status=1)
        assert "expected exactly one" in failed.stderr, failed.stderr

        # Explicit script selection remains deterministic.
        run_one("builtin")
        if tool or native:
            run_one("binaryen")

        _, failed = compile_one("builtin", ["--opt", "9"], expected_status=2)
        assert "--opt must be in 0..3" in failed.stderr

        if tool:
            # Tool failure must be reported, never passed off as optimized WASM.
            failed_env = dict(env, XYO_WASM_OPT="/missing/wasm-opt",
                              XYO_BINARYEN_ENGINE="external")
            probe = subprocess.run(
                [str(args.binary.resolve()), "wasm", str(sb3),
                 "-o", str(directory / "missing.wasm"),
                 "--script", top_id, "--backend", "binaryen"],
                env={**failed_env, "TMPDIR": str(directory)}, capture_output=True,
                text=True, timeout=30)
            assert probe.returncode != 0, probe
            assert not (directory / "missing.wasm").exists()
        # Standalone WASM has no env.h runtime math helper. A fractional
        # remainder is not exactly computable with n-trunc(n/m)*m, so the
        # generated function must return bailout status rather than silently
        # publishing an approximate number. HotSpot uses host fmod instead.
        guarded = Gen(["x", "y"], [])
        guarded_id = guarded.script([
            S_set("x", mod(num(-10), num(-0.02))),
            S_set("y", num(2)),
        ])
        write_sb3(str(sb3), make_project(["x", "y"], [], guarded))
        js_guard = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial: 256});
const {instance} = await WebAssembly.instantiate(bytes, {env: {memory}});
const state = instance.exports.f(64);
if (state !== 1) throw Error('fractional modulo should bail: ' + state);
"""
        for mod_backend in (["builtin", "binaryen"] if (tool or native)
                            else ["builtin"]):
            path, _ = compile_one(
                mod_backend, ["--script", guarded_id, "--opt", "2"])
            out = subprocess.run(
                [node, "--input-type=module", "-e", js_guard, str(path)],
                capture_output=True, text=True, timeout=30)
            assert out.returncode == 0, out.stdout + out.stderr
            print("PASS", mod_backend, "unsafe standalone modulo bailout")

        # A standalone WASM entry cannot yield between for-each iterations.
        # Reject that script instead of emitting a semantically wrong module;
        # only the HotSpot scheduler supports segmented for-each execution.
        sequential = Gen(["x", "y", "i"], [])
        seq_id = sequential.script([
            S_set("x", num(0)),
            S_repeat(num(3), [S_set("x", add(var("x"), var("i")))]),
        ])
        for block in sequential.blocks.values():
            if block["opcode"] == "control_repeat":
                block["opcode"] = "control_for_each"
                block["fields"]["VARIABLE"] = sequential.field("VARIABLE", "i")
                block["inputs"]["VALUE"] = block["inputs"].pop("TIMES")
                break
        write_sb3(str(sb3), make_project(["x", "y", "i"], [], sequential))
        for kind in (["builtin", "binaryen"] if (tool or native) else ["builtin"]):
            _, failure = compile_one(
                kind, ["--script", seq_id], expected_status=1)
            assert "WebAssembly lowering failed" in failure.stderr, failure.stderr
        print("PASS unsupported standalone for-each rejected")

        # Mathop uses native WASM ops for the simple four operations. The
        # transcendental ops require Scratch/V8-compatible runtime math:
        # standalone modules must bail rather than call an absent host ABI.
        math_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial:256});
const {instance} = await WebAssembly.instantiate(bytes, {env:{memory}});
const status = instance.exports.f(64);
const expected = Number(process.argv[2]);
if (status !== expected) throw Error('mathop status='+status+' expected='+expected);
if (status === 0) {
  const value = new DataView(memory.buffer).getFloat64(64, true);
  if (value !== 4) throw Error('sqrt(16) != 4: '+value);
}
"""
        for name, arg, expected in (("sqrt", 16, 0), ("sin", 30, 1)):
            math_gen = Gen(["x", "y"], [])
            math_id = math_gen.script([
                S_set("x", op("operator_mathop", {"OPERATOR":name}, NUM=num(arg))),
            ])
            write_sb3(str(sb3), make_project(["x", "y"], [], math_gen))
            for backend in (["builtin", "binaryen"] if (tool or native)
                            else ["builtin"]):
                path, _ = compile_one(backend, ["--script", math_id])
                proc = subprocess.run(
                    [node, "--input-type=module", "-e", math_js, str(path),
                     str(expected)],
                    capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, proc.stdout + proc.stderr
            print("PASS standalone mathop", name, "status", expected)

        # Cross-backend numeric compare: < and > share typed SSA after
        # Scratch has established that both values are numeric.
        compare_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const mem = new WebAssembly.Memory({initial: 256});
const {instance} = await WebAssembly.instantiate(bytes, {env: {memory: mem}});
const status = instance.exports.f(64);
const values = new Float64Array(mem.buffer, 64, 2);
if (status !== 0 || values[0] !== 1 || values[1] !== 0)
  throw Error('comparison result mismatch: '+status+' '+values);
"""
        compare_gen = Gen(["x", "y"], [])
        compare_id = compare_gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_if(lt(num(-3), num(4)), [S_set("x", num(1))]),
            S_ifelse(lt(num(-3), num(4)),
                     [S_set("x", num(1))],
                     [S_set("x", num(2))]),
            S_ifelse(gt(num(-3), num(4)),
                     [S_set("y", num(2))],
                     [S_set("y", num(0))]),
        ])
        write_sb3(str(sb3), make_project(["x", "y"], [], compare_gen))
        for kind in (["builtin", "binaryen"] if (tool or native)
                     else ["builtin"]):
            path, _ = compile_one(kind, ["--script", compare_id])
            result = subprocess.run(
                [node, "--input-type=module", "-e", compare_js, str(path)],
                capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            print("PASS", kind, "typed numeric comparison")

        # Backedge SSA: zero-count, nonempty, and nested repeat bodies must
        # preserve the original Scratch control-flow and induction values.
        loop_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial:256});
const {instance} = await WebAssembly.instantiate(bytes, {env:{memory}});
const status = instance.exports.f(64);
const slots = new Float64Array(memory.buffer, 64, 2);
if (status !== 0 || slots[0] !== 12 || slots[1] !== 99)
  throw Error('repeat backedge mismatch: '+status+' '+slots);
"""
        nested = Gen(["x", "y"], [])
        nested_id = nested.script([
            S_set("x", num(0)), S_set("y", num(99)),
            S_repeat(num(0), [S_set("y", num(0))]),
            S_repeat(num(3), [
                S_repeat(num(4), [S_set("x", add(var("x"), num(1)))])
            ]),
        ])
        write_sb3(str(sb3), make_project(["x", "y"], [], nested))
        for kind in (["builtin", "binaryen"] if (tool or native)
                     else ["builtin"]):
            path, _ = compile_one(kind, ["--script", nested_id])
            result = subprocess.run(
                [node, "--input-type=module", "-e", loop_js, str(path)],
                capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            print("PASS", kind, "nested repeat and zero-count backedge")

        # Scratch ToNumber accepts canonical numeric NaN (as zero) and
        # boolean true (as one). Unsupported heap-string conversion must
        # bail without publishing output in a standalone WASM module.
        numeric_guard = Gen(["x", "y"], [])
        numeric_id = numeric_guard.script([
            S_set("y", add(var("x"), num(1))),
        ])
        write_sb3(str(sb3), make_project(["x", "y"], [], numeric_guard))
        tag_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial:256});
const view = new DataView(memory.buffer);
const meta = JSON.parse(fs.readFileSync(process.argv[4], 'utf8'));
const x = meta.slots.indexOf('v_x');
const ySlot = meta.slots.indexOf('v_y');
if (x < 0 || ySlot < 0 || x === ySlot) throw Error('invalid slot metadata');
const {instance} = await WebAssembly.instantiate(bytes, {env:{memory}});
view.setBigUint64(64 + x * 8, BigInt(process.argv[2]), true);
view.setFloat64(64 + ySlot * 8, 123, true);
const status = instance.exports.f(64);
const expected = Number(process.argv[3]);
if (status !== expected) throw Error('guard status '+status+' != '+expected);
const y = view.getFloat64(64 + ySlot * 8, true);
if (y !== Number(process.argv[5]))
  throw Error('guard wrote output on failed assumption: '+y);
"""
        for backend in (["builtin", "binaryen"] if (tool or native)
                        else ["builtin"]):
            path, _ = compile_one(backend, ["--script", numeric_id])
            for tag, expected, expected_y in (
                ("0x7ff8000000000000", 0, 1), # canonical numeric NaN -> 0
                ("0x7ff8000000000002", 0, 2), # Scratch true -> 1
                ("0x7ff9000000001234", 1, 123), # heap string -> bailout
            ):
                result = subprocess.run(
                    [node, "--input-type=module", "-e", tag_js, str(path),
                     tag, str(expected), str(path.with_suffix(".wasm.json")),
                    str(expected_y)],
                    capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stdout + result.stderr
            print("PASS", backend, "numeric NaN/boolean coercion and heap-string bailout")

        # Boolean/empty-string/color immediate coercion is shared with the
        # native Scratch runtime. The value is injected as raw tagged bits;
        # only the body of ChangeVar needs to run.
        immediate = Gen(["x", "y"], [])
        immediate_id = immediate.script([S_change("x", num(5))])
        write_sb3(str(sb3), make_project(["x", "y"], [], immediate))
        immediate_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial:256});
const view = new DataView(memory.buffer);
const {instance} = await WebAssembly.instantiate(bytes,{env:{memory}});
view.setBigUint64(64, BigInt(process.argv[2]), true);
view.setFloat64(72, 222, true);
const status = instance.exports.f(64);
const expectedStatus = Number(process.argv[3]);
const expectedX = Number(process.argv[4]);
if (status !== expectedStatus ||
    (status === 0 && view.getFloat64(64,true) !== expectedX) ||
    view.getFloat64(72,true) !== 222)
  throw Error('immediate coercion: '+status+' '+view.getFloat64(64,true));
"""
        for backend in (["builtin", "binaryen"] if (tool or native)
                        else ["builtin"]):
            path, _ = compile_one(backend, ["--script", immediate_id])
            for tag, status, value in (
                ("0x7ff8000000000002", 0, 6), # true + 5
                ("0x7ff8000000000001", 0, 5), # false + 5
                ("0x7ff8000000000003", 0, 5), # empty string + 5
                ("0x7ffa000000abcdef", 0, 5), # RGB object + 5
                ("0x7ff9000000001234", 1, 0), # heap string -> bailout
            ):
                proc = subprocess.run(
                    [node, "--input-type=module", "-e", immediate_js,
                     str(path), tag, str(status), str(value)],
                    capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, proc.stdout + proc.stderr
            print("PASS", backend, "immediate ToNumber/change-variable")

        count_guard = Gen(["x", "y"], [])
        count_id = count_guard.script([
            S_repeat(num(2147483648), [S_set("x", num(1))]),
        ])
        write_sb3(str(sb3), make_project(["x", "y"], [], count_guard))
        count_js = """import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
const memory = new WebAssembly.Memory({initial:256});
const view = new DataView(memory.buffer);
const {instance} = await WebAssembly.instantiate(bytes, {env:{memory}});
view.setFloat64(64, 123, true);
const status = instance.exports.f(64);
if (status !== 1 || view.getFloat64(64, true) !== 123)
  throw Error('loop count guard failed: '+status);
"""
        for backend in (["builtin", "binaryen"] if (tool or native)
                        else ["builtin"]):
            path, _ = compile_one(backend, ["--script", count_id])
            result = subprocess.run(
                [node, "--input-type=module", "-e", count_js, str(path)],
                capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            print("PASS", backend, "repeat count guard")

        print("WASM backend smoke tests passed")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
