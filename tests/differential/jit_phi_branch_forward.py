#!/usr/bin/env python3
"""Read-only second-IF forwarding of a proven numeric branch-join SSA value.

All negative controls must keep the previous conversion. The optimization
never authorizes alias writes, opaque calls, yields or mixed-type values.
"""
from __future__ import annotations
import argparse
import os
import subprocess
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_call,S_change,S_if,S_ifelse,S_repeat,
    S_set,add,floor_,item,lt,make_project,num,round_,text,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen


def fixture(size:int,kind:str):
    gen=ForEachGen(['i','temp','sum'],['values','output'])
    i=var('i')
    rhs=text('5') if kind=='mixed' else floor_(add(i,num(1)))
    merged=S_ifelse(lt(i,num(size//2)),
                   [S_set('temp',round_(i))],[S_set('temp',rhs)])
    read=S_change('sum',item('values',var('temp')))
    if kind=='read':
        after=S_if(lt(var('temp'),num(size//2)),[read])
    elif kind=='write':
        after=S_if(lt(var('temp'),num(size//2)),
                   [S_set('temp',text('5')),read])
    elif kind=='nested':
        after=S_if(lt(var('temp'),num(size//2)),
                   [S_repeat(num(1),[read])])
    elif kind=='mixed':
        after=S_if(lt(var('temp'),num(size//2)),[read])
    elif kind=='ifelse':
        after=S_ifelse(lt(var('temp'),num(size//2)),
                       [read],[S_change('sum',num(-1))])
    elif kind=='other_write':
        after=S_if(lt(var('temp'),num(size//2)),
                   [S_set('sum',num(1)),read])
    else:raise ValueError(kind)
    gen.script([S_set('sum',num(0)),
                ('foreach','i',num(size),
                 [S_set('temp',text('wrong')),merged,after]),
                S_add('output',var('sum')),
                S_add('output',var('temp'))])
    return make_project(gen.vars.keys(),gen.lists.keys(),gen,
                        {'values':[1]*(size+5)})


def run(binary:Path,project:Path,jit:str,enabled:bool|None,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    # Keep this regression focused on the older plain-IF forwarding.
    env.update(XYO_JIT_PHI_RANGE_IFELSE_FORWARD='0',
               XYO_JIT_PHI_INTEGER_RANGE='1',
               XYO_JIT_LAZY_TYPED_SSA='1',XYO_JIT_LOCAL_VAR_FORWARD='1',
               XYO_JIT_LIST_PROVEN_I32_INDEX='1',
               XYO_JIT_UNARY_INTEGER_RANGE='1',
               XYO_JIT_LAZY_MODE='off',XYO_PARFOR='0',XYO_GPU='0')
    if enabled is not None:env['XYO_JIT_PHI_RANGE_BRANCH_FORWARD']=str(int(enabled))
    if dump:env['XYO_JIT_DUMP2']='1'
    proc=subprocess.run([str(binary),'run',str(project),'--jit',jit,
                        '--print-list','output'],cwd=ROOT,
                        env=env,capture_output=True,text=True,timeout=75)
    if proc.returncode:
        raise RuntimeError((project,jit,enabled,proc.returncode,
                            proc.stderr[-900:]))
    return tuple(x.strip() for x in proc.stdout.splitlines() if x.strip()),proc.stderr


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
         default=ROOT/'build/multi-bench-20261110/phi-branch-forward')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    names=('read','write','nested','mixed','ifelse','other_write')
    for name in names:
        project=args.out/f'{name}.sb3'
        write_sb3(project,fixture(512,name))
        expected,_=run(binary,project,'off',False)
        for tier in ('eager','hotspot'):
            for enabled in (False,True,None):
                actual,_=run(binary,project,tier,enabled)
                assert actual==expected,(name,tier,enabled,actual,expected)
        counts=[]
        for enabled in (False,True,None):
            _,ir=run(binary,project,'eager',enabled,True)
            (args.out/f'{name}-{str(int(enabled)) if enabled is not None else "default"}.ll').write_text(ir)
            counts.append(ir.count('call i64 @llvm.x86.sse2.cvttsd2si64('))
        assert counts[1]==counts[2],(name,counts)
        if name in ('read','other_write'):
            assert counts[1]<counts[0],(name,counts)
        else:
            assert counts[1]==counts[0],(name,counts)
        print('PASS',name,'output',expected,'conversion calls',counts,flush=True)
    print('PASS all second-IF PHI forwarding tests',flush=True)

if __name__=='__main__':main()
