#!/usr/bin/env python3
"""Archived numeric-reporter fact trial: requires saved trial binary, not normal build."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import (Gen, S_add, S_call, S_change, S_ifelse, S_repeat, S_set, add, arg,
    div, eq, gt, item, join, length, lt, make_project, mod, mul, num, op, sub,
    text, var, write_sb3)


def project():
    g = Gen(['x', 'rope'], ['data', 'out'])
    reporters = [op('operator_length', STRING=var('x')), length('data'),
                 op('data_itemnumoflist', {'LIST': 'data'}, ITEM=var('x'))]
    body = [S_set('x', arg('input'))]
    for reporter in reporters:
        body += [S_add('out', reporter), S_add('out', add(reporter, num(0.5))),
            S_add('out', mul(reporter, num(-2))), S_add('out', sub(num(3), reporter)),
            S_add('out', div(num(1), reporter)), S_add('out', mod(reporter, num(3))),
            S_add('out', div(num(1), mul(reporter, num(-1))))]
        for other in (text('0'), text(' '), text('NaN'), text('0017'), num(0), num(2)):
            for compare in (eq, lt, gt):
                body.append(S_ifelse(compare(reporter, other),
                    [S_add('out', text('true'))], [S_add('out', text('false'))]))
    body.append(S_add('out', join(var('x'), text('Z'))))
    g.define('evaluate %s', ['input'], body, warp=True)
    inputs = [text(s) for s in ('', ' ', '0017', '-0', 'NaN', 'Infinity', '-Infinity',
        '\U0001f600', 'a\U0001f600b', '\ud800', 'a\x00b', 'TRUE', 'abc')]
    inputs += [num(0), num(-2), num(0.5), num(1e20),
               div(num(0), num(0)), div(num(1), num(0)),
               op('math_boolean', {'BOOL': True}), op('math_boolean', {'BOOL': False})]
    script = [S_call('evaluate %s', [value]) for value in inputs]
    # A long cons string exercises length without flattening its original value.
    script += [S_set('rope', text('')), S_repeat(num(128),
        [S_set('rope', join(var('rope'), text('\U0001f600ab')))]),
        S_call('evaluate %s', [var('rope')])]
    g.script(script)
    return make_project(g.vars, g.lists, g,
        {'data': [0, '0', '0017', ' ', True, False, 'NaN', -0.0, '\U0001f600', 'abc']})


def execute(binary, path, mode, enabled, out, obj=None, dump=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_NUMERIC_REPORTER_FACTS=str(enabled), XYO_HOT_INVOCATIONS='1',
        XYO_HOT_STATS='1', XYO_GPU='0', XYO_PAR='0', XYO_PARFOR='0')
    if dump:
        env['XYO_JIT_DUMP2'] = '1'
    cmd = [str(binary), 'run', str(path), '--jit', mode, '--print-list', 'out']
    if obj:
        cmd += ['--aot', str(obj)]
    p = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True,
                       errors='surrogateescape', timeout=120)
    assert p.returncode == 0, (mode, enabled, p.stderr[-2000:])
    if mode == 'hotspot':
        assert re.search(r'^\[hot\]\s+\S+\s+\d+\s+\d+\s+yes\s', p.stderr, re.M)
    if obj:
        assert 'aot: load failed' not in p.stderr
        assert re.search(r'aot: [1-9][0-9]* scripts precompiled', p.stderr)
    if dump:
        (out / f'{mode}-{enabled}.ll').write_text(p.stderr)
    return p.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--aot', action='store_true')
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    assert b'XYO_JIT_NUMERIC_REPORTER_FACTS' in binary.read_bytes(), \
        'discarded trial requires archived reporter-paired/runtime, not normal build'
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for variant in ('valid', 'missing-field', 'unresolved-list'):
        current = out / variant
        current.mkdir(exist_ok=True)
        path = current / 'numeric-reporters.sb3'
        spec = project()
        if variant != 'valid':
            for target in spec['targets']:
                for block in target['blocks'].values():
                    if block['opcode'] in ('data_lengthoflist', 'data_itemnumoflist'):
                        if variant == 'missing-field':
                            block['fields'].pop('LIST', None)
                        else:
                            block['fields']['LIST'] = ['unknown-list', 'unknown-list-id']
        write_sb3(path, spec)
        expected = execute(binary, path, 'off', 0, current)
        assert len(expected) == 22 * 76, len(expected)
        for mode in ('eager', 'hotspot'):
            for enabled in (0, 1):
                assert execute(binary, path, mode, enabled, current, dump=mode == 'eager') == expected, (variant, mode, enabled)
        if args.aot:
            for enabled in (0, 1):
                env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
                env['XYO_JIT_NUMERIC_REPORTER_FACTS'] = str(enabled)
                obj = current / f'reporters-{enabled}.o'
                p = subprocess.run([str(binary), 'aot', str(path), '--obj', str(obj),
                    '-o', str(current / f'reporters-app-{enabled}')], env=env, cwd=ROOT,
                    capture_output=True, text=True, timeout=120)
                assert p.returncode == 0, p.stderr[-2000:]
                assert execute(binary, path, 'eager', enabled, current, obj=obj) == expected, ('aot', variant, enabled)
        total += len(expected)
        print(f'PASS {variant}: {len(expected)} outputs', flush=True)
    print(f'PASS {total} reporter outputs, interp/eager/hotspot, facts ON/OFF' +
          (', actual AOT ON/OFF' if args.aot else ''), flush=True)


if __name__ == '__main__':
    main()
