#!/usr/bin/env python3
"""Differential test for a guarded, positive-integer small-modulus shortcut.

The full numeric-domain cases run as ordinary Scratch expressions in a warp
procedure (not hand-picked benchmark IDs). The shortcut must preserve both
sign and zero; negative, fractional and oversized inputs fall back.
"""
from __future__ import annotations
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import Gen, S_call, S_add, S_set, div, make_project, mod, num, text, var, write_sb3

DIVISORS = (1, 2, 3, 4, 7, 12, 256, 65536, 65537, 2147483647)


def cases(m: int):
    return (
        num(-2 * m), num(-m), num(-1), num(-0.0), num(0),
        num(0.5), num(1), num(m - 1), num(m), num(m + 0.5),
        num(2 * m - 1), num(2 * m), num(2 * m + 1),
        num(2 * m + 0.5), num(3 * m), num(1e12),
        text('0'), text('-0'), text('12.0'), text(' 12 '),
        text('NaN'), text('Infinity'), text('not-a-number'),
    )


def project(m: int):
    gen = Gen(['x'], ['output'])
    body = []
    for value in cases(m):
        body.extend([
            S_set('x', value),
            S_add('output', mod(var('x'), num(m))),
        ])
    gen.define('test', [], body, warp=True)
    gen.script([S_call('test')])
    return make_project(gen.vars.keys(), gen.lists.keys(), gen)


def run(binary: Path, sb3: Path, jit: str, wrap: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_MOD_SMALL_WRAP='1' if wrap else '0',
               XYO_JIT_MODE=jit, XYO_PARFOR='0', XYO_GPU='0')
    cmd = [str(binary), 'run', str(sb3), '--jit', jit, '--print-list', 'output']
    p = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                       capture_output=True, timeout=45)
    if p.returncode != 0:
        raise RuntimeError(f'{sb3.name} {jit} wrap={wrap}: {p.returncode} {p.stderr[-700:]}')
    return tuple(s.strip() for s in p.stdout.splitlines() if s.strip())


def main():
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT/'build/xyo').resolve()
    out = ROOT/'build/life-codecompare-20261110/mod-wrap-cases'
    out.mkdir(exist_ok=True, parents=True)
    data = []
    for divisor in DIVISORS:
        sb3 = out/f'mod-{divisor}.sb3'
        write_sb3(sb3, project(divisor))
        expected = run(binary, sb3, 'off', False)
        assert len(expected) == len(cases(divisor)), (divisor, len(expected), expected)
        for jit in ('eager', 'hotspot'):
            for shortcut in (False, True):
                result = run(binary, sb3, jit, shortcut)
                if result != expected:
                    raise AssertionError((divisor, jit, shortcut, expected, result))
        data.append({'divisor': divisor, 'count': len(expected), 'result': list(expected)})
        print(f'PASS % {divisor}: {len(expected)} values, interpreter/eager/hotspot, wrap OFF/ON', flush=True)
    (out/'results.json').write_text(json.dumps(data, indent=2)+'\n')
    print(f'PASS {len(DIVISORS)} divisors x {len(cases(12))} inputs = {sum(len(cases(m)) for m in DIVISORS)} Scratch results',flush=True)


if __name__ == '__main__':
    main()
