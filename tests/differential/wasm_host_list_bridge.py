#!/usr/bin/env python3
"""Exercise real generated WASM host numeric-list read calls with Node.

The C++ host binding/GC semantics are separately checked by the fixture
generator. A JS import mock validates argument order, status, and rollback.
"""
from __future__ import annotations
import pathlib
import shutil
import subprocess
import sys

JS = r"""
import fs from 'node:fs';
const dir = process.argv[1];
for (const [name, hostStatus, expected, calls] of [
  ['list-standalone.wasm', 0, 1, 0],
  ['list-host.wasm', 0, 0, 1],
  ['list-host.wasm', 1, 1, 1],
]) {
  const memory = new WebAssembly.Memory({initial:256});
  const view = new DataView(memory.buffer);
  view.setFloat64(64, 999.0, true);
  let count = 0;
  const bytes = fs.readFileSync(dir+'/'+name);
  if (!WebAssembly.validate(bytes)) throw Error('invalid WASM');
  const env = {memory, h(id, pointer, slot) {
    count++;
    // The current Value-preserving ABI loads an item through helper 23.
    // Numeric f64 is still bit-identical to an untagged number.
    if (id !== 23 || slot !== 0 || view.getFloat64(pointer,true) !== 1)
      throw Error('list helper ABI mismatch: '+id+','+pointer+','+slot);
    if (hostStatus) return -1;
    view.setFloat64(pointer, 37.25, true);
    return 0;
  }};
  const {instance} = await WebAssembly.instantiate(bytes, {env});
  const status = instance.exports.f(64);
  if (status !== expected || count !== calls)
    throw Error(name+': wrong status/calls '+status+'/'+count);
  const value = view.getFloat64(64,true);
  if (value !== (expected === 0 ? 37.25 : 999.0))
    throw Error(name+': unexpected write '+value);
}
// Test a complete sequence of speculative list writes and a read between
// them. The final list must be [99], and scalar output must be 11.
for (const failOnDelete of [false, true]) {
  const bytes = fs.readFileSync(dir+'/list-edits.wasm');
  if (!WebAssembly.validate(bytes)) throw Error('invalid write WASM');
  const memory = new WebAssembly.Memory({initial:256});
  const view = new DataView(memory.buffer);
  view.setFloat64(64, 777, true);
  const authoritative = [5];
  let staged = null;
  const calls = [];
  const env = {memory, h(op, ptr, binding) {
    if (binding !== 0) throw Error('incorrect binding');
    calls.push(op);
    const index = view.getFloat64(ptr,true);
    const value = view.getFloat64(ptr+8,true);
    if (failOnDelete && op === 9) return -1;
    if (staged === null) staged = [...authoritative];
    if (op === 7) staged.push(value);
    else if (op === 8) staged[index-1] = value;
    else if (op === 9) staged.splice(index-1,1);
    else if (op === 23) view.setFloat64(ptr,staged[index-1],true);
    else throw Error('unexpected op '+op);
    return 0;
  }};
  const {instance} = await WebAssembly.instantiate(bytes,{env});
  const result = instance.exports.f(64);
  if (result !== (failOnDelete ? 1 : 0))
    throw Error('wrong list-edit status '+result);
  if (calls.join(',') !== '7,23,8,9')
    throw Error('wrong list-edit order '+calls);
  if (authoritative.length !== 1 || authoritative[0] !== 5)
    throw Error('helper unexpectedly committed list');
  if (failOnDelete) {
    if (view.getFloat64(64,true) !== 11) {
      // WASM scalar slot is speculative; host must rollback after bailout.
      throw Error('unexpected staged scalar');
    }
    // Discard the shadow; the authoritative list remains [5].
  } else {
    if (view.getFloat64(64,true) !== 11 ||
        staged.length !== 1 || staged[0] !== 99)
      throw Error('wrong list result');
    authoritative.splice(0,authoritative.length,...staged);
    if (authoritative[0] !== 99) throw Error('commit failed');
  }
}
"""

def main() -> None:
    fixture = pathlib.Path(sys.argv[1]).resolve()
    directory = pathlib.Path(sys.argv[2]).resolve()
    node = shutil.which("node")
    if node is None:
        print("SKIP: Node.js not installed")
        return
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(fixture),str(directory)],check=True,timeout=30)
    subprocess.run([node,"--input-type=module","-e",JS,str(directory)],
                   check=True,timeout=30)
    print("PASS: WASM tagged-list import, standalone fallback, helper failure")

if __name__ == "__main__":
    main()
