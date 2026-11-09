#!/usr/bin/env python3
"""Execute identical executable-CFG graphs with LLVM lli and Node WebAssembly."""
from __future__ import annotations
import pathlib
import json
import shutil
import subprocess
import sys

JS = r"""
import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
if (!WebAssembly.validate(bytes)) throw Error('invalid wasm');
const {instance} = await WebAssembly.instantiate(bytes);
const actual = instance.exports.main();
const expected = BigInt(process.argv[2]);
if (actual !== expected) throw Error(`WASM: ${actual} != ${expected}`);
"""

JS_EXTERNAL = r"""
import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
if (!WebAssembly.validate(bytes)) throw Error('invalid external f64 WASM');
const spec = JSON.parse(process.argv[2]);
const viewBits = new DataView(new ArrayBuffer(8));
function bits(number) {
  if (number === 'NaN') return 0x7ff8000000000000n;
  viewBits.setFloat64(0, number, true);
  return viewBits.getBigUint64(0, true);
}
const memory = new WebAssembly.Memory({initial:1});
const view = new DataView(memory.buffer);
const base = 64;
for (let i=0; i<spec.input.length; i++)
  view.setBigUint64(base+8*i,bits(spec.input[i]),true);
const {instance} = await WebAssembly.instantiate(bytes,{env:{memory}});
const result = BigInt.asUintN(64, instance.exports.run(base));
const expected = bits(spec.result);
if (result !== expected)
  throw Error('return mismatch: '+result+' != '+expected);
for (let i=0; i<spec.expected.length; i++) {
  const actual = view.getBigUint64(base+8*i,true);
  if (actual !== bits(spec.expected[i]))
    throw Error('slot '+i+' mismatch: '+actual+' != '+bits(spec.expected[i]));
}
"""

JS_RESUMABLE = r"""
import fs from 'node:fs';
const bytes = fs.readFileSync(process.argv[1]);
if (!WebAssembly.validate(bytes)) throw Error('invalid resumable wasm');
const spec = JSON.parse(process.argv[2]);
const memory = new WebAssembly.Memory({initial:2});
const view = new DataView(memory.buffer);
const base = 64, frame = 4096;
const buf = new DataView(new ArrayBuffer(8));
function bits(value) {
  if (value === 'NaN') return 0x7ff8000000000000n;
  buf.setFloat64(0, value, true);
  return buf.getBigUint64(0, true);
}
for (let i = 0; i < spec.input.length; i++)
  view.setBigUint64(base + 8*i, bits(spec.input[i]), true);
const {instance} = await WebAssembly.instantiate(bytes,{env:{memory}});
const snapshots = spec.snapshots;
for (let step = 0; step < snapshots.length; step++) {
  const returned = instance.exports.run(base, frame);
  if (returned !== BigInt(step + 1 === snapshots.length ? 0 : 1))
    throw Error('incorrect slice status '+returned+' step '+step);
  for (let i = 0; i < spec.input.length; i++) {
    const expected = bits(snapshots[step][i]);
    const actual = view.getBigUint64(base + 8*i, true);
    if (actual !== expected)
      throw Error('snapshot step='+step+' slot='+i+
                  ' actual='+actual+' expected='+expected);
  }
  const pc = view.getBigUint64(frame, true);
  if (step + 1 === snapshots.length) {
    if (pc !== 0xffffffffffffffffn)
      throw Error('completed without DONE PC');
  } else if (pc === 0xffffffffffffffffn) {
    throw Error('yielded with completed PC');
  }
}
const finalExpected = spec.resultRaw !== undefined
  ? BigInt(spec.resultRaw) : bits(spec.result);
if (view.getBigUint64(frame + 8, true) !== finalExpected)
  throw Error('bad final result');
"""

def main() -> int:
    binary, directory = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    lli, node = shutil.which("lli"), shutil.which("node")
    opt, wasm_opt = shutil.which("opt"), shutil.which("wasm-opt")
    if not lli or not node:
        print("SKIP: lli or node unavailable")
        return 0
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(binary.resolve()), str(directory.resolve())],
                   check=True, timeout=30)
    for name, expected in (("loop", 55), ("diamond", 42), ("diamond-false", 99), ("phi-swap", 20), ("same-target", 7), ("var-loop", 17), ("var-true", 27), ("var-false", 91), ("var-two", 35), ("var-zero", 0), ("scratch-i64", 24), ("scratch-sub", 10), ("scratch-shadow", 24)):
        llvm = subprocess.run([lli, "-entry-function=main",
                               str(directory / f"{name}.ll")], timeout=30)
        assert llvm.returncode == expected, (name, llvm.returncode, expected)
        subprocess.run([node, "--input-type=module", "-e", JS,
                        str(directory / f"{name}.wasm"), str(expected)],
                       check=True, timeout=30)
        if opt:
            optimized_ir = directory / f"{name}.opt.ll"
            subprocess.run([opt, "-S", "-passes=default<O2>",
                            str(directory / f"{name}.ll"),
                            "-o", str(optimized_ir)],
                           check=True, timeout=30)
            optimized = subprocess.run(
                [lli, "-entry-function=main", str(optimized_ir)], timeout=30)
            assert optimized.returncode == expected, (
                name, "LLVM O2", optimized.returncode, expected)
        if wasm_opt:
            optimized_wasm = directory / f"{name}.opt.wasm"
            subprocess.run([wasm_opt, "-O2",
                            str(directory / f"{name}.wasm"),
                            "-o", str(optimized_wasm)],
                           check=True, timeout=30)
            subprocess.run([node, "--input-type=module", "-e", JS,
                            str(optimized_wasm), str(expected)],
                           check=True, timeout=30)
        print("PASS", name, "LLVM", expected, "WASM", expected)
    # These modules use caller-staged raw Scratch Values (not private i64
    # locals). Native LLVM validates the same slot writes in its main wrapper.
    external = {
        "scratch-f64": {"input": [6.0, -2.0],
                        "expected": [10.75, 12.5], "result": 12.5},
        "scratch-f64-change": {"input": [1.5, -2.0],
                               "expected": [12.25, 14.0], "result": 14.0},
        "scratch-f64-negzero": {"input": [1.0],
                                "expected": [-0.0], "result": -0.0},
        "scratch-f64-nan": {"input": [2.0, 3.0],
                            "expected": [0.0, "NaN"], "result": "NaN"},
        "scratch-f64-mul": {"input": [1.0, 0.0],
                            "expected": [15.0, 107.0], "result": 107.0},
        "scratch-if-true": {"input": [4.0, 0.0],
                            "expected": [22.0, 31.0], "result": 31.0},
        "scratch-if-false": {"input": [4.0, 0.0],
                             "expected": [17.0, 26.0], "result": 26.0},
        "scratch-if-empty": {"input": [4.0, 0.0],
                             "expected": [15.0, 24.0], "result": 24.0},
        "scratch-if-equal": {"input": [4.0, 0.0],
                             "expected": [22.0, 31.0], "result": 31.0},
        "scratch-if-gt": {"input": [4.0, 0.0],
                          "expected": [22.0, 31.0], "result": 31.0},
        "scratch-initial-nan": {"input": ["NaN", 3.0],
                                "expected": [15.0, 24.0], "result": 24.0},
        "scratch-nan-to-zero": {"input": [0.0, 3.0],
                                "expected": [0.0, 2.0], "result": 2.0},
        "scratch-repeat3": {"input": [0.0, 0.0],
                             "expected": [21.0, 30.0], "result": 30.0},
        "scratch-repeat0": {"input": [0.0, 0.0],
                             "expected": [15.0, 24.0], "result": 24.0},
        "scratch-repeat-if": {"input": [0.0, 0.0],
                              "expected": [26.0, 35.0], "result": 35.0},
        "scratch-repeat-nested": {"input": [0.0, 0.0],
                                  "expected": [27.0, 36.0], "result": 36.0},
    }
    for name, spec in external.items():
        llvm_file = directory / f"{name}.ll"
        wasm_file = directory / f"{name}.wasm"
        result = subprocess.run([lli, "-entry-function=main", str(llvm_file)],
                                timeout=30)
        assert result.returncode == 0, (name, "LLVM", result.returncode)
        subprocess.run([node, "--input-type=module", "-e", JS_EXTERNAL,
                        str(wasm_file), json.dumps(spec)],
                       check=True, timeout=30)
        if opt:
            optimized_ir = directory / f"{name}.opt.ll"
            subprocess.run([opt, "-S", "-passes=default<O2>",
                            str(llvm_file), "-o", str(optimized_ir)],
                           check=True, timeout=30)
            optimized = subprocess.run(
                [lli, "-entry-function=main", str(optimized_ir)],
                timeout=30)
            assert optimized.returncode == 0, (name, "LLVM O2")
        if wasm_opt:
            optimized_wasm = directory / f"{name}.opt.wasm"
            subprocess.run([wasm_opt, "-O2", str(wasm_file),
                            "-o", str(optimized_wasm)],
                           check=True, timeout=30)
            subprocess.run([node, "--input-type=module", "-e", JS_EXTERNAL,
                            str(optimized_wasm), json.dumps(spec)],
                           check=True, timeout=30)
        print("PASS", name, "LLVM", "WASM", "NaN-boxed f64 slots")
    resumable = {
        "scratch-resume3": {
            "input": [0.0, 0.0],
            "snapshots": [[17.0, 0.0], [19.0, 0.0],
                           [21.0, 0.0], [21.0, 30.0]],
            "result": 30.0
        },
        "scratch-resume0": {
            "input": [0.0, 0.0],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        },
        "scratch-resume-if": {
            "input": [0.0, 0.0],
            "snapshots": [[17.0, 0.0], [19.0, 0.0], [21.0, 0.0],
                           [26.0, 0.0], [26.0, 35.0]],
            "result": 35.0
        },
        "scratch-resume-nested": {
            "input": [0.0, 0.0],
            "snapshots": [[17.0, 0.0], [19.0, 0.0], [19.0, 0.0],
                           [21.0, 0.0], [23.0, 0.0], [23.0, 0.0],
                           [25.0, 0.0], [27.0, 0.0], [27.0, 0.0],
                           [27.0, 36.0]],
            "result": 36.0
        },
        "scratch-resume-phi": {
            "input": [],
            "snapshots": [[], [], [], []],
            "resultRaw": "20"
        },
        "scratch-resume-sequential": {
            "input": [0.0, 0.0],
            "snapshots": [[17.0, 0.0], [19.0, 0.0], [21.0, 0.0],
                           [26.0, 0.0], [31.0, 0.0], [31.0, 40.0]],
            "result": 40.0
        },
        "scratch-resume-dynamic": {
            "input": [0.0, 2.5],
            "snapshots": [[17.0, 2.5], [19.0, 2.5], [21.0, 2.5],
                           [21.0, 30.0]],
            "result": 30.0
        },
        "scratch-resume-dynamic-expr": {
            "input": [0.0, 1.0],
            "snapshots": [[17.0, 1.0], [19.0, 1.0], [21.0, 1.0],
                           [21.0, 30.0]],
            "result": 30.0
        },
        "scratch-resume-dynamic-negative": {
            "input": [0.0, -0.5],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        },
        "scratch-resume-literal-half": {
            "input": [0.0, 0.0],
            "snapshots": [[17.0, 0.0], [19.0, 0.0], [19.0, 28.0]],
            "result": 28.0
        },
        "scratch-resume-literal-negative": {
            "input": [0.0, 0.0],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        },
        "scratch-resume-dynamic-half": {
            "input": [0.0, 0.5],
            "snapshots": [[17.0, 0.5], [17.0, 26.0]],
            "result": 26.0
        },
        "scratch-resume-dynamic-nan": {
            "input": [0.0, "NaN"],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        },
        "scratch-resume-dynamic-below-half": {
            "input": [0.0, 0.49999999999999994],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        },
        "scratch-resume-literal-below-half": {
            "input": [0.0, 0.0],
            "snapshots": [[15.0, 24.0]],
            "result": 24.0
        }
    }
    for name, spec in resumable.items():
        llvm_file = directory / f"{name}.ll"
        wasm_file = directory / f"{name}.wasm"
        native = subprocess.run([lli, "-entry-function=main",
                                 str(llvm_file)], timeout=30)
        assert native.returncode == 0, (name, "LLVM resume", native.returncode)
        subprocess.run([node, "--input-type=module", "-e", JS_RESUMABLE,
                        str(wasm_file), json.dumps(spec)],
                       check=True, timeout=30)
        if opt:
            optimized_ir = directory / f"{name}.opt.ll"
            subprocess.run([opt, "-S", "-passes=default<O2>",
                            str(llvm_file), "-o", str(optimized_ir)],
                           check=True, timeout=30)
            optimized = subprocess.run([lli, "-entry-function=main",
                                        str(optimized_ir)], timeout=30)
            assert optimized.returncode == 0, (name, "LLVM O2 resume")
        if wasm_opt:
            optimized_wasm = directory / f"{name}.opt.wasm"
            subprocess.run([wasm_opt, "-O2", str(wasm_file),
                            "-o", str(optimized_wasm)],
                           check=True, timeout=30)
            subprocess.run([node, "--input-type=module", "-e", JS_RESUMABLE,
                            str(optimized_wasm), json.dumps(spec)],
                           check=True, timeout=30)
        print("PASS", name, "LLVM", "WASM", "checkpoint/resume")
    return 0

if __name__ == "__main__":
    sys.exit(main())
