#!/usr/bin/env python3
"""Safe recursive IF/IF-ELSE range proof following a numeric branch PHI.

The entire nested subtree must have read-only reporter conditions, no yield,
no unknown controls, and no mutation of the protected local scalar.
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
    g=ForEachGen(['i','temp','sum'],['values','output'])
    i=var('i')
    right=text('5') if kind=='mixed' else floor_(add(i,num(1)))
    init=S_ifelse(lt(i,num(size//2)),[S_set('temp',round_(i))],
                [S_set('temp',right)])
    t=var('temp')
    read=S_change('sum',item('values',t))
    if kind=='nested_if':
        a,b=[S_if(lt(t,num(size//3)),[read])],[S_change('sum',num(1))]
    elif kind=='nested_ifelse':
        a,b=[S_ifelse(lt(t,num(size//3)),[read],[read])],[read]
    elif kind=='nested_depth3':
        a,b=[S_if(lt(t,num(size//3)),[S_if(lt(t,num(size//4)),[read])])],[read]
    elif kind=='nested_other_write':
        a,b=[S_if(lt(t,num(size//3)),[S_set('sum',num(3)),read])],[read]
    elif kind=='nested_mutates':
        a,b=[S_if(lt(t,num(size//3)),[S_set('temp',text('5')),read])],[read]
    elif kind=='nested_repeat':
        a,b=[S_if(lt(t,num(size//3)),[S_repeat(num(1),[read])])],[read]
    elif kind=='mixed':
        a,b=[S_if(lt(t,num(size//3)),[read])],[read]
    elif kind=='nested_no_read':
        a,b=[S_if(lt(t,num(size//3)),[S_change('sum',num(3))])],[read]
    else:raise ValueError(kind)
    nextControl=S_ifelse(lt(t,num(size//2)),a,b)
    g.script([S_set('sum',num(0)),
              ('foreach','i',num(size),
               [S_set('temp',text('bad')),init,nextControl]),
              S_add('output',var('sum')),S_add('output',var('temp'))])
    return make_project(g.vars.keys(),g.lists.keys(),g,
                        {'values':[1]*(size+5)})

def run(binary:Path,project:Path,tier:str,nested:bool|None,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_PHI_RANGE_BRANCH_FORWARD='1',
        XYO_JIT_PHI_RANGE_IFELSE_FORWARD='1',
        XYO_JIT_PHI_INTEGER_RANGE='1',XYO_JIT_LAZY_TYPED_SSA='1',
        XYO_JIT_LOCAL_VAR_FORWARD='1',XYO_JIT_LIST_PROVEN_I32_INDEX='1',
        XYO_JIT_UNARY_INTEGER_RANGE='1',XYO_JIT_LAZY_MODE='off',
        XYO_PARFOR='0',XYO_GPU='0')
    if nested is not None:
        env['XYO_JIT_PHI_NESTED_BRANCH_FORWARD']=str(int(nested))
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',tier,
        '--print-list','output'],env=env,cwd=ROOT,
        text=True,capture_output=True,timeout=75)
    if p.returncode:
        raise RuntimeError((project.name,tier,nested,p.returncode,p.stderr[-900:]))
    return tuple(x.strip() for x in p.stdout.splitlines() if x.strip()),p.stderr

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
        default=ROOT/'build/multi-bench-20261111/phi-nested-forward')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    cases=('nested_if','nested_ifelse','nested_depth3','nested_other_write',
           'nested_mutates','nested_repeat','mixed','nested_no_read')
    for case in cases:
        project=args.out/f'{case}.sb3'
        write_sb3(project,fixture(512,case))
        expected,_=run(binary,project,'off',False)
        for jit in ('eager','hotspot'):
            for setting in (False,True,None):
                result,_=run(binary,project,jit,setting)
                assert result==expected,(case,jit,setting,expected,result)
        cvtt={}
        for setting in (False,True,None):
            _,ir=run(binary,project,'eager',setting,True)
            label='default' if setting is None else str(int(setting))
            (args.out/f'{case}-{label}.ll').write_text(ir)
            cvtt[label]=ir.count('call i64 @llvm.x86.sse2.cvttsd2si64(')
        assert cvtt['default']==cvtt['1'],(case,cvtt)
        if case in ('nested_if','nested_ifelse','nested_depth3','nested_other_write'):
            assert cvtt['1']<cvtt['0'],(case,cvtt)
        else:
            assert cvtt['1']==cvtt['0'],(case,cvtt)
        print('PASS',case,'expected',expected,'cvtt',cvtt,flush=True)
    print('PASS nested IF forwarding differential corpus')

if __name__=='__main__':main()
