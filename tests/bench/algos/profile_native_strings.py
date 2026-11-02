#!/usr/bin/env python3
"""Resolve optional sample_native.cpp self PCs offline (no JIT attribution guesses)."""
import argparse
import bisect
import collections
import json
import pathlib
import re
import struct
import subprocess


def elf_base(path, start, offset):
    # ELF64 little endian PT_LOAD: maps offsets are rounded down to page size.
    with open(path, 'rb') as f:
        header = f.read(64)
        if header[:6] != b'\x7fELF\x02\x01':
            raise ValueError('requires little-endian ELF64')
        phoff = struct.unpack_from('<Q', header, 32)[0]
        entsize, count = struct.unpack_from('<HH', header, 54)
        for i in range(count):
            f.seek(phoff + i * entsize)
            p = struct.unpack('<IIQQQQQQ', f.read(56))
            if p[0] == 1 and (p[2] & ~4095) == offset:
                return start - (p[3] & ~4095)
    raise ValueError('no matching PT_LOAD')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('prefix')
    args = ap.parse_args()
    prefix = pathlib.Path(args.prefix)
    lines = pathlib.Path(str(prefix) + '.samples').read_text().splitlines()
    maps = []
    for line in pathlib.Path(str(prefix) + '.maps').read_text().splitlines():
        p = line.split(maxsplit=5)
        lo, hi = [int(x, 16) for x in p[0].split('-')]
        maps.append((lo, hi, int(p[2], 16), p[5] if len(p) > 5 else '', p[1]))
    cache = {}
    functions, modules, unresolved = collections.Counter(), collections.Counter(), collections.Counter()
    for line in lines[1:]:
        pc, n = line.split(); pc, n = int(pc, 16), int(n)
        mapping = next((m for m in maps if m[0] <= pc < m[1]), None)
        if mapping is None:
            unresolved['not in exit maps (possibly unmapped code; not necessarily JIT)'] += n
            continue
        lo, hi, offset, path, perms = mapping
        modules[path or '<anonymous>'] += n
        if not path.startswith('/') or path.endswith(' (deleted)'):
            unresolved[path or '<anonymous mapping; provenance unknown>'] += n
            continue
        try:
            if path not in cache:
                proc = subprocess.run(['nm', '-n', '-S', '-C', path], text=True, capture_output=True)
                dynamic = subprocess.run(['nm', '-D', '-n', '-S', '-C', path], text=True, capture_output=True)
                symbols = []
                for s in (proc.stdout + '\n' + dynamic.stdout).splitlines():
                    m = re.match(r'^([0-9a-fA-F]+) ([0-9a-fA-F]+) ([tTwW]) (.*)$', s)
                    if m:
                        symbols.append((int(m[1], 16), int(m[2], 16), m[4]))
                symbols.sort()
                cache[path] = (symbols, [s[0] for s in symbols], proc.returncode)
            symbols, starts, rc = cache[path]
            address = pc - elf_base(path, lo, offset)
            idx = bisect.bisect_right(starts, address) - 1
            if idx >= 0 and symbols[idx][0] <= address < sum(symbols[idx][:2]):
                functions[path + ': ' + symbols[idx][2]] += n
            else:
                unresolved[path + (' (nm failed/no symbols)' if rc else ' (no sized symbol covering PC)')] += n
        except (OSError, ValueError, struct.error) as e:
            unresolved[path + ': ' + str(e)] += n
    result = {'metadata': lines[0], 'recorded': sum(functions.values()) + sum(unresolved.values()),
              'functions': functions.most_common(), 'modules': modules.most_common(),
              'unresolved': unresolved.most_common(),
              'caveat': 'Self samples only, whole process lifetime including loader/compiler. Process CPU timer delivery/main-thread filtering can bias results. Exit maps cannot identify unmapped JIT PCs.'}
    pathlib.Path(str(prefix) + '.resolved.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
