#!/usr/bin/env python3
"""Fixed-work coalesced short-broadcast probe (not a ScratchBench score)."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import zipfile


def fixture(iterations):
    blocks = {}
    def block(key, opcode, next=None, inputs=None, fields=None, **extra):
        blocks[key] = dict(opcode=opcode, next=next, parent=None,
                           inputs=inputs or {}, fields=fields or {},
                           shadow=False, topLevel=False, **extra)
        return key
    number = lambda n: [1, [4, str(n)]]
    variable = lambda name: [1, [12, name, name]]
    message = {'BROADCAST_INPUT': [1, [11, 'PING', 'ping']]}
    mutation = dict(tagName='mutation', children=[], proccode='burst',
                    argumentids='[]', argumentnames='[]', argumentdefaults='[]', warp='true')
    block('flag', 'event_whenflagclicked', 'warmup')
    blocks['flag']['topLevel'] = True
    block('warmup', 'control_repeat', 'clear',
          {'TIMES': number(12), 'SUBSTACK': [2, 'warm']})
    block('warm', 'event_broadcastandwait', inputs=message)
    block('clear', 'data_setvariableto', 'start', {'VALUE': number(0)}, {'VARIABLE': ['hits', 'hits']})
    block('start', 'data_setvariableto', 'call', {'VALUE': [2, 'clock0']}, {'VARIABLE': ['t0', 't0']})
    block('clock0', 'sensing_dayssince2000')
    block('call', 'procedures_call', 'finish')
    blocks['call']['mutation'] = mutation
    block('finish', 'data_setvariableto', 'drain', {'VALUE': [2, 'duration']}, {'VARIABLE': ['elapsed', 'elapsed']})
    block('clock1', 'sensing_dayssince2000')
    block('difference', 'operator_subtract', inputs={'NUM1': [2, 'clock1'], 'NUM2': variable('t0')})
    block('duration', 'operator_multiply', inputs={'NUM1': [2, 'difference'], 'NUM2': number(86400000)})
    block('drain', 'event_broadcastandwait', 'time', message)
    block('time', 'data_addtolist', 'checksum', {'ITEM': variable('elapsed')}, {'LIST': ['output', 'output']})
    block('checksum', 'data_addtolist', inputs={'ITEM': variable('hits')}, fields={'LIST': ['output', 'output']})
    block('definition', 'procedures_definition', 'repeat', {'custom_block': [1, 'prototype']})
    blocks['definition']['topLevel'] = True
    block('prototype', 'procedures_prototype')
    blocks['prototype'].update(shadow=True, mutation=mutation)
    block('repeat', 'control_repeat', inputs={'TIMES': number(iterations), 'SUBSTACK': [2, 'send']})
    block('send', 'event_broadcast', inputs=message)
    block('receiver', 'event_whenbroadcastreceived', 'inc', fields={'BROADCAST_OPTION': ['PING', 'ping']})
    blocks['receiver']['topLevel'] = True
    block('inc', 'data_changevariableby', inputs={'VALUE': number(1)}, fields={'VARIABLE': ['hits', 'hits']})
    stage = dict(isStage=True, name='Stage', variables={n: [n, 0] for n in ('hits', 't0', 'elapsed')},
                 lists={'output': ['output', []]}, broadcasts={'ping': 'PING'}, blocks=blocks,
                 comments={}, costumes=[], sounds=[], currentCostume=0, volume=100,
                 tempo=60, videoTransparency=50, videoState='on', textToSpeechLanguage=None)
    return dict(targets=[stage], monitors=[], extensions=[], meta={})


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--runs', type=int, default=12)
    ap.add_argument('--iterations', type=int, default=200000)
    ap.add_argument('--cpu', type=int)
    args = ap.parse_args()
    if args.runs < 1 or args.iterations < 1:
        ap.error('runs and iterations must be positive')
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    project = args.out.resolve() / 'coalesced.sb3'
    with zipfile.ZipFile(project, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('project.json', json.dumps(fixture(args.iterations)))
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env['XYO_RESTART_THREAD_REUSE'] = '0'
    results = []
    for run in range(args.runs):
        for on in ([False, True] if run % 2 == 0 else [True, False]):
            label = 'on' if on else 'off'
            trial_env = {**env, 'XYO_LIGHT_BROADCAST_RESTART': str(int(on))}
            p = subprocess.run([str(binary), 'run', str(project), '--jit', 'hotspot',
                                '--print-list', 'output'], env=trial_env,
                               capture_output=True, text=True, timeout=90)
            (args.out / f'{run + 1}-{label}.out').write_text(p.stdout)
            (args.out / f'{run + 1}-{label}.err').write_text(p.stderr)
            values = p.stdout.splitlines()
            if p.returncode or len(values) != 2 or float(values[1]) != 1:
                raise RuntimeError(f'invalid probe ({p.returncode}): {p.stdout} {p.stderr[-2000:]}')
            elapsed = float(values[0])
            if elapsed <= 0:
                raise RuntimeError('measurement too short')
            results.append(dict(run=run + 1, variant=label, ms=elapsed, checksum=1))
            print(run + 1, label, elapsed, flush=True)
    off = [r['ms'] for r in results if r['variant'] == 'off']
    on = [r['ms'] for r in results if r['variant'] == 'on']
    if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
        raise RuntimeError('binary changed during measurement')
    report = dict(binary=str(binary), binary_sha256=binary_hash, cpu=args.cpu,
                  iterations=args.iterations, runs=args.runs,
                  off_median_ms=statistics.median(off), on_median_ms=statistics.median(on),
                  paired_speedup=math.exp(statistics.mean(math.log(a/b) for a,b in zip(off,on))),
                  on_wins=sum(b < a for a,b in zip(off,on)), results=results)
    (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'results'}, indent=2))


if __name__ == '__main__':
    main()
