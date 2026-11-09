#!/usr/bin/env python3
"""Check the generated host import ABI with a real WebAssembly engine.

The imported helper here is a deterministic JS mock of env.h. Native C++
tests separately verify the exact Scratch UTF-16 conversion implementation.
"""
from __future__ import annotations
import pathlib
import shutil
import subprocess
import sys

JS = r"""
import fs from 'node:fs';
const root = process.argv[1];
const original = 0x7ff9000000001234n;
for (const [filename, expectedStatus, expectedNumber, expectedCalls, deny] of [
  ['string-standalone.wasm', 1, null, 0, false],
  ['string-host.wasm', 0, 47.5, 1, false],
  ['string-host.wasm', 1, null, 1, true],
]) {
  const bytes = fs.readFileSync(root + '/' + filename);
  if (!WebAssembly.validate(bytes)) throw Error('invalid binary: '+filename);
  const memory = new WebAssembly.Memory({initial:256});
  const view = new DataView(memory.buffer);
  const base = 64;
  view.setBigUint64(base, original, true);
  let calls = 0;
  const imports = {
    env: {
      memory,
      h(id, addr, extra) {
        calls++;
        if (id !== 5 || extra !== 0 ||
            view.getBigUint64(addr, true) !== original)
          throw Error('bad host ABI');
        if (deny) return -1;
        view.setFloat64(addr, 42.5, true);
        return 0;
      }
    }
  };
  const {instance} = await WebAssembly.instantiate(bytes, imports);
  const status = instance.exports.f(base);
  if (status !== expectedStatus || calls !== expectedCalls)
    throw Error(filename + ': status='+status+' calls='+calls);
  if (expectedNumber === null) {
    if (view.getBigUint64(base, true) !== original)
      throw Error('bailout mutated input');
  } else if (view.getFloat64(base, true) !== expectedNumber) {
    throw Error('wrong converted variable');
  }
}
"""

def main() -> None:
    executable = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(sys.argv[2]).resolve()
    node = shutil.which("node")
    if not node:
        print("SKIP: Node.js unavailable")
        return
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(executable), str(root)], check=True, timeout=40)
    subprocess.run([node, "--input-type=module", "-e", JS, str(root)],
                   check=True, timeout=40)
    print("PASS: env.h string conversion ABI + standalone bailout")

if __name__ == "__main__":
    main()
