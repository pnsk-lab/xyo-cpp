#!/usr/bin/env python3
"""Check path-local numeric PHI forwarding into both arms of a second IF-ELSE.

The first IF-ELSE provides a proven numeric PHI. The second is tested with
both/one/no eligible paths; unknown writes and mixed numeric/string PHIs
must not acquire an unjustified integer interval.
"""
from __future__ import annotations
import argparse,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_change,S_if,S_ifelse,S_repeat,S_set,
    add,floor_,item,lt,make_project,num,round_,text,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen


def fixture(size:int,kind:str):
    gen=ForEachGen(['i','temp','sum'],['values','output'])
    i=var('i')
    alternate=text('5') if kind=='mixed' else floor_(add(i,num(1)))
    first=S_ifelse(lt(i,num(size//2)),
          [S_set('temp',round_(i))],[S_set('temp',alternate)])
    read=S_change('sum',item('values',var('temp')))
    if kind=='both': a,b=[read],[read]
    elif kind=='true': a,b=[read],[S_change('sum',num(1))]
    elif kind=='false': a,b=[S_change('sum',num(1))],[read]
    elif kind=='other_write':
        a,b=[S_set('sum',num(3)),read],[S_set('sum',num(2)),read]
    elif kind=='true_mutates':
        a,b=[S_set('temp',text('5')),read],[read]
    elif kind=='both_mutate':
        a,b=([S_set('temp',text('5')),read],
             [S_set('temp',text('4')),read])
    elif kind=='mixed': a,b=[read],[read]
    elif kind=='nested': a,b=[S_repeat(num(1),[read])],[read]
    elif kind=='no_read':
        a,b=[S_change('sum',num(3))],[S_change('sum',num(-1))]
    else:raise ValueError(kind)
    second=S_ifelse(lt(var('temp'),num(size//2)),a,b)
    gen.script([S_set('sum',num(0)),
                ('foreach','i',num(size),
                 [S_set('temp',text('wrong')),first,second]),
                S_add('output',var('sum')),
                S_add('output',var('temp'))])
    return make_project(gen.vars.keys(),gen.lists.keys(),gen,
                        {'values':[1]*(size+5)})


def run(binary,project,tier,flag,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_PHI_RANGE_BRANCH_FORWARD='1',
        XYO_JIT_PHI_INTEGER_RANGE='1',XYO_JIT_LAZY_TYPED_SSA='1',
        XYO_JIT_LOCAL_VAR_FORWARD='1',XYO_JIT_LIST_PROVEN_I32_INDEX='1',
        XYO_JIT_UNARY_INTEGER_RANGE='1',XYO_JIT_LAZY_MODE='off',
        XYO_PARFOR='0',XYO_GPU='0')
    if flag is not None:env['XYO_JIT_PHI_RANGE_IFELSE_FORWARD']=str(int(flag))
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',tier,
                      '--print-list','output'],cwd=ROOT,env=env,
                     text=True,capture_output=True,timeout=75)
    if p.returncode:
        raise RuntimeError((project.name,tier,flag,p.returncode,p.stderr[-950:]))
    return tuple(x.strip() for x in p.stdout.splitlines() if x.strip()),p.stderr


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--out',type=Path,
      default=ROOT/'build/multi-bench-20261111/phi-ifelse-forward')
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    kinds=('both','true','false','other_write','true_mutates','both_mutate',
           'mixed','nested','no_read')
    for kind in kinds:
        project=args.out/f'{kind}.sb3';write_sb3(project,fixture(512,kind))
        expected,_=run(binary,project,'off',False)
        for tier in ('eager','hotspot'):
            for flag in (False,True,None):
                got,_=run(binary,project,tier,flag)
                assert got==expected,(kind,tier,flag,expected,got)
        counts={}
        for flag in (False,True,None):
            _,ir=run(binary,project,'eager',flag,True)
            label='default' if flag is None else str(int(flag))
            (args.out/f'{kind}-{label}.ll').write_text(ir)
            counts[label]=ir.count('call i64 @llvm.x86.sse2.cvttsd2si64(')
        assert counts['default']==counts['1'],(kind,counts)
        if kind in ('both','true','false','other_write','true_mutates'):
            assert counts['1']<counts['0'],(kind,counts)
        else:
            assert counts['1']==counts['0'],(kind,counts)
        print('PASS',kind,'output',expected,'cvtt',counts,flush=True)
    print('PASS all IF-ELSE branch-local PHI forwarding tests')

if __name__=='__main__':main()
