#!/usr/bin/env python3
"""Check no-wrap LLVM loop-counter flags against interpreter semantics.

The original private repeat counter is updated only through its guarded
latch; optional LLVM nuw/nsw facts must not change Scratch-visible behavior.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tests/differential'),
                str(ROOT/'tests/bench/algos')]
from gen_algo_bench import write_sb3
from jit_repeat_integer_counter import fixture


def execute(binary: Path, project: Path, mode: str, nowrap: bool):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('XYO_')}
    env.update(XYO_JIT_REPEAT_COUNTER_NOWRAP='1' if nowrap else '0',
               XYO_JIT_LAZY_MODE='off', XYO_GPU='0', XYO_PARFOR='0',
               XYO_REPEAT_UNROLL='0')
    p = subprocess.run([str(binary), 'run', str(project), '--jit', mode,
                        '--print-list', 'result'], cwd=ROOT, env=env,
                       text=True, capture_output=True, timeout=90)
    if p.returncode:
        raise RuntimeError((project.name, mode, nowrap, p.returncode,
                            p.stderr[-800:]))
    return tuple(s.strip() for s in p.stdout.splitlines() if s.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path,
                        default=ROOT/'build/multi-bench-20261110/nowrap-parity')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    binpath = args.binary.resolve()
    testcases = ((0, False), (1, False), (2, False), (31, False),
                 (64, False), (65, False), (1001, False),
                 (-3, False), (0.5, False), (8.5, True), (1600.5, False))
    for n, nested in testcases:
        project = args.out/f'repeat-{n}-nested-{nested}.sb3'
        write_sb3(project, fixture(n, nested=nested))
        reference = execute(binpath, project, 'off', False)
        for mode in ('eager', 'hotspot'):
            for nowrap in (False, True):
                observed = execute(binpath, project, mode, nowrap)
                assert observed == reference, (n, nested, mode, nowrap,
                                               reference, observed)
        print(f'PASS n={n} nested={nested} output={reference}', flush=True)
    print(f'PASS {len(testcases)} fixtures, interpreter/eager/hotspot, nowrap on/off', flush=True)


if __name__ == '__main__':
    main()
