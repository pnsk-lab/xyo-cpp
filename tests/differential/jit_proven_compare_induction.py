#!/usr/bin/env python3
"""Verify that a yield-free read-only for-each index stays numeric in pure IFs.

No opaque calls, aliases or mutable Scratch state are assumed numeric: the
compiler must reject any induction-range proof if a branch writes the index.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import S_add,S_change,S_set,S_if,S_ifelse,add,eq,gt,lt,item,num,round_,floor_,var,make_project,write_sb3
from jit_list_proven_i32_index import ForEachGen


def fixture(limit,kind):
    g = ForEachGen(['i','sum'],['values','output'])
    i = var('i')
    if kind == 'lt_round':
        body = [S_if(lt(round_(i),num(250)), [S_change('sum',item('values',i))])]
    elif kind == 'gt_floor':
        body = [S_if(gt(floor_(i),num(250)), [S_change('sum',item('values',i))])]
    elif kind == 'eq_round':
        body = [S_if(eq(round_(i),num(250)), [S_change('sum',item('values',i))])]
    elif kind == 'if_else':
        body = [S_ifelse(lt(round_(i),num(250)),
                         [S_change('sum',item('values',i))],
                         [S_change('sum',num(-1))])]
    elif kind == 'nested':
        body = [S_if(gt(i,num(10)),[S_if(lt(round_(i),num(250)),
                       [S_change('sum',item('values',i))])])]
    elif kind == 'index_write':
        body = [S_if(lt(round_(i),num(250)),
                         [S_set('i',num(-100)),S_change('sum',item('values',i))])]
    elif kind == 'fractional':
        body = [S_if(lt(round_(i),num(250.25)),
                         [S_change('sum',item('values',i))])]
    elif kind == 'string_literal':
        body = [S_if(lt(round_(i),num(250)),
                         [S_change('sum',item('values',i))])]
    else: raise ValueError(kind)
    g.script([S_set('sum',num(0)),('foreach','i',num(limit),body),
              S_add('output',var('sum')),S_add('output',var('i'))])
    values = list(range(1,1001)) if kind != 'string_literal' else ['42','abc',0,'',3.5]+list(range(6,1001))
    return make_project(g.vars.keys(),g.lists.keys(),g,{'values':values})


def run(binary,project,jit,enabled,dump=False):
    env = {k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_UNARY_INTEGER_RANGE='1',
               XYO_JIT_LIST_PROVEN_I32_INDEX='1',
               XYO_JIT_LAZY_MODE='off',XYO_PARFOR='0',XYO_GPU='0',
               XYO_JIT_LLVM_REPEAT_HINT='0')
    if enabled is not None:env['XYO_JIT_COMPARE_PROVEN_NUM']=str(enabled)
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',jit,
                      '--print-list','output'],cwd=ROOT,env=env,
                     capture_output=True,text=True,timeout=60)
    if p.returncode:raise RuntimeError((project.name,jit,enabled,p.returncode,p.stderr[-850:]))
    result=tuple(x.strip() for x in p.stdout.splitlines() if x.strip())
    return result,p.stderr


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
                    default=ROOT/'build/multi-bench-20261110/proven-compare')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binpath=args.binary.resolve()
    cases=('lt_round','gt_floor','eq_round','if_else',
           'nested','index_write','fractional','string_literal')
    for kind in cases:
        p=args.out/f'{kind}.sb3'
        write_sb3(p,fixture(500,kind))
        baseline,_=run(binpath,p,'off',False)
        for jit in ('eager','hotspot'):
            for mode in (0,1,2,3,None):
                actual,_=run(binpath,p,jit,mode)
                assert actual==baseline,(kind,jit,mode,baseline,actual)
        print('PASS',kind,baseline,flush=True)
    for kind in ('lt_round','eq_round','index_write'):
        for mode in (0,1,2,3,None):
            _,ir=run(binpath,args.out/f'{kind}.sb3','eager',mode,True)
            (args.out/f'{kind}-{str(mode) if mode is not None else "default"}.ll').write_text(ir)
            print('IR',kind,mode,'cvtt_calls',ir.count('call i64 @llvm.x86.sse2.cvttsd2si64('),
                  'num_eq_calls',ir.count('call i32 @xyo_lt('),
                  'truthy_calls',ir.count('call i32 @xyo_truthy('),flush=True)
    # The restricted proof is intentionally unavailable for equals and
    # a self-mutating index, but remains available for ordered comparisons.
    for kind in ('lt_round','eq_round','index_write'):
        no=(args.out/f'{kind}-0.ll').read_text()
        mode3=(args.out/f'{kind}-3.ll').read_text()
        cvtt=lambda ir:ir.count('call i64 @llvm.x86.sse2.cvttsd2si64(')
        if kind=='lt_round': assert cvtt(mode3)<cvtt(no),(kind,cvtt(mode3),cvtt(no))
        if kind=='eq_round': assert cvtt(mode3)==cvtt(no),(kind,cvtt(mode3),cvtt(no))
        if kind=='index_write': assert cvtt(mode3)==cvtt(no),(kind,cvtt(mode3),cvtt(no))
    # JIT pins live values and string caches at host process addresses;
    # their pointer constants naturally differ between separate executions.
    # Verify the branch-lowering signature instead of comparing raw pointers.
    for kind in ('lt_round','eq_round','index_write'):
        default=(args.out/f'{kind}-default.ll').read_text()
        mode3=(args.out/f'{kind}-3.ll').read_text()
        for pattern in ('call i64 @llvm.x86.sse2.cvttsd2si64(',
                        'icmp ult i64', 'phi i32', 'fcmp olt double'):
            assert default.count(pattern)==mode3.count(pattern),(
                kind,pattern,default.count(pattern),mode3.count(pattern))
    print('PASS all pure IF / comparison integer range scenarios, modes 0/1/2/3/default',flush=True)

if __name__=='__main__':main()
