#!/usr/bin/env python3
"""Frozen full 47-workload suite, paired projection guard ON/OFF, official scoring."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--project', type=Path, default=ROOT / 'scratch-bench/.cache/build/performance.sb3')
    ap.add_argument('--blocks', type=int, default=4)
    ap.add_argument('--cpu', type=int, default=16)
    args = ap.parse_args()
    assert args.blocks > 0 and args.cpu in os.sched_getaffinity(0)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary, project = out / 'runtime', out / 'performance.sb3'
    shutil.copy2(args.binary.resolve(), binary)
    shutil.copy2(args.project.resolve(), project)
    shutil.copy2(__file__, out / Path(__file__).name)
    hashes = {str(path): digest(path) for path in (binary, project)}
    (out / 'manifest.json').write_text(json.dumps(dict(cpu=args.cpu, blocks=args.blocks,
        hashes=hashes, mode='hotspot', fps=30, danger=False,
        scoring='scratch-bench/site/shared/scoring.ts scoreRuntime, version 2',
        note='Unmodified full SB3; project-reported timing, deterministic checksums validated'), indent=2) + '\n')
    rows, expected = [], {}
    for block in range(args.blocks):
        for enabled in ((0, 1) if block % 2 == 0 else (1, 0)):
            env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
            env.update(XYO_JIT_INTEGER_NUM_CLASSIFY='0', XYO_JIT_COERCION_PROJECTION=str(enabled))
            p = subprocess.run(['taskset', '-c', str(args.cpu), str(binary), 'run', str(project),
                                '--jit', 'hotspot', '--fps', '30', '--print-list', 'output'],
                               env=env, cwd=ROOT, capture_output=True, text=True, timeout=180)
            (out / f'b{block}-{enabled}.stdout').write_text(p.stdout)
            (out / f'b{block}-{enabled}.stderr').write_text(p.stderr)
            assert p.returncode == 0 and 'DONE' in p.stdout, (p.returncode, p.stderr[-1200:])
            data = {}
            for line in p.stdout.splitlines():
                fields = [part.strip() for part in line.split('|')]
                if len(fields) != 6 or fields[0] == 'id':
                    continue
                name, rate, checksum = fields[0], float(fields[3]), fields[5]
                assert math.isfinite(rate) and rate > 0 and name not in data, fields
                data[name] = dict(rate=rate, checksum=checksum)
                if not name.startswith('sched_'):
                    if name in expected:
                        assert expected[name] == checksum, (name, checksum, expected[name])
                    else:
                        expected[name] = checksum
            assert len(data) == 47 and len(expected) == 39, (len(data), len(expected))
            rows.append(dict(block=block, enabled=enabled, data=data))
            (out / 'runs.json').write_text(json.dumps(rows, indent=2) + '\n')
            print('PASS block', block, 'projection', enabled, '47 rates, 39 checksums', flush=True)
    medians = {str(enabled): {name: statistics.median(row['data'][name]['rate'] for row in rows
                        if row['enabled'] == enabled) for name in rows[0]['data']} for enabled in (0, 1)}
    medians_file = out / 'medians.json'
    medians_file.write_text(json.dumps(medians, indent=2) + '\n')
    script = '''import {scoreRuntime} from './scratch-bench/site/shared/scoring.ts';
const data=await Bun.file(process.argv.at(-1)).json();
console.log(JSON.stringify(Object.fromEntries(Object.entries(data).map(([key,rates])=>[key,scoreRuntime(rates)]))));'''
    scoring = subprocess.run(['bun', '--eval', script, str(medians_file)], cwd=ROOT,
                              capture_output=True, text=True)
    assert scoring.returncode == 0, scoring.stderr
    scores = json.loads(scoring.stdout)
    summary = dict(scores=scores, total_ratio=scores['1']['total'] / scores['0']['total'],
                   item_ratios={name: medians['1'][name] / medians['0'][name] for name in medians['0']})
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    assert hashes == {path: digest(Path(path)) for path in hashes}, 'frozen inputs changed'
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
