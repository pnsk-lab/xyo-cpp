#!/usr/bin/env python3
"""Alternate OFF/ON flags on one frozen binary and unchanged scheduler kernels."""
import argparse
import hashlib
import json
import math
import os
import shutil
from pathlib import Path
import statistics
import subprocess
import zipfile

from bench_scheduler import scheduler_project


SCHEDULER_CASES = {'sched_looptick', 'sched_pingpong', 'sched_restart',
                   'sched_clonechurn', 'sched_threads', 'sched_fanout',
                   'sched_fanwait', 'sched_waituntil'}


def select_cases(source, destination, cases):
    """Keep kernel bodies intact; remove only unselected suite call sites."""
    with zipfile.ZipFile(source) as archive:
        project = json.loads(archive.read('project.json'))
        blocks = project['targets'][0]['blocks']
        skipped = {key for key, block in blocks.items()
                   if isinstance(block, dict) and block.get('opcode') == 'procedures_call'
                   and block.get('mutation', {}).get('proccode') in SCHEDULER_CASES - cases}
        for block in blocks.values():
            if not isinstance(block, dict):
                continue
            while block.get('next') in skipped:
                block['next'] = blocks[block['next']].get('next')
        with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as out:
            for entry in archive.infolist():
                data = (json.dumps(project).encode() if entry.filename == 'project.json'
                        else archive.read(entry.filename))
                out.writestr(entry, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--flag', action='append', required=True)
    parser.add_argument('--common-flag', action='append', default=[],
                        help='Fixed XYO_NAME=value setting shared by OFF and ON')
    parser.add_argument('--jit', choices=['hotspot','eager','off'], default='hotspot')
    parser.add_argument('--runs', type=int, default=8)
    parser.add_argument('--cpu', type=int)
    parser.add_argument('--case', action='append', choices=sorted(SCHEDULER_CASES))
    parser.add_argument('--full', action='store_true', help='run original project and verify non-scheduler checksums')
    args = parser.parse_args()
    if args.runs < 1 or any(not f.startswith('XYO_') for f in args.flag):
        parser.error('positive --runs and XYO_ flags required')
    common = dict(item.split('=', 1) for item in args.common_flag)
    if any(not key.startswith('XYO_') for key in common) or (set(args.flag) & set(common)):
        parser.error('--common-flag must be XYO_NAME=value and must not overlap --flag')
    if args.full and args.case:
        parser.error('--full and --case cannot be combined')
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    args.out.mkdir(parents=True, exist_ok=True)
    source_binary = args.binary.resolve()
    binary = args.out.resolve() / 'frozen-xyo'
    shutil.copy2(source_binary, binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    # Freeze the source archive too: other local benchmark jobs may rebuild
    # the shared cache while these repeated measurements are in progress.
    source_bytes = args.project.read_bytes()
    source_snapshot = args.out.resolve() / 'source-performance.sb3'
    source_snapshot.write_bytes(source_bytes)
    project = source_snapshot if args.full else args.out.resolve() / 'scheduler.sb3'
    if not args.full:
        scheduler_project(source_snapshot, project)
    expected_cases = set(args.case or SCHEDULER_CASES)
    if args.case:
        selected = args.out.resolve() / 'selected-scheduler.sb3'
        select_cases(project, selected, expected_cases)
        project = selected
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    binary_hash = digest(binary)
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env['XYO_JIT_MODE'] = args.jit
    env.update(common)
    results = []
    reference_checksums = None
    for run in range(args.runs):
        for enabled in ([False, True] if run % 2 == 0 else [True, False]):
            variant = 'on' if enabled else 'off'
            trial_env = {**env, **{f: str(int(enabled)) for f in args.flag}}
            proc = subprocess.run(
                [str(binary), 'run', str(project), '--fps', '30',
                 '--print-list', 'output'], env=trial_env,
                capture_output=True, text=True, timeout=180)
            stem = args.out / f'{run + 1}-{variant}'
            stem.with_suffix('.out').write_text(proc.stdout)
            stem.with_suffix('.err').write_text(proc.stderr)
            if proc.returncode or 'DONE' not in proc.stdout.splitlines():
                raise RuntimeError(f'{variant}: exit {proc.returncode}: {proc.stderr[-2000:]}')
            rows = {}
            checksums = {}
            for line in proc.stdout.splitlines():
                fields = [s.strip() for s in line.split('|')]
                if len(fields) == 6 and fields[0].startswith('sched_'):
                    rows[fields[0]] = float(fields[3])
                elif len(fields) == 6 and fields[0] != 'id':
                    checksums[fields[0]] = fields[5]
            if args.full:
                if len(checksums) != 39:
                    raise RuntimeError(f'expected 39 non-scheduler checksums, got {len(checksums)}')
                if reference_checksums is None:
                    reference_checksums = checksums
                elif checksums != reference_checksums:
                    raise RuntimeError(f'non-scheduler checksum mismatch: {checksums}')
            if set(rows) != expected_cases or any(v <= 0 for v in rows.values()):
                raise RuntimeError(f'incomplete scheduler output: {rows}')
            results.append({'run': run + 1, 'variant': variant, 'rows': rows,
                            'non_scheduler_checksums': checksums})
            print(run + 1, variant, rows, flush=True)
    if digest(binary) != binary_hash:
        raise RuntimeError('binary changed during paired measurement')
    summary = {}
    for key in results[0]['rows']:
        off = [r['rows'][key] for r in results if r['variant'] == 'off']
        on = [r['rows'][key] for r in results if r['variant'] == 'on']
        ratios = [b / a for a, b in zip(off, on)]
        summary[key] = {
            'off_median': statistics.median(off),
            'on_median': statistics.median(on),
            'paired_geomean': math.exp(statistics.mean(map(math.log, ratios))),
            'on_wins': sum(b > a for a, b in zip(off, on)),
        }
    report = {
        'binary': str(binary), 'original_binary': str(source_binary),
        'binary_sha256': binary_hash,
        'source': str(args.project.resolve()), 'source_sha256': hashlib.sha256(source_bytes).hexdigest(),
        'source_snapshot': str(source_snapshot), 'full': args.full,
        'non_scheduler_checksum_count': len(reference_checksums or {}),
        'scheduler_sha256': digest(project), 'flags': args.flag,
        'common_flags': common, 'jit': args.jit, 'fps': 30, 'cpu': args.cpu, 'runs': args.runs,
        'cases': sorted(expected_cases),
        'summary': summary, 'results': results,
    }
    (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
