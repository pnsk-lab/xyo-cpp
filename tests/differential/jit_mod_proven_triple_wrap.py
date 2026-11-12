#!/usr/bin/env python3
"""3*m bounded integer modulo lowering: positive exact SSA proof only."""
from __future__ import annotations
import argparse,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_change,S_set,add,make_project,mod,
                            num,sub,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen

def fixture(m,variant):
    n = {
        'triple':3*m, 'triple_minus':3*m-1,
        'triple_plus':3*m+1, 'double':2*m,
        'upper':2*m, 'negative':3*m
    }[variant]
    g=ForEachGen(['i','sum'],['output'])
    x=var('i')
    if variant=='upper':x=add(x,num(m))
    if variant=='negative':x=sub(num(1),x)
    g.script([S_set('sum',num(0)),
              ('foreach','i',num(n),[S_change('sum',mod(x,num(m)))]),
              S_add('output',var('sum')),S_add('output',var('i'))])
    return make_project(g.vars.keys(),g.lists.keys(),g)

def run(binary,project,tier,flag,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_MOD_PROVEN_SMALL_WRAP='1',
        XYO_JIT_MOD_PROVEN_I32_WRAP='1',XYO_GPU='0',XYO_PARFOR='0',
        XYO_JIT_LAZY_MODE='off')
    if flag is not None:env['XYO_JIT_MOD_PROVEN_TRIPLE_WRAP']=str(int(flag))
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',tier,
                      '--print-list','output'],cwd=ROOT,env=env,
                     capture_output=True,text=True,timeout=90)
    if p.returncode:raise RuntimeError((project.name,tier,flag,p.returncode,p.stderr[-650:]))
    return tuple(s.strip() for s in p.stdout.splitlines() if s.strip()),p.stderr

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
        default=ROOT/'build/multi-bench-20261111/proven-triple-mod')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    for m in (31,63,127,254,255,256,257,511,1000,1023,2047,4095,65535,65536):
        for variant in ('triple','triple_minus','triple_plus',
                        'double','upper','negative'):
            project=args.out/f'm{m}-{variant}.sb3'
            write_sb3(project,fixture(m,variant))
            expected,_=run(binary,project,'off',False)
            for tier in ('eager','hotspot'):
                for flag in (False,True,None):
                    got,_=run(binary,project,tier,flag)
                    assert got==expected,(m,variant,tier,flag,expected,got)
            if m in (31,255,1023,4095,65535) and variant in ('triple','triple_minus','triple_plus','double','upper'):
                counts={}
                for flag in (False,True,None):
                    _,ir=run(binary,project,'eager',flag,True)
                    label='default' if flag is None else str(int(flag))
                    (args.out/f'm{m}-{variant}-{label}.ll').write_text(ir)
                    counts[label]=(ir.count('urem '),ir.count('select i1'))
                assert counts['default']==counts['1'],(m,variant,counts)
                if m <= 4095 and variant in ('triple','triple_minus','upper'):
                    assert counts['1'][0]<counts['0'][0],(m,variant,counts)
                else:assert counts['1']==counts['0'],(m,variant,counts)
                print('IR',m,variant,counts,flush=True)
        print('PASS',m,'6 corpus variants',flush=True)
    print('PASS triple bounded remainder differential suite',flush=True)

if __name__=='__main__':main()
