#!/usr/bin/env python3
"""Test numeric-literal equality with mutable numeric-string cache states.

A promoted variable may hold a numeric string whose SStr cache was populated
by an earlier fallback. The fast path may reuse the cache and update its
private numeric shadow, but must preserve the original boxed string value.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import Gen, S_call, S_change, S_if, S_repeat, S_set, add, eq, make_project, num, text, var, write_sb3


def fixture(path: Path) -> list[str]:
    values = [
        num(0), num(1), num(-1), num(2), num(3), num(0.5), num(-0.0),
        num(1000), num(1e20),
        text('0'), text('-0'), text('00'), text('3'), text('03'), text('3.0'),
        text('1e3'), text('1e100'), text('-1'), text('1.0'), text(''),
        text(' '), text('abc'), text('NaN'), text('Infinity'), text('-Infinity'),
        text('true'), text('false'), text('NULL'), text('🍣'),
        add(num(3),num(0)), add(num(-1),num(1)),
    ]
    literals = [num(0), num(3), num(1), text('0'), text('3'), text('003'),
                text('-0'), text('1e3'), text('Infinity'), text('-Infinity'),
                text(''), text('NaN'), text('abc')]
    repeated = [
        (text('003'), text('3')), (text('-0'), text('0')),
        (text('Infinity'), text('Infinity')),
        (text('infinity'), text('Infinity')),
        (text('NaN'), text('NaN')), (text(' '), text('0')),
        (text('abc'), text('0')), (text('0x10'), text('16')),
        (text('1e3'), text('1000')), (text('1.25'), text('1.25')),
        (text(''), text('0')), (text('HELLO'), text('hello')),
        (num(3), text('3')), (num(-0.0), text('0')),
        (text('🍣'), text('2')),
    ]
    fields = [f'fwd_{i}' for i in range(len(values))]
    fields += [f'rev_{i}' for i in range(len(values))]
    fields += [f'loop_{i}' for i in range(len(repeated))]
    g = Gen(['candidate', 'sum', 'done', *fields], [])
    body=[]
    for i, value in enumerate(values):
        body.append(S_set('candidate', value))
        body.append(S_set('sum',num(0)))
        for literal in literals:
            body.append(S_if(eq(var('candidate'), literal),
                             [S_change('sum', num(1))]))
        body.append(S_set(f'fwd_{i}', var('sum')))
        body.append(S_set('sum',num(0)))
        for literal in literals:
            body.append(S_if(eq(literal, var('candidate')),
                             [S_change('sum', num(1))]))
        body.append(S_set(f'rev_{i}', var('sum')))
    g.define('compare', [], body, warp=True)
    repeated_body=[]
    for i, (dynamic, literal) in enumerate(repeated):
        loop_body = [
            S_if(eq(var('candidate'), literal), [S_change('sum',num(1))]),
            S_if(eq(literal, var('candidate')), [S_change('sum',num(1))]),
        ]
        # Switch the variable from its original string type to numeric inside
        # some loops, checking that a cached shadow is properly overwritten.
        if i % 5 == 0:
            loop_body.append(S_change('candidate', num(1)))
        repeated_body.extend([
            S_set('candidate', dynamic), S_set('sum',num(0)),
            S_repeat(num(130), loop_body),
            S_set(f'loop_{i}', var('sum')),
        ])
    g.define('repeated', [], repeated_body, warp=True)
    g.script([S_call('compare'), S_call('repeated'), S_set('done',num(1))])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*fields, 'done']


def run(binary: Path, sb3: Path, fields: list[str], mode: str, config: dict[str,str]):
    env = {k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update({'XYO_DUMPVARS':'json', 'XYO_GPU':'0', 'XYO_PAR':'0'})
    env.update(config)
    p = subprocess.run([str(binary),'run',str(sb3),'--jit',mode,'--frames','30'],
                       cwd=ROOT, env=env, text=True, capture_output=True, timeout=120)
    if p.returncode:
        raise RuntimeError((mode,config,p.returncode,p.stderr[-1200:]))
    raw=p.stdout
    data=json.loads(raw[raw.index('{'):raw.rindex('}')+1])['Bench']
    return {field:data[field] for field in fields}


def main():
    binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/xyo'
    out=ROOT/'build/life-investigation-20261106'
    out.mkdir(parents=True,exist_ok=True)
    sb3=out/'eq-plain-numeric.sb3'
    fields=fixture(sb3)
    baseline=run(binary,sb3,fields,'off',{})
    assert baseline['done']=='1',baseline
    for label,mode,config in [
        ('eager','eager',{}),
        ('eager generic','eager',{'XYO_JIT_EQ_STRING_CACHE':'0'}),
        ('hotspot','hotspot',{}),
        ('hotspot generic','hotspot',{'XYO_JIT_EQ_STRING_CACHE':'0'}),
        ('O2','eager',{'XYO_JIT_OPT_LEVEL':'2'}),
        ('numeric generic','eager',{'XYO_JIT_DISABLE_EQ_NUMERIC':'1'}),
    ]:
        current=run(binary,sb3,fields,mode,config)
        if current!=baseline:
            errors={key:(baseline[key],current[key]) for key in fields
                    if baseline[key]!=current[key]}
            raise AssertionError((label,errors))
        print('PASS',label,len(fields),flush=True)


if __name__=='__main__':main()
