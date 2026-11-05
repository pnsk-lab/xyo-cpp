#!/usr/bin/env python3
"""Compare generic Scratch semantics with guarded SSA variable-copy loops.

Includes yield-boundary counts, nonnumerics, numeric strings, signed zero,
float values, out-of-range integers, copied values, and non-warp execution.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import Gen, S_call, S_change, S_repeat, S_set, num, text, var, write_sb3, make_project


def fixture(path: Path, warp: bool) -> list[str]:
    cases = [
        # count, starting a/b/c, delta, kind
        (0, num(0), num(0), num(0), 1, 'copychain'),
        (1, num(0), num(0), num(0), 1, 'copychain'),
        (2, num(0), num(0), num(0), 1, 'copychain'),
        (3, num(0), num(0), num(0), 1, 'copychain'),
        (63, num(2), num(4), num(7), 1, 'copychain'),
        (64, num(2), num(4), num(7), 1, 'copychain'),
        (65, num(2), num(4), num(7), 1, 'copychain'),
        (127, num(2), num(4), num(7), 1, 'copychain'),
        (128, num(2), num(4), num(7), 1, 'copychain'),
        (129, num(2), num(4), num(7), 1, 'copychain'),
        (300, num(-5), num(-4), num(-3), 3, 'copychain'),
        (10000, num(0), num(0), num(0), 1, 'copychain'),
        (130, num(1), num(2), num(3), -2, 'interleave'),
        (128, num(1), num(2), num(3), 2, 'reverse'),
        (64, text('a'), num(5), num(6), 1, 'copychain'),
        (65, text('0'), text('0'), text('0'), 1, 'copychain'),
        (130, text(''), text('1.25'), text('abc'), 1, 'copychain'),
        (70, num(1.25), num(2.75), num(3.5), -1, 'copychain'),
        (70, text('-0'), text('0'), text('-0'), 1, 'copychain'),
        (64, text('Infinity'), text('-Infinity'), num(0), 2, 'copychain'),
        (130, num(281474976710650), num(0), num(0), 3, 'copychain'),
        (130, num(-281474976710650), num(0), num(0), -4, 'copychain'),
        (75, num(281474976710655), num(1), num(0), 65536, 'copychain'),
        (75, num(1), num(2), num(3), 0, 'reverse'),
        (125, num(1), num(2), num(3), 65537, 'interleave'),
    ]
    outputs = [f'r{i}_{varname}' for i in range(len(cases)) for varname in 'abc']
    g = Gen(['a', 'b', 'c', 'limit', 'done', *outputs], [])
    stmts = []
    for i, (count, a, b, c, delta, kind) in enumerate(cases):
        if kind == 'copychain':
            ops = [S_set('a', var('c')), S_set('b', var('a')),
                   S_set('c', var('b')), S_change('c', num(delta))]
        elif kind == 'interleave':
            ops = [S_change('b', num(delta)), S_set('a', var('b')),
                   S_change('a', num(1)), S_set('c', var('a'))]
        else:
            ops = [S_set('a', var('b')), S_set('b', var('c')),
                   S_set('c', var('a')), S_change('b', num(delta))]
        stmts.extend([
            S_set('a', a), S_set('b', b), S_set('c', c),
            S_repeat(num(count), ops),
            S_set(outputs[3 * i], var('a')),
            S_set(outputs[3 * i + 1], var('b')),
            S_set(outputs[3 * i + 2], var('c')),
        ])
    # Dynamic counts cannot enable the i32 repeat induction path.
    stmts.extend([
        S_set('limit', num(65)), S_set('a', num(0)), S_set('b', num(1)),
        S_repeat(var('limit'), [S_set('a', var('b')), S_change('b', num(1))]),
        S_set('done', num(1)),
    ])
    g.define('copies', [], stmts, warp=warp)
    g.script([S_call('copies')])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*outputs, 'done', 'a', 'b', 'c']


def run(binary: Path, sb3: Path, keys: list[str], mode: str,
        enabled: bool, extra_env: dict[str,str]|None=None) -> dict[str,str]:
    env = {k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_DUMPVARS='json', XYO_GPU='0', XYO_PAR='0',
               XYO_JIT_REPEAT_COPY_SSA='1' if enabled else '0')
    env.update(extra_env or {})
    proc = subprocess.run([str(binary), 'run', str(sb3), '--jit', mode,
                           '--frames', '1000'], cwd=ROOT, env=env,
                          text=True, capture_output=True, timeout=120)
    if proc.returncode:
        raise AssertionError((mode,enabled,proc.returncode,proc.stderr[-1200:]))
    output = proc.stdout
    record = json.loads(output[output.index('{'):output.rindex('}')+1])['Bench']
    return {k:record[k] for k in keys}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/xyo'
    out = ROOT/'build/variable-investigation-20261105'
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = out/('copies-warp.sb3' if warp else 'copies-nonwarp.sb3')
        keys = fixture(path, warp)
        baseline = run(binary,path,keys,'off',False)
        assert baseline['done']=='1',baseline
        for mode,enabled,flags in [
            ('eager',True,{}),('eager',False,{}),
            ('hotspot',True,{}),('hotspot',False,{}),
            ('eager',True,{'XYO_JIT_OPT_LEVEL':'2'}),
        ]:
            result=run(binary,path,keys,mode,enabled,flags)
            if result!=baseline:
                diff={k:(baseline[k],result[k]) for k in keys
                      if baseline[k]!=result[k]}
                raise AssertionError((warp,mode,enabled,flags,diff))
            print('PASS',warp,mode,enabled,flags,flush=True)


if __name__=='__main__': main()
