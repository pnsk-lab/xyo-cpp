#!/usr/bin/env python3
"""Paired, CPU-pinned trials on unchanged source-derived ScratchBench projects."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[3]
PROFILES = {'base': (0, 0), 'classify': (1, 0), 'projection': (0, 1), 'both': (1, 1),
            'projected_base': (0, 1, 0), 'reporter_facts': (0, 1, 1)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--projects', type=Path, default=ROOT / 'board/prof-005/isolated')
    ap.add_argument('--names', default='list_read,list_replace,algo_matmul')
    ap.add_argument('--profiles', default='base,projection')
    ap.add_argument('--blocks', type=int, default=8)
    ap.add_argument('--baseline', default='base')
    ap.add_argument('--cpu', type=int, default=16)
    args = ap.parse_args()
    assert args.cpu in os.sched_getaffinity(0), 'CPU not available'
    assert args.blocks > 0
    names, profiles = args.names.split(','), args.profiles.split(',')
    assert set(profiles) <= set(PROFILES) and len(set(profiles)) == len(profiles)
    assert args.baseline in profiles
    if set(profiles) & {'classify', 'both'}:
        assert b'XYO_JIT_INTEGER_NUM_CLASSIFY' in args.binary.read_bytes(), 'discarded trial requires archived binary'
    if 'reporter_facts' in profiles:
        assert b'XYO_JIT_NUMERIC_REPORTER_FACTS' in args.binary.read_bytes(), 'reporter trial not in binary'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = out / 'runtime'
    shutil.copy2(args.binary.resolve(), binary)
    inputs = {}
    for name in names:
        inputs[name] = out / f'{name}.sb3'
        shutil.copy2(args.projects.resolve() / f'{name}.sb3', inputs[name])
    driver = out / Path(__file__).name
    shutil.copy2(__file__, driver)
    hashes = {str(path): digest(path) for path in [binary, driver, *inputs.values()]}
    (out / 'manifest.json').write_text(json.dumps(dict(cpu=args.cpu, blocks=args.blocks,
        profiles=profiles, baseline=args.baseline, hashes=hashes, seed=2037, jit='eager', fps=30,
        note='Original source-derived isolated projects; no kernel/timing/score changes'), indent=2) + '\n')
    rows, expected = [], {}
    def execute(name, profile, diagnostic=False):
        flags = PROFILES[profile]
        env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
        env.update(XYO_GPU='0', XYO_PAR='0', XYO_PARFOR='0',
            XYO_JIT_INTEGER_NUM_CLASSIFY=str(flags[0]), XYO_JIT_COERCION_PROJECTION=str(flags[1]),
            XYO_JIT_NUMERIC_REPORTER_FACTS=str(flags[2] if len(flags) > 2 else 0))
        if diagnostic:
            env['XYO_JIT_DUMP2'] = '1'
        p = subprocess.run(['taskset', '-c', str(args.cpu), str(binary), 'run',
                            str(inputs[name]), '--jit', 'eager', '--fps', '30',
                            '--print-list', 'output'], env=env, cwd=ROOT,
                           capture_output=True, text=True, timeout=120)
        assert p.returncode == 0, (name, profile, p.returncode, p.stderr[-1000:])
        assert 'DONE' in p.stdout, (name, profile, p.stdout[-1000:])
        records = [line.split('|') for line in p.stdout.splitlines() if line.startswith(name + ' |')]
        assert len(records) == 1, p.stdout[-1500:]
        fields = [x.strip() for x in records[0]]
        rate, checksum = float(fields[3]), fields[5]
        assert math.isfinite(rate) and rate > 0, fields
        if name in expected:
            assert checksum == expected[name], (name, profile, checksum, expected[name])
        else:
            expected[name] = checksum
        return dict(rate=rate, checksum=checksum, stdout=p.stdout), p.stderr
    rng = random.Random(2037)
    order = [(name, profile) for name in names for profile in profiles]
    for block in range(args.blocks):
        if block % 2 == 0:
            rng.shuffle(order)
        else:
            order.reverse()
        for name, profile in order:
            result, _ = execute(name, profile)
            rows.append(dict(block=block, name=name, profile=profile, **result))
            (out / 'runs.json').write_text(json.dumps(rows, indent=2) + '\n')
            print(block, name, profile, result['rate'], 'checksum', result['checksum'], flush=True)
    summary = {}
    for name in names:
        base = {row['block']: row['rate'] for row in rows if row['name'] == name and row['profile'] == args.baseline}
        for profile in profiles:
            selected = [row for row in rows if row['name'] == name and row['profile'] == profile]
            ratios = [row['rate'] / base[row['block']] for row in selected]
            summary[f'{name}/{profile}'] = dict(median_rate=statistics.median(row['rate'] for row in selected),
                paired_geomean=math.exp(statistics.mean(math.log(x) for x in ratios)),
                wins=sum(x > 1 for x in ratios), ratios=ratios)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    for profile in profiles:
        _, ir = execute('list_read', profile, True) if 'list_read' in names else execute(names[0], profile, True)
        (out / f'ir-{profile}.ll').write_text(ir)
        if 'list_read' in names:
            assert 'mixed.list.bounded' in ir, ('typed list path not selected', profile)
    assert hashes == {path: digest(Path(path)) for path in hashes}, 'frozen input changed'
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
