#!/usr/bin/env python3
"""Integer numeric classification and arithmetic-only list projection parity."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/differential'))
from jit_numeric_region_list import fixture, write_sb3
from gen_algo_bench import (Gen, S_add, S_call, S_change, S_ifelse, S_repeat, S_set,
    add, div, eq, item, make_project, mod, mul, num, sub, var)

PROFILES = {'base': (0, 0), 'classify': (1, 0), 'projection': (0, 1), 'both': (1, 1)}


def operation_fixture(kind, values):
    g = Gen(['i', 'acc', 'temp'], ['data', 'indices', 'checks', 'probe'])
    read = item('data', add(mod(var('i'), num(len(values))), num(1)))
    expressions = {
        'subtract': sub(num(3), read), 'multiply': mul(read, num(-2)),
        'divide_lhs': div(read, num(-2)), 'divide_rhs': div(num(-7), read),
        'mod_lhs': mod(read, num(-3)), 'mod_rhs': mod(num(-7), read),
        'chain': add(mul(read, num(0.5)), num(1)),
        'index': item('indices', read),
    }
    body = [S_change('i', num(1))]
    if kind == 'phi':
        body += [S_ifelse(eq(mod(var('i'), num(2)), num(0)),
                    [S_set('temp', read)], [S_set('temp', num(1))]),
                 S_change('acc', var('temp'))]
    elif kind == 'shared_compare':
        body += [S_set('temp', read),
                 S_ifelse(eq(var('temp'), num(0)), [S_change('acc', var('temp'))],
                          [S_change('acc', add(var('temp'), num(1)))])]
    else:
        body += [S_change('acc', expressions[kind])]
    g.define('kernel', [], [S_repeat(num(129), body)], warp=True)
    g.script([S_set('i', add(num(0), num(0))), S_set('acc', add(num(0), num(0))),
        S_set('temp', add(num(0), num(0))), S_call('kernel'),
        S_add('checks', var('i')), S_add('checks', var('acc')),
        S_add('checks', var('temp')), S_add('checks', div(num(1), var('acc'))),
        S_add('probe', var('temp'))])
    return make_project(g.vars, g.lists, g,
        {'data': values, 'indices': [1, '2', '0017', ' ', True, False, 0, -0.0]})


def execute(binary, path, mode, flags, finite, storage, out, label, dump=False, obj=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_GPU='0', XYO_PAR='0', XYO_PARFOR='0', XYO_DUMPVARS='json',
               XYO_HOT_INVOCATIONS='1', XYO_HOT_STATS='1', XYO_LIST_STORAGE=storage,
               XYO_JIT_INTEGER_NUM_CLASSIFY=str(flags[0]),
               XYO_JIT_COERCION_PROJECTION=str(flags[1]),
               XYO_JIT_NUMERIC_REGION_LIST_FINITE=str(finite))
    if dump:
        env['XYO_JIT_DUMP2'] = '1'
    cmd = [str(binary), 'run', str(path), '--jit', mode, '--frames', '100000']
    if obj:
        cmd += ['--aot', str(obj)]
    p = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True, timeout=90)
    if p.returncode:
        raise RuntimeError((label, p.returncode, p.stderr[-1500:]))
    data = json.loads(p.stdout[p.stdout.index('{'):p.stdout.rindex('}') + 1])['Bench']
    if mode == 'hotspot':
        assert re.search(r'^\[hot\]\s+\S+\s+\d+\s+\d+\s+yes\s', p.stderr, re.M), label
    if obj:
        assert 'aot: load failed' not in p.stderr, label
        assert re.search(r'aot: [1-9][0-9]* scripts precompiled', p.stderr), label
    if dump:
        (out / f'{label}.ll').write_text(p.stderr)
    return data, p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--aot', action='store_true')
    ap.add_argument('--raw-smoke', type=Path)
    ap.add_argument('--checkpoint-smoke', type=Path)
    ap.add_argument('--include-classify-trial', action='store_true',
                    help='also test discarded integer-classification trial (requires archived binary)')
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    profiles = PROFILES if args.include_classify_trial else {key: PROFILES[key] for key in ('base', 'projection')}
    if args.include_classify_trial:
        assert b'XYO_JIT_INTEGER_NUM_CLASSIFY' in binary.read_bytes(), 'trial not in this binary'
        if args.raw_smoke:
            assert b'XYO_JIT_INTEGER_NUM_CLASSIFY' in args.raw_smoke.read_bytes(), 'trial not in raw smoke binary'
    out.mkdir(parents=True, exist_ok=True)
    arrays = [list(range(1, 65)), ['0', '1', '0017', ' ', 'abc', 'NaN', '0.5', '-0'],
              [True, False, 1, '1', '0', ' ', -0.0, 5e-324, -5e-324], []]
    cases = [(f'{kind}-a{i}', kind, values, 'flat')
             for kind in ('sum', 'copy', 'compare', 'fractional', 'outside', 'nonfinite')
             for i, values in enumerate(arrays[:2] if args.quick else arrays)]
    if not args.quick:
        cases += [(f'storage-{storage}', 'sum', arrays[2], storage)
                  for storage in ('sbo4', 'sbo8', 'deque', 'gap', 'chunked', 'adaptive')]
    operation_values = arrays[2] + ['Infinity', '-Infinity', 'NaN', '-0', '-3', '2.5']
    cases += [(f'op-{kind}', f'op-{kind}', operation_values, 'flat') for kind in
              ('subtract', 'multiply', 'divide_lhs', 'divide_rhs', 'mod_lhs',
               'mod_rhs', 'chain', 'phi', 'shared_compare', 'index')]
    comparisons = 0
    rows = []
    for label, kind, values, storage in cases:
        path = out / f'{label}.sb3'
        write_sb3(path, operation_fixture(kind[3:], values) if kind.startswith('op-')
                  else fixture(129, kind, values))
        expected, _ = execute(binary, path, 'off', (0, 0), 0, storage, out, label)
        for finite in (0, 1):
            for profile, flags in profiles.items():
                for mode in ('eager', 'hotspot'):
                    run_label = f'{label}-f{finite}-{profile}-{mode}'
                    dump = label == 'sum-a0' and mode == 'eager'
                    got, ir = execute(binary, path, mode, flags, finite, storage, out, run_label, dump)
                    assert got == expected, (run_label, got, expected)
                    if dump:
                        assert 'mixed.list.bounded' in ir, 'expected real typed list region: ' + run_label
                        # The archived classifier build predates these IR names.
                        if not args.include_classify_trial:
                            assert ('mixed.list.raw.accepted' in ir) == (flags[1] == 0), \
                                ('redundant raw-tag gate', run_label)
                            assert ('mixed.list.finite' in ir) == bool(finite), \
                                ('required finite guard', run_label)
                    comparisons += 1
        rows.append(label)
        print('PASS', label, flush=True)
    if args.aot:
        for kind in ('sum', 'copy', 'compare', 'op-mod_lhs', 'op-mod_rhs', 'op-phi', 'op-index'):
            path = out / f'aot-{kind}.sb3'
            write_sb3(path, operation_fixture(kind[3:], operation_values) if kind.startswith('op-')
                      else fixture(129, kind, arrays[2]))
            expected, _ = execute(binary, path, 'off', (0, 0), 1, 'flat', out, kind)
            for profile, flags in profiles.items():
                obj = out / f'aot-{kind}-{profile}.o'
                env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
                env.update(XYO_JIT_INTEGER_NUM_CLASSIFY=str(flags[0]),
                           XYO_JIT_COERCION_PROJECTION=str(flags[1]))
                p = subprocess.run([str(binary), 'aot', str(path), '--obj', str(obj),
                                    '-o', str(out / f'aot-app-{kind}-{profile}')],
                                   env=env, cwd=ROOT, capture_output=True, text=True, timeout=120)
                assert p.returncode == 0, p.stderr[-1500:]
                got, _ = execute(binary, path, 'eager', flags, 1, 'flat', out, kind, obj=obj)
                assert got == expected, ('AOT', kind, profile)
                comparisons += 1
        print(f'PASS actual AOT arithmetic/copy/compare/mod/Phi/index, {len(profiles)} profiles', flush=True)
    raw_runs = 0
    if args.raw_smoke:
        raw_json = out / 'raw-values.json'
        raw_json.write_text(json.dumps(fixture(129, 'sum', [])))
        for finite in (0, 1):
            for profile, flags in profiles.items():
                env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
                env.update(XYO_GPU='0', XYO_PAR='0', XYO_PARFOR='0', XYO_LIST_STORAGE='flat',
                    XYO_JIT_INTEGER_NUM_CLASSIFY=str(flags[0]),
                    XYO_JIT_COERCION_PROJECTION=str(flags[1]),
                    XYO_JIT_NUMERIC_REGION_LIST_FINITE=str(finite))
                p = subprocess.run([str(args.raw_smoke.resolve()), str(raw_json)],
                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
                assert p.returncode == 0, (finite, profile, p.stdout[-1500:], p.stderr[-1500:])
                assert 'PASS raw list projections:' in p.stdout, p.stdout
                (out / f'raw-f{finite}-{profile}.log').write_text(p.stdout + p.stderr)
                raw_runs += 1
        print(f'PASS {raw_runs} raw Value runs, interpreter/JIT/AOT, tags preserved', flush=True)
    checkpoint_runs = 0
    if args.checkpoint_smoke:
        from gen_numeric_region_list_checkpoint import project as checkpoint_project
        checkpoint = out / 'native-checkpoints.sb3'
        write_sb3(checkpoint, checkpoint_project())
        for enabled in (0, 1):
            env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
            env['XYO_JIT_COERCION_PROJECTION'] = str(enabled)
            p = subprocess.run([str(args.checkpoint_smoke.resolve()), str(checkpoint)],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
            assert p.returncode == 0, (enabled, p.stdout[-2000:], p.stderr[-2000:])
            assert p.stdout.count('completed=1025 PASS') == 9, p.stdout
            (out / f'checkpoints-{enabled}.log').write_text(p.stdout + p.stderr)
            checkpoint_runs += 1
        print('PASS 18 real native-yield configurations; mutations/storage/epochs rechecked', flush=True)
    summary = dict(cases=rows, comparisons=comparisons, raw_runs=raw_runs,
                   checkpoint_runs=checkpoint_runs, all_equal=True)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'PASS {len(rows)} fixtures, {comparisons} exact JIT/AOT comparisons')


if __name__ == '__main__':
    main()
