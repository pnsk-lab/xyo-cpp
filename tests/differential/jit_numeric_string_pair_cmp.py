#!/usr/bin/env python3
"""Verify cached numeric-string comparisons stay equivalent to Scratch.

Tests the same dynamic variable/variable pair against interpreter in Eager,
HotSpot and LLVM O2/3, both with and without direct LLVM comparison.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/bench/algos'))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set, eq, gt, lt,
    make_project, num, text, var, write_sb3,
)

VALUES = [
    (text('3'), text('03')),
    (text('0'), text('-0')),
    (text('-0'), text('00')),
    (text('1.25'), text('1.250')),
    (text('1e3'), text('1000')),
    (text('0x10'), text('16')),
    (text('2'), text('10')),
    (text('-1'), text('2')),
    (text('Infinity'), text('+Infinity')),
    (text('-Infinity'), text('Infinity')),
    (text('infinity'), text('Infinity')),
    (text('INF'), text('Infinity')),
    (text('NaN'), text('NaN')),
    (text('abc'), text('ABC')),
    (text('hello'), text('HELLO')),
    (text(''), text('0')),
    (text(' '), text('0')),
    (text('abc'), text('0')),
    (text('3'), num(3)),
    (num(3), text('03')),
    (num(0), num(-0.0)),
    (num(3), num(4)),
    (text('🍣'), text('日本')),
    (text('1e100'), text('1e101')),
    (text('1e-200'), text('0')),
    (text('+02'), text('2.0')),
    (text('0'), text('false')),
    (text('TRUE'), text('true')),
    (text('K'), text('k')),
    (text('İ'), text('i̇')),
    (text('ß'), text('ss')),
    (text('日本語'), text('日本語')),
    (text('🍣'), text('🍤')),
    (text('a\x00'), text('a')),
    (text('a'), text('a\x00')),
    (text('\x00'), text('')),
    (text('HELLO'), text('hello')),
    (text('zzzz'), text('ZZZZ')),
    (text('z'), text('a')),
    (text('a'), text('z')),
    # 8-16 character vector equality: equal/casefold/different and prefix.
    (text('abcdefgh'), text('ABCDEFGH')),
    (text('abcdefgh'), text('abcdefgi')),
    (text('abcdefgha'), text('ABCDEFGHA')),
    (text('abcdefghi'), text('abcdefghj')),
    (text('Hello World'), text('hELLO wORLD')),
    (text('Hello World'), text('HELLO WOrLd!')),
    (text('ABCDEFGHIJKL'), text('abcdefghijkl')),
    (text('abcdefghijklmno'), text('ABCDEFGHIJKLMNO')),
    (text('abcdefghijklmnop'), text('ABCDEFGHIJKLMNOP')),
    (text('abcdefghijklmnop'), text('ABCDEFGHIJKLMNOp')),
    (text('abcdefghijklmnopq'), text('ABCDEFGHIJKLMNOPQ')),
    (text('a\x00bcdefgh'), text('A\x00BCDEFGH')),
    (text('abcdefgİjk'), text('abcdefgi̇jk')),
    (text('a\x00bcdefgh'), text('a\x00BcDEfgH')),
]


def build(path: Path, warp: bool) -> list[str]:
    fields = [f'{relation}_{i}' for i in range(len(VALUES))
              for relation in ('eq', 'lt', 'gt')]
    g = Gen(['left','right','x','done',*fields], [])
    body = []
    for i, (left, right) in enumerate(VALUES):
        checks = []
        for op, fn in [('eq',eq),('lt',lt),('gt',gt)]:
            checks.append(S_if(fn(var('left'),var('right')),
                               [S_change(f'{op}_{i}', num(1))]))
        body.extend([S_set('left',left), S_set('right',right),
                     S_repeat(num(130), checks)])
    g.define('compare', [], body, warp=warp)
    g.script([*(S_set(field,num(0)) for field in fields),
              S_call('compare'), S_set('done',num(1))])
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return [*fields, 'done']


def run(binary: Path, path: Path, fields: list[str], mode: str,
        config: dict[str,str]) -> dict[str,str]:
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_DUMPVARS='json',XYO_GPU='0',XYO_PAR='0')
    env.update(config)
    proc=subprocess.run([str(binary),'run',str(path),'--jit',mode,
                         '--frames','1000'],cwd=ROOT,env=env,
                        text=True,capture_output=True,timeout=120)
    if proc.returncode:
        raise RuntimeError((mode,config,proc.returncode,proc.stderr[-1600:]))
    output=proc.stdout
    d=json.loads(output[output.index('{'):output.rindex('}')+1])['Bench']
    return {f:d[f] for f in fields}


def main():
    binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/xyo'
    out=ROOT/'build/comparison-pair-20261106'
    out.mkdir(parents=True,exist_ok=True)
    for warp in (True, False):
        project=out/('pair-warp.sb3' if warp else 'pair-nonwarp.sb3')
        fields=build(project,warp)
        baseline=run(binary,project,fields,'off',{})
        assert baseline['done']=='1'
        for mode,config in [
            ('eager',{}), ('eager',{'XYO_JIT_EQ_ASCII_SIMD':'0'}),
            ('eager',{'XYO_JIT_STRING_PAIR_NUMERIC':'0'}),
            ('eager',{'XYO_JIT_STRING_PAIR_ASCII':'0'}),
            ('eager',{'XYO_JIT_STRING_PAIR_UNICODE_STABLE':'0'}),
            ('hotspot',{}), ('hotspot',{'XYO_JIT_EQ_ASCII_SIMD':'0'}),
            ('hotspot',{'XYO_JIT_STRING_PAIR_NUMERIC':'0'}),
            ('hotspot',{'XYO_JIT_STRING_PAIR_ASCII':'0'}),
            ('hotspot',{'XYO_JIT_STRING_PAIR_UNICODE_STABLE':'0'}),
            ('eager',{'XYO_JIT_OPT_LEVEL':'2'}),
            ('eager',{'XYO_JIT_EQ_GENERIC':'1'}),
        ]:
            data=run(binary,project,fields,mode,config)
            differences={k:(baseline[k],data[k]) for k in fields
                         if baseline[k]!=data[k]}
            assert not differences,(warp,mode,config,differences)
            print('PASS',warp,mode,config,len(fields),flush=True)


if __name__=='__main__':main()
