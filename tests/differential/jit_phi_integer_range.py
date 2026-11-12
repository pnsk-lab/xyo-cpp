#!/usr/bin/env python3
"""Validate exact-integer SSA range union through a guarded IF/ELSE PHI.

Each side must produce a canonical numeric Value with an independently
verified bounded-integer range. Mixed values, NaN, and mutated aliases must
not inherit a range from their neighboring path.
"""
from __future__ import annotations
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_change,S_if,S_ifelse,S_set,add,
    div,floor_,item,lt,make_project,num,round_,sub,text,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen


def fixture(n:int,case:str):
    g=ForEachGen(['i','temp','sum'],['values','output'])
    i=var('i')
    if case=='both_integer':
        left=round_(i);right=floor_(add(i,num(1)))
    elif case=='two_negative':
        left=round_(sub(i,num(2)));right=floor_(sub(i,num(1)))
    elif case=='mixed_string':
        left=round_(i);right=text('5')
    elif case=='nan':
        left=round_(i);right=div(num(0),num(0))
    elif case=='infinity':
        left=round_(i);right=div(i,num(0))
    elif case=='fractional':
        left=round_(i);right=div(i,num(2))
    elif case=='only_same':
        left=round_(i);right=floor_(i)
    elif case=='compare_after':
        left=round_(i);right=floor_(add(i,num(1)))
    elif case=='compare_mixed':
        left=round_(i);right=text('5')
    elif case=='nested':
        left=floor_(add(i,num(1)));right=round_(i)
    elif case in ('if_numeric','if_mixed'):
        left=round_(i);right=round_(i)
    else:raise ValueError(case)
    then=[S_set('temp',left)]
    other=[S_set('temp',right)]
    split=S_ifelse(lt(i,num(n//2)),then,other)
    if case=='nested':split=S_ifelse(lt(i,num(n//3)),
             [S_ifelse(lt(i,num(n//2)),then,other)],other)
    if case=='if_numeric':
        body=[S_set('temp',text('bad')),S_set('temp',round_(i)),
              S_if(lt(i,num(n//2)),[S_set('temp',floor_(add(i,num(1))))]),
              S_change('sum',item('values',var('temp')))]
    elif case=='if_mixed':
        body=[S_set('temp',text('bad')),S_set('temp',round_(i)),
              S_if(lt(i,num(n//2)),[S_set('temp',text('5'))]),
              S_change('sum',item('values',var('temp')))]
    elif case in ('compare_after','compare_mixed'):
        body=[S_set('temp',text('bad')),split,
              S_if(lt(var('temp'),num(n//2)),
                [S_change('sum',item('values',var('temp')))])]
    else:
        body=[S_set('temp',text('bad')),split,
              S_change('sum',item('values',var('temp')))]
    g.script([S_set('sum',num(0)),('foreach','i',num(n),body),
              S_add('output',var('sum')),S_add('output',var('temp'))])
    return make_project(g.vars.keys(),g.lists.keys(),g,
                        {'values':[1]*(n+3)})


def run(binary,project,tier,enabled,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_UNARY_INTEGER_RANGE='1',
        XYO_JIT_LIST_PROVEN_I32_INDEX='1',
        XYO_JIT_COMPARE_PROVEN_NUM='3',
        XYO_JIT_LAZY_TYPED_SSA='1',
        XYO_JIT_LOCAL_VAR_FORWARD='1',
        XYO_JIT_IF_PHI='1',
        # Isolate this test's PHI-union behavior from later read-only-branch
        # forwarding, which has its own differential and IR tests.
        XYO_JIT_PHI_RANGE_BRANCH_FORWARD='0',
        XYO_JIT_PHI_RANGE_IFELSE_FORWARD='0',
        XYO_JIT_LAZY_MODE='off',
        XYO_PARFOR='0',XYO_GPU='0')
    if enabled is not None:env['XYO_JIT_PHI_INTEGER_RANGE']='1' if enabled else '0'
    if dump:env['XYO_JIT_DUMP2']='1'
    proc=subprocess.run([str(binary),'run',str(project),'--jit',tier,
        '--print-list','output'],cwd=ROOT,env=env,capture_output=True,
        text=True,timeout=80)
    if proc.returncode:
        raise RuntimeError((project,tier,enabled,proc.returncode,proc.stderr[-1100:]))
    return tuple(line.strip() for line in proc.stdout.splitlines()
                 if line.strip()),proc.stderr


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--out',type=Path,
        default=ROOT/'build/multi-bench-20261110/phi-integer-range')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    cases=('both_integer','two_negative','mixed_string','nan','infinity',
           'fractional','only_same','nested','if_numeric','if_mixed',
           'compare_after','compare_mixed')
    for case in cases:
        project=args.out/f'{case}.sb3'
        write_sb3(project,fixture(512,case))
        expected,_=run(binary,project,'off',False)
        for tier in ('eager','hotspot'):
            for flag in (False,True,None):
                got,_=run(binary,project,tier,flag)
                assert got==expected,(case,tier,flag,expected,got)
        print('PASS',case,expected,flush=True)
    for case in cases:
        for flag in (False,True,None):
            _,ir=run(binary,args.out/f'{case}.sb3','eager',flag,True)
            suffix='default' if flag is None else str(int(flag))
            (args.out/f'{case}-{suffix}.ll').write_text(ir)
            print('IR',case,suffix,'typed_numeric_phi',ir.count('typed.merge.number'),
                  'cvtt',ir.count('call i64 @llvm.x86.sse2.cvttsd2si64('),
                  'fptoui',ir.count('fptoui double'),flush=True)
    def cvtt(case,mode):
        return (args.out/f'{case}-{mode}.ll').read_text().count(
            'call i64 @llvm.x86.sse2.cvttsd2si64(')
    for case in ('both_integer','only_same','nested','if_numeric'):
        assert cvtt(case,'0') > cvtt(case,'1') == cvtt(case,'default'),case
    # A new Scratch IF introduces a separate CFG region; its inner list read
    # conservatively reloads the scalar, so no range crosses this new edge.
    for case in ('two_negative','mixed_string','nan','infinity','fractional','if_mixed',
                 'compare_after','compare_mixed'):
        assert cvtt(case,'0') == cvtt(case,'1') == cvtt(case,'default'),case
    print('PASS all integer PHI differential fixtures',flush=True)

if __name__=='__main__':main()
