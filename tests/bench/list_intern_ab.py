#!/usr/bin/env python3
"""Frozen-binary, equal-work 3D2 A/B (list sharing or JIT math boxing).

Run from the workspace root after building. No build runs during measurement.
The output directory must be new, so previous evidence is never overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ppm(path):
    data = path.read_bytes()
    header = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    if not header:
        raise ValueError(f'Unsupported PPM header: {path}')
    w, h = map(int, header.groups())
    pixels = data[header.end():]
    assert len(pixels) == w * h * 3
    return w, h, pixels


def compare_images(off, on, mask_fps=True):
    w, h, a = ppm(off)
    wb, hb, b = ppm(on)
    assert (w, h) == (wb, hb) == (960, 720), 'FPS mask assumes scale=2'
    points = [(i % w, i // w) for i in range(w * h)
              if a[3*i:3*i+3] != b[3*i:3*i+3]]
    # This project displays wall-clock FPS at the upper right. No scene pixel
    # may be excluded: narrow bounds cover only its known numeric label.
    # A fractional FPS glyph's antialiased edge can reach x=731 (Res=4).
    outside = [(x, y) for x, y in points
               if not mask_fps or not (618 <= x <= 731 and 54 <= y <= 69)]
    result = dict(different_pixels=len(points), outside_fps=len(outside),
                  bounds=([min(x for x, y in points), max(x for x, y in points),
                           min(y for x, y in points), max(y for x, y in points)]
                          if points else None))
    assert not outside, result
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, default=Path('build/tests/xyo-pen-bench'))
    ap.add_argument('--project', type=Path, default=Path('3D2.sb3'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--backend', choices=['gl', 'cpu'], default='gl')
    ap.add_argument('--comparison', choices=['list-intern', 'math-box'], default='list-intern')
    ap.add_argument('--normalize-fps', action='store_true',
                    help='normalize only the FPS monitor after timing; require full-image identity')
    ap.add_argument('--cpu', type=int, default=12)
    ap.add_argument('--trials', type=int, default=5)
    args = ap.parse_args()
    assert args.trials >= 3
    args.output.mkdir(parents=True, exist_ok=False)
    frozen = (args.output / 'probe-frozen').resolve()
    shutil.copy2(args.binary, frozen)
    digest = sha256(frozen)
    project_digest = sha256(args.project)
    manifest = dict(binary_sha256=digest, project_sha256=project_digest,
                    source_binary=str(args.binary.resolve()), cpu=args.cpu,
                    backend=args.backend, comparison=args.comparison,
                    normalize_fps=args.normalize_fps,
                    frames=90, warmup_active_frames=60, trials=args.trials)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    base = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    base.update(SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy',
                XYO_SEED='1', XYO_BENCH_WARMUP_FRAMES='60',
                XYO_BENCH_CPU=str(args.cpu))
    if args.normalize_fps:
        base['XYO_BENCH_SHOT_MONITOR'] = '@fps'
    if args.backend == 'cpu':
        base.update(XYO_PEN_CPU='1', SDL_RENDER_DRIVER='software')
    # Preserve an explicitly chosen GL vendor; don't require NVIDIA on other hosts.
    rows = []
    for res, trial in [(1, i) for i in range(args.trials)] + [(4, 0)]:
        for name in (['off', 'on'] if trial % 2 == 0 else ['on', 'off']):
            assert sha256(frozen) == digest
            assert sha256(args.project) == project_digest
            env = dict(base)
            if args.comparison == 'list-intern':
                env['XYO_SB3_LIST_INTERN'] = '0' if name == 'off' else '1'
            elif name == 'off':
                env['XYO_JIT_MATH_BOX_GENERIC'] = '1'
            if res == 4:
                env.update(XYO_SET='Res=4', XYO_BENCH_ACTIVE_STROKES='10000')
            stem = f'{name}-res{res}-trial{trial}'
            cmd = [str(frozen), '--project', str(args.project), '--frames', '90']
            if trial == 0:
                cmd += ['--shot', str(args.output / f'{stem}.ppm')]
            p = subprocess.run(cmd, env=env, capture_output=True, text=True)
            (args.output / f'{stem}.stdout').write_text(p.stdout)
            (args.output / f'{stem}.stderr').write_text(p.stderr)
            p.check_returncode()
            assert sha256(frozen) == digest
            if args.normalize_fps and trial == 0:
                assert 'PROJECT snapshot_monitor_matches=1' in p.stdout
            assert f'PROJECT pinned_main_cpu={args.cpu}' in p.stdout
            assert 'warmup_active=60 active=90 steps=90 draws=90' in p.stdout
            assert 'PROJECT par_batches=0 dispatches=0 threads=0 groups=0' in p.stdout
            assert not any(s in p.stderr for s in ['JIT session error', 'failed to compile'])
            m = re.search(r'PROJECT strokes=(\d+) quads=(\d+) frames=(\d+) flushes=(\d+) readbacks=(\d+) uploads=(\d+)', p.stdout)
            counts = list(map(int, m.groups())) if m else []
            strokes = 15552000 if res == 1 else 972000
            expected = ([strokes, strokes, 90, 180 if res == 1 else 90, 0, 0]
                        if args.backend == 'gl' else [strokes, 0, 90, 0, 0, 90])
            assert counts == expected, p.stdout
            wall = float(re.search(r'PROJECT total=([\d.]+)ms', p.stdout)[1])
            cpu = float(re.search(r'PROJECT process_cpu=([\d.]+)ms', p.stdout)[1])
            row = dict(name=name, trial=trial, res=res, total_ms=wall, cpu_ms=cpu,
                       counts=counts, binary_sha256=digest)
            rows.append(row)
            print(json.dumps(row), flush=True)
            (args.output / 'results.json').write_text(json.dumps(rows, indent=2))
    summary = {}
    for name in ['off', 'on']:
        group = [r for r in rows if r['name'] == name and r['res'] == 1]
        walls = [r['total_ms'] for r in group]
        median = statistics.median(walls)
        summary[name] = dict(median_ms=median, fps=90000 / median,
                             median_cpu_ms=statistics.median(r['cpu_ms'] for r in group),
                             range_ms=[min(walls), max(walls)])
    summary['faster_pairs'] = sum(
        next(r['total_ms'] for r in rows if r['name'] == 'on' and r['res'] == 1 and r['trial'] == i)
        < next(r['total_ms'] for r in rows if r['name'] == 'off' and r['res'] == 1 and r['trial'] == i)
        for i in range(args.trials))
    summary['images'] = {str(res): compare_images(
        args.output / f'off-res{res}-trial0.ppm', args.output / f'on-res{res}-trial0.ppm',
        mask_fps=not args.normalize_fps)
        for res in [1, 4]}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    if not __debug__:
        raise SystemExit('Do not use python -O/PYTHONOPTIMIZE: workload checks require assertions')
    main()
