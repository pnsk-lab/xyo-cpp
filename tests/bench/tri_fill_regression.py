#!/usr/bin/env python3
"""Equal-work TFG A/B for headless Vulkan or rendered OpenGL playback.

At 1M stamps, compare the old capacity fallback against headless no-payload
execution (--mode run) or transactional GL chunking (--mode play).
All generated files go into a NEW output directory; source SB3 is untouched.
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
import time
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, default=Path('build/xyo'))
    ap.add_argument('--project', type=Path, default=Path('The fastest tri fill engine.sb3'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--mode', choices=['run', 'play'], default='run')
    ap.add_argument('--frames', type=int, default=12,
                    help='number of complete 1M-stamp batches (not a scheduler step cutoff)')
    ap.add_argument('--trials', type=int, default=5)
    args = ap.parse_args()
    assert args.frames >= 3 and args.trials >= 3
    args.output.mkdir(parents=True, exist_ok=False)
    frozen = (args.output / 'xyo-frozen').resolve()
    shutil.copy2(args.binary, frozen)
    digest = sha(frozen)
    original_digest = sha(args.project)
    with zipfile.ZipFile(args.project) as z:
        entries = [(i, z.read(i.filename)) for i in z.infolist()]
        project = json.loads(z.read('project.json'))
    matches = 0
    for target in project['targets']:
        for value in target.get('variables', {}).values():
            if value[0] == '@stamps':
                value[1] = 1000000
                matches += 1
    assert matches == 1
    # End after the SAME number of complete batches, even if the CPU fallback
    # yields at its 500ms budget. A scheduler-step cutoff can stop mid-batch.
    stage = next(t for t in project['targets'] if t['isStage'])
    fill = next(t for t in project['targets'] if t['name'] == 'fill')
    blocks = fill['blocks']
    hats = [b for b in blocks.values() if isinstance(b, dict) and
            b.get('opcode') == 'event_whenflagclicked']
    assert len(hats) == 1
    loop_id = hats[0]['next']
    loop = blocks[loop_id]
    assert loop['opcode'] == 'control_forever'
    counter_id = '__xyo_bench_completed'
    stop_id, count_id = '__xyo_bench_stop', '__xyo_bench_count'
    assert counter_id not in stage['variables'] and stop_id not in blocks and count_id not in blocks
    stage['variables'][counter_id] = ['__xyo_bench_batches', 0]
    loop['opcode'] = 'control_repeat'
    loop['inputs']['TIMES'] = [1, [6, str(args.frames)]]
    loop['next'] = stop_id
    blocks[stop_id] = dict(opcode='control_stop', next=None, parent=loop_id,
                          inputs={}, fields={'STOP_OPTION': ['all', None]})
    tps_id = next(i for i, v in stage['variables'].items() if v[0] == '@tps')
    tps_setters = [(i, b) for i, b in blocks.items() if isinstance(b, dict) and
                   b.get('opcode') == 'data_setvariableto' and
                   b.get('fields', {}).get('VARIABLE', [None, None])[1] == tps_id]
    assert len(tps_setters) == 1 and tps_setters[0][1].get('next') is None
    setter_id, setter = tps_setters[0]
    setter['next'] = count_id
    blocks[count_id] = dict(opcode='data_changevariableby', next=None, parent=setter_id,
                           inputs={'VALUE': [1, [4, '1']]},
                           fields={'VARIABLE': ['__xyo_bench_batches', counter_id]})
    variant = (args.output / 'stamps-1000000.sb3').resolve()
    with zipfile.ZipFile(variant, 'x') as z:
        for info, data in entries:
            z.writestr(info, json.dumps(project).encode() if info.filename == 'project.json' else data)
    control, candidate = (('unbatched', 'chunked') if args.mode == 'play'
                          else ('recorded', 'headless'))
    manifest = dict(binary_sha256=digest, original_project_sha256=original_digest,
                    workload_sha256=sha(variant), stamps=1000000,
                    batches=args.frames, scheduler_frame_limit=10000, trials=args.trials,
                    mode=args.mode,
                    comparison=('XYO_GPU_GL_BATCH=0 vs default; OpenGL renderer' if args.mode == 'play'
                                else 'XYO_GPU_VK_RECORDS=1 vs default; no renderer'))
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    base = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    base.update(XYO_SEED='1', XYO_DUMPVARS='1')
    if args.mode == 'play':
        base.update(SDL_VIDEODRIVER='offscreen', SDL_RENDER_DRIVER='opengl',
                    SDL_AUDIODRIVER='dummy', XYO_EXIT_ON_STOP='1',
                    XYO_EXIT_FRAMES='10000')
    rows = []

    def run(name, trial, diagnostic=False):
        assert sha(frozen) == digest and sha(args.project) == original_digest
        assert sha(variant) == manifest['workload_sha256']
        env = dict(base)
        if name == control:
            if args.mode == 'play':
                env['XYO_GPU_GL_BATCH'] = '0'
            else:
                env['XYO_GPU_VK_RECORDS'] = '1'
        if diagnostic:
            env['XYO_GPU_DEBUG'] = '1'
        tag = f'{name}-' + ('diagnostic' if diagnostic else f'trial{trial}')
        start = time.perf_counter()
        cmd = ([str(frozen), 'play', str(variant), '--fps', '0'] if args.mode == 'play'
               else [str(frozen), 'run', str(variant), '--frames', '10000'])
        p = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=120)
        wall = 1000 * (time.perf_counter() - start)
        (args.output / f'{tag}.stdout').write_text(p.stdout)
        (args.output / f'{tag}.stderr').write_text(p.stderr)
        assert p.returncode == 0, (p.returncode, p.stderr)
        assert 'JIT session error' not in p.stderr
        assert sha(frozen) == digest
        stamps = re.search(r'var Stage/@stamps\[[^\n]+\]=([^\n]+)', p.stdout)
        tps = re.search(r'var Stage/@tps\[[^\n]+\]=([^\n]+)', p.stdout)
        elapsed = re.search(r'var fill/fps.time\[[^\n]+\]=([^\n]+)', p.stdout)
        batches = re.search(r'var Stage/__xyo_bench_batches\[[^\n]+\]=([^\n]+)', p.stdout)
        assert stamps and float(stamps[1]) == 1000000 and tps and elapsed
        assert batches and float(batches[1]) == args.frames, p.stdout
        if diagnostic:
            if name == candidate:
                assert p.stderr.count('executed 1000000 lanes') == args.frames, p.stderr
                assert 'CPU fallback' not in p.stderr, p.stderr
                if args.mode == 'play':
                    assert p.stderr.count('gpu exec: chunks lanes=1000000') == args.frames, p.stderr
            else:
                reason = 'plan rejected' if args.mode == 'play' else 'allocation budget exceeded'
                assert reason in p.stderr, p.stderr
                assert 'executed 1000000 lanes' not in p.stderr, p.stderr
            print('diagnostic', name, 'OK', flush=True)
            return
        row = dict(name=name, trial=trial, process_wall_ms=wall,
                   reported_tps=float(tps[1]), last_batch_seconds=float(elapsed[1]),
                   stamps=1000000, completed_batches=int(float(batches[1])),
                   total_stamps=1000000 * args.frames, exit_code=p.returncode)
        rows.append(row)
        (args.output / 'results.json').write_text(json.dumps(rows, indent=2))
        print(json.dumps(row), flush=True)

    for name in [control, candidate]:
        run(name, 0, diagnostic=True)
    for trial in range(args.trials):
        for name in ([control, candidate] if trial % 2 == 0 else [candidate, control]):
            run(name, trial)
    summary = {}
    for name in [control, candidate]:
        group = [r for r in rows if r['name'] == name]
        summary[name] = dict(
            median_process_wall_ms=statistics.median(r['process_wall_ms'] for r in group),
            median_reported_tps=statistics.median(r['reported_tps'] for r in group),
            tps_range=[min(r['reported_tps'] for r in group), max(r['reported_tps'] for r in group)],
            process_wall_range_ms=[min(r['process_wall_ms'] for r in group), max(r['process_wall_ms'] for r in group)])
    summary['faster_pairs'] = sum(
        next(r['process_wall_ms'] for r in rows if r['name'] == candidate and r['trial'] == i)
        < next(r['process_wall_ms'] for r in rows if r['name'] == control and r['trial'] == i)
        for i in range(args.trials))
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    if not __debug__:
        raise SystemExit('Do not use python -O/PYTHONOPTIMIZE: workload checks require assertions')
    main()
