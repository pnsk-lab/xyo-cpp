#!/usr/bin/env python3
"""Audited helper attributes: edge-value parity, optimized-IR CSE, paired timing.

Usage: python3 tests/differential/jit_llvm_helper_attributes.py BINARY [--bench]
No benchmark-specific code generation is used. The benchmark deliberately repeats
identical dynamic math expressions to expose the missing call-effect information.
"""
from __future__ import annotations
import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import (Gen, S_add, S_call, S_change, S_repeat, S_set,
                           add, arg, days, div, make_project, mul, num, op,
                           sub, text, var, write_sb3)


def mathop(name, value):
    return op('operator_mathop', {'OPERATOR': name}, NUM=value)


def edge_project():
    g = Gen(['x'], ['out'])
    body = [S_set('x', arg('input'))]
    for name in ('asin', 'acos', 'atan', 'e ^', 'ln'):
        body.append(S_add('out', mathop(name, var('x'))))
    # Preserve observable sign-of-zero coverage despite String(-0) == '0'.
    body.append(S_add('out', div(num(1), mathop('asin', var('x')))))
    g.define('evaluate %s', ['input'], body)
    values = [-1.0, math.nextafter(-1.0, 0), math.nextafter(-1.0, -math.inf),
              -0.975, -0.5, -1e-300, -5e-324, 0, 5e-324, 1e-300,
              0.5, 0.975, math.nextafter(1.0, 0), 1.0,
              math.nextafter(1.0, math.inf), -2, 2, -1000, 1000]
    inputs = [num(v) for v in values] + [text(s) for s in
              ('-0', 'NaN', 'Infinity', '-Infinity', '0.5', ' 0.5 ', '', 'bad')]
    g.script([S_call('evaluate %s', [value]) for value in inputs])
    return make_project(g.vars, g.lists, g), len(inputs) * 6


def cse_project(trip):
    g = Gen(['x', 'sum', 't0'], ['out'])
    pair = add(add(mathop('asin', var('x')), mathop('asin', var('x'))),
               add(mathop('acos', var('x')), mathop('acos', var('x'))))
    g.define('kernel %s', ['count'], [
        S_set('x', num(0.125)), S_set('sum', num(0)),
        S_repeat(arg('count'), [S_change('x', num(1e-7)),
                               S_change('sum', pair)])])
    g.script([S_call('kernel %s', [num(1000)]), S_set('t0', days()),
              S_call('kernel %s', [num(trip)]),
              S_add('out', mul(sub(days(), var('t0')), num(86400000))),
              S_add('out', var('sum'))])
    return make_project(g.vars, g.lists, g)


def run(binary, project, mode, enabled, dump=False, aot=None, profile=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_PURE_MATH=str(int(enabled)), XYO_GPU='0', XYO_PARFOR='0',
               XYO_HOT_INVOCATIONS='1', XYO_HOT_STATS='1')
    if dump:
        env['XYO_JIT_DUMP2'] = '1'
    if profile:
        env['XYO_PROF'] = '1'
    cmd = [str(binary), 'run', str(project), '--jit', mode, '--print-list', 'out']
    if aot:
        cmd += ['--aot', str(aot)]
    p = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError((cmd, p.returncode, p.stderr[-2000:]))
    if mode == 'hotspot':
        assert re.search(r'^\[hot\]\s+\S+\s+\d+\s+\d+\s+yes\s', p.stderr, re.M), \
            ('HotSpot did not compile this fixture', p.stderr[-2000:])
    if aot:
        assert 'aot: load failed' not in p.stderr, p.stderr[-2000:]
        assert re.search(r'aot: [1-9][0-9]* scripts precompiled', p.stderr), p.stderr[-2000:]
    return p.stdout.splitlines(), p.stderr


def check_attributes(ir, enabled):
    groups = dict(re.findall(r'^attributes #(\d+) = \{([^\n]*)\}', ir, re.M))
    for name in ('asin', 'acos', 'atan', 'exp', 'log'):
        declaration = re.search(r'^declare double @xyo_' + name + r'\([^\n]*', ir, re.M)
        assert declaration, ('missing declaration', name)
        group = re.search(r'#(\d+)', declaration[0])
        attrs = groups[group[1]] if group else declaration[0]
        pure = 'memory(none)' in attrs
        assert pure == (enabled and name in ('asin', 'acos')), (name, attrs)
        if pure:
            assert 'nounwind' in attrs and 'willreturn' in attrs, (name, attrs)


def call_counts(ir, name):
    counts = Counter()
    # Enumerating definitions also separates identical local names in different
    # dumped modules. Count redundancy only inside one basic block, not across
    # mutually exclusive cloned/peeled paths.
    functions = re.findall(r'^define [^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
    for function_index, function in enumerate(functions):
        blocks = re.split(r'^[^\s;][^:\n]*:.*$', function, flags=re.M)
        for block_index, block in enumerate(blocks):
            operands = re.findall(r'\bcall\b[^\n]*@xyo_' + name + r'\(double ([^)]+)\)', block)
            counts.update((function_index, block_index, value) for value in operands)
    return dict(total=sum(counts.values()), max_same_input=max(counts.values(), default=0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/llvm-helper-effects/results')
    parser.add_argument('--bench', action='store_true')
    parser.add_argument('--rounds', type=int, default=7)
    parser.add_argument('--trip', type=int, default=1000000)
    parser.add_argument('--aot', action='store_true')
    parser.add_argument('--profile', action='store_true', help='require active runtime profiling')
    args = parser.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    edge = out / 'edges.sb3'
    project, count = edge_project()
    write_sb3(edge, project)
    expected, _ = run(binary, edge, 'off', False, profile=args.profile)
    assert len(expected) == count, (len(expected), count)
    for mode in ('eager', 'hotspot'):
        for enabled in (False, True):
            got, ir = run(binary, edge, mode, enabled, dump=True, profile=args.profile)
            assert got == expected, ('parity', mode, enabled)
            if args.profile and mode == 'eager':
                assert '[rtprof] arith' in ir, 'runtime arithmetic profiler was not exercised'
            (out / f'edges-{mode}-{int(enabled)}.ll').write_text(ir)
            check_attributes(ir, enabled)
    print(f'PASS edge parity: {count} outputs, interpreter/eager/hotspot, attributes on/off', flush=True)
    cse = out / 'cse.sb3'
    write_sb3(cse, cse_project(args.trip))
    callcounts = {}
    checksum = None
    for enabled in (False, True):
        result, ir = run(binary, cse, 'eager', enabled, dump=True, profile=args.profile)
        assert len(result) == 2, result
        checksum = checksum or result[1]
        assert result[1] == checksum, ('cse checksum', enabled, result)
        (out / f'cse-{int(enabled)}.ll').write_text(ir)
        callcounts[enabled] = {name: call_counts(ir, name) for name in ('asin', 'acos')}
    for name in ('asin', 'acos'):
        assert callcounts[False][name]['max_same_input'] >= 2, (name, callcounts)
        assert callcounts[True][name]['max_same_input'] == 1, (name, callcounts)
    print('PASS optimized IR CSE:', callcounts, 'checksum:', checksum, flush=True)
    if args.aot:
        for enabled in (False, True):
            obj = out / f'edges-{int(enabled)}.o'
            env = dict(os.environ, XYO_JIT_PURE_MATH=str(int(enabled)))
            p = subprocess.run([str(binary), 'aot', str(edge), '--obj', str(obj),
                                '-o', str(out / f'edges-app-{int(enabled)}')],
                               env=env, capture_output=True, text=True, timeout=120)
            assert p.returncode == 0, p.stderr[-2000:]
            got, _ = run(binary, edge, 'eager', enabled, aot=obj, profile=args.profile)
            assert got == expected, ('aot parity', enabled)
        print('PASS AOT edge parity on/off', flush=True)
    if args.bench:
        samples = {False: [], True: []}
        for round_no in range(args.rounds):
            for enabled in ((False, True) if round_no % 2 == 0 else (True, False)):
                result, _ = run(binary, cse, 'eager', enabled, profile=args.profile)
                assert result[1] == checksum, result
                samples[enabled].append(float(result[0]))
                print('timing', round_no, int(enabled), result[0], 'ms', flush=True)
        medians = {str(int(k)): statistics.median(v) for k, v in samples.items()}
        summary = dict(samples={str(int(k)): v for k, v in samples.items()},
                       medians_ms=medians, speedup=medians['0'] / medians['1'],
                       checksum=checksum, calls=callcounts)
        (out / 'paired.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
