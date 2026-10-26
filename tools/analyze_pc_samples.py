#!/usr/bin/env python3
"""Summarize opt-in xyo-pen-bench main-thread PC samples (Linux x86-64)."""
import argparse
import collections
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def mappings(path):
    result = []
    for line in path.read_text().splitlines():
        fields = line.split(None, 5)
        lo, hi = (int(part, 16) for part in fields[0].split('-'))
        result.append((lo, hi, fields[1], int(fields[2], 16),
                       fields[5] if len(fields) == 6 else ''))
    return result


def symbolize(module, offsets):
    """Return innermost and outermost debug frames for each ELF virtual address."""
    if not shutil.which('addr2line') or not Path(module).is_file():
        return {}
    args = ['addr2line', '-a', '-f', '-C', '-i', '-e', module]
    process = subprocess.run(args, input=''.join(f'{x:#x}\n' for x in offsets),
                             text=True, capture_output=True)
    if process.returncode:
        print(f'WARNING addr2line {module}: {process.stderr.strip()}')
        return {}
    groups, current, frames = {}, None, []
    for line in process.stdout.splitlines():
        if re.fullmatch(r'0x[0-9a-fA-F]+', line):
            if current is not None:
                groups[current] = frames
            current, frames = int(line, 16), []
        else:
            frames.append(line)
    if current is not None:
        groups[current] = frames
    return {offset: {'inner': lines[0], 'inner_location': lines[1],
                     'outer': lines[-2], 'outer_location': lines[-1]}
            for offset, lines in groups.items() if len(lines) >= 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--executable', type=Path,
                        help='saved copy of the sampled benchmark (use before rebuilding it)')
    parser.add_argument('--executable-module', default='xyo-pen-bench',
                        help='original module basename or full path to replace for symbolization')
    args = parser.parse_args()
    def image_for(module):
        if args.executable and (module == args.executable_module or
                                Path(module).name == args.executable_module):
            return str(args.executable)
        return module
    raw = Path(str(args.prefix) + '.pcs').read_bytes()
    if len(raw) % 8:
        raise SystemExit('sample data is not a whole number of 64-bit PCs')
    pcs = collections.Counter(pc[0] for pc in struct.iter_unpack('<Q', raw))
    total = sum(pcs.values())
    if not total:
        raise SystemExit('no captured samples')
    meta = json.loads(Path(str(args.prefix) + '.json').read_text())
    maps = mappings(Path(str(args.prefix) + '.maps'))
    by_module, native_offsets = collections.Counter(), {}
    elf_types = {}
    for pc, count in pcs.items():
        match = next((m for m in maps if m[0] <= pc < m[1]), None)
        if match is None:
            by_module['unmapped'] += count
            continue
        lo, hi, permissions, offset, name = match
        if name.startswith('/'):
            module = name.removesuffix(' (deleted)')
            by_module[module] += count
            # ET_EXEC addresses are already ELF virtual addresses. Ordinary
            # ET_DYN/PIE mappings use map-start minus file-offset as load bias.
            # Anonymous code has no ELF/source relationship and is not symbolized.
            if module not in elf_types:
                try:
                    with open(image_for(module), 'rb') as image:
                        header = image.read(20)
                    byteorder = '<' if header[5] == 1 else '>'
                    elf_types[module] = struct.unpack_from(byteorder + 'H', header, 16)[0] if header[:4] == b'\x7fELF' else 0
                except (OSError, IndexError, struct.error):
                    elf_types[module] = 0
            address = pc if elf_types[module] == 2 else pc-lo+offset
            native_offsets.setdefault(module, collections.Counter())[address] += count
        else:
            label = name or ('anonymous executable (JIT candidate)' if 'x' in permissions else 'anonymous non-executable')
            by_module[label] += count
    functions, lines = collections.Counter(), collections.Counter()
    for module, offsets in native_offsets.items():
        symbols = symbolize(image_for(module), list(offsets))
        for offset, count in offsets.items():
            item = symbols.get(offset)
            if item:
                functions[(Path(module).name, item['outer'])] += count
                lines[(Path(module).name, item['inner'], item['inner_location'])] += count
            else:
                functions[(Path(module).name, 'unresolved')] += count
    print('METADATA', json.dumps(meta, sort_keys=True))
    print('CAPTURED', total, 'samples,', len(pcs), 'unique PCs')
    if meta.get('sample_count') is not None and meta['sample_count'] != total:
        raise SystemExit('metadata count disagrees with sample data')
    if meta.get('thread_cpu_ms') is not None and meta.get('period_us'):
        expected = meta['thread_cpu_ms'] * 1000 / meta['period_us']
        accounted = total + meta.get('overruns', 0) + meta.get('dropped', 0)
        ratio = accounted / expected if expected > 0 else 0
        print(f'EXPECTED about {expected:.1f} CPU-timer events; captured ratio {total/expected:.3f}; accounted ratio {ratio:.3f}')
        meta['coverage_accounted_ratio'] = ratio
        meta['coverage_ok'] = (0.9 <= ratio <= 1.1 and not meta.get('dropped', 0)
                               and meta.get('overruns', 0) <= expected * 0.05
                               and meta.get('pcs_write_ok', True)
                               and meta.get('maps_write_ok', True) and total >= 100)
        if not meta['coverage_ok']:
            print('WARNING: sample coverage is incomplete, heavily coalesced, or too small; do not treat percentages as representative.')
    def show(title, counter):
        print('\n' + title)
        for key, count in counter.most_common(args.top):
            label = ' | '.join(key) if isinstance(key, tuple) else key
            print(f'{count:7d} {100*count/total:6.2f}% {label}')
    show('MODULES', by_module)
    show('NATIVE OUTER FUNCTIONS (percent of all samples)', functions)
    show('NATIVE INNER SOURCE LOCATIONS', lines)
    if args.json:
        args.json.write_text(json.dumps(dict(metadata=meta, total_samples=total,
            modules=[dict(name=k,count=v,percent=100*v/total) for k,v in by_module.most_common()],
            functions=[dict(module=k[0],function=k[1],count=v,percent=100*v/total) for k,v in functions.most_common()],
            locations=[dict(module=k[0],function=k[1],location=k[2],count=v,percent=100*v/total) for k,v in lines.most_common()]), indent=2))


if __name__ == '__main__':
    main()
