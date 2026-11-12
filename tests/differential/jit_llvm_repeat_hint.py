#!/usr/bin/env python3
"""Check LLVM loop hint against interpreter and the identical JIT without hints.

The hint changes only the IR metadata passed to LLVM's optimization pipeline.
All logical Scratch iterations and scheduler checks must remain observable.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import Gen, S_add, S_change, S_repeat, S_set, add, make_project, num, var, write_sb3


def fixture(count: float, nested: bool = False):
    g = Gen(['sum', 'index'], ['output'])
    body = [S_change('index', num(1)), S_change('sum', add(var('index'), num(3)))]
    if nested:
        body.append(S_repeat(num(3), [S_change('sum', num(2))]))
    g.script([S_set('sum', num(0)), S_set('index', num(0)),
              S_repeat(num(count), body), S_add('output', var('sum'))])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def execute(binary: Path, sb3: Path, jit: str, hint: int):
    env = {key: value for key, value in os.environ.items() if not key.startswith('XYO_')}
    env.update(XYO_JIT_LLVM_REPEAT_HINT=str(hint),
               XYO_REPEAT_UNROLL='0', XYO_PARFOR='0', XYO_GPU='0')
    proc = subprocess.run([str(binary), 'run', str(sb3), '--jit', jit,
                           '--print-list', 'output'], cwd=ROOT, env=env,
                          text=True, capture_output=True, timeout=60)
    if proc.returncode:
        raise RuntimeError((str(sb3), jit, hint, proc.returncode,
                            proc.stderr[-900:]))
    return tuple(line.strip() for line in proc.stdout.splitlines() if line.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path,
                        default=ROOT/'build/multi-bench-20261110/llvm-hint-differential')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    cases = ((0, False), (1, False), (2, False), (3, False),
             (6, False), (12, False), (31, False), (64, False),
             (65, False), (5.5, False), (12, True))
    for count, nested in cases:
        sb3 = args.out / f'repeat-{count}-nested-{nested}.sb3'
        write_sb3(sb3, fixture(count, nested))
        expected = execute(binary, sb3, 'off', 0)
        for jit in ('eager', 'hotspot'):
            for hint in (0, 1, 2):
                actual = execute(binary, sb3, jit, hint)
                assert actual == expected, (count, nested, jit, hint,
                                            expected, actual)
        print(f'PASS count={count} nested={nested}: {expected}', flush=True)
    print(f'PASS {len(cases)} fixtures; Interpreter/Eager/HotSpot, hint 0/1/2', flush=True)


if __name__ == '__main__':
    main()
