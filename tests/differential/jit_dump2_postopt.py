#!/usr/bin/env python3
"""DUMP2 must contain the actual O3 IR, including when compilation is lazy.

This protects against the diagnostic pitfall where a deferred LLVM module is
printed before its IRTransformLayer has run the default optimization pipeline.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import Gen, S_add, S_call, S_change, S_repeat, S_set, make_project, num, var, write_sb3


def make_fixture():
    gen = Gen(['counter'], ['result'])
    gen.define('work', [], [
        S_repeat(num(31), [
            S_add('result', var('counter')),
            S_change('counter', num(1)),
        ])
    ], warp=True)
    gen.script([S_set('counter', num(0)), S_call('work')])
    return make_project(gen.vars.keys(), gen.lists.keys(), gen)


def execute(binary: Path, project: Path, lazy: str, dump: bool,
            preopt: bool = False):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_LAZY_MODE=lazy, XYO_JIT_MODE='hotspot',
               XYO_PARFOR='0', XYO_GPU='0', XYO_REPEAT_UNROLL='0')
    if dump:
        env['XYO_JIT_DUMP2'] = '1'
    if preopt:
        env['XYO_JIT_DUMP_PREOPT'] = '1'
    result = subprocess.run([str(binary), 'run', str(project), '--jit', 'hotspot',
                             '--print-list', 'result'], cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError((lazy, dump, result.returncode, result.stderr[-1000:]))
    return result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path,
                        default=ROOT/'build/multi-bench-20261110/dump2-postopt')
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    project = args.out/'two-function-repeat.sb3'
    write_sb3(project, make_fixture())
    reference = None
    for lazy in ('off', 'on'):
        stdout, stderr = execute(binary, project, lazy, True)
        (args.out/f'{lazy}.ll').write_text(stderr)
        (args.out/f'{lazy}.stdout').write_text(stdout)
        assert '; ModuleID = ' in stderr, (lazy, 'missing LLVM dump')
        # A raw JIT module includes a rep_i32 alloca; O3 promotes it into
        # loop-carried i32 Phi. Some other allocas (such as escaped GC roots)
        # may legitimately remain in optimized IR.
        assert not re.search(r'%rep_i32(?:\d+)? = alloca i32', stderr), (
            lazy, 'repeat i32 alloca not promoted')
        assert re.search(r'%rep_i32[^=\n]* = phi i32', stderr), (
            lazy, 'missing loop-counter Phi')
        data = tuple(line.strip() for line in stdout.splitlines() if line.strip())
        if reference is None:
            reference = data
        else:
            assert data == reference, ('lazy/eager output mismatch', data, reference)
        print(f'PASS lazy={lazy} result_count={len(data)} promoted_repeat_phi=true', flush=True)
    # Explicit raw+optimized request must show both distinct modules.
    stdout, stderr = execute(binary, project, 'on', True, preopt=True)
    assert stdout == (args.out/'on.stdout').read_text()
    assert stderr.count('; ModuleID = ') >= 2, (
        'missing raw or post-O3 lazy dump', stderr[-400:])
    assert re.search(r'%rep_i32(?:\d+)? = alloca i32', stderr), (
        'raw pre-optimization counter missing')
    (args.out/'raw-and-post.ll').write_text(stderr)
    print('PASS explicit PREOPT + DUMP2 contains raw and post-O3 IR', flush=True)
    print('PASS lazy and nonlazy DUMP2 post-O3 IR parity', flush=True)


if __name__ == '__main__':
    main()
