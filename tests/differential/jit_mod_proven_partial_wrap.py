#!/usr/bin/env python3
"""Confirm integer-SSA remainder specialization on finer exact intervals.

Only static interval proofs can eliminate modulo compare/select operations.
Run actual Scratch with interpreter/eager/hotspot and compare optimized IR.
"""
from __future__ import annotations
import argparse, os, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_change,S_set,add,make_project,mod,num,
                            sub,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen

def fixture(m, variant):
    n={'low':m-1, 'high':m, 'high_endpoint':m,
       'cross':2*m-1, 'full':2*m,
       'outside':2*m+1, 'negative':m}[variant]
    g=ForEachGen(['i','sum'],['output'])
    v=var('i')
    if variant=='high':v=add(v,num(m-1))
    if variant=='high_endpoint':v=add(v,num(m))
    if variant=='negative':v=sub(num(1),v)
    g.script([S_set('sum',num(0)),
              ('foreach','i',num(n),[S_change('sum',mod(v,num(m)))]),
              S_add('output',var('sum')),S_add('output',var('i'))])
    return make_project(g.vars.keys(),g.lists.keys(),g)

def run(binary,project,tier,flag,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_MOD_PROVEN_SMALL_WRAP='1',
       XYO_JIT_MOD_PROVEN_I32_WRAP='1',
       XYO_JIT_LAZY_MODE='off',XYO_GPU='0',XYO_PARFOR='0')
    if flag is not None:
        env['XYO_JIT_MOD_PROVEN_PARTIAL_WRAP']=str(int(flag))
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',tier,
                     '--print-list','output'],env=env,cwd=ROOT,
                     capture_output=True,text=True,timeout=90)
    if p.returncode:
        raise RuntimeError((project.name,tier,flag,p.returncode,p.stderr[-900:]))
    return tuple(x.strip() for x in p.stdout.splitlines() if x.strip()),p.stderr

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
      default=ROOT/'build/multi-bench-20261111/proven-partial-mod')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    b=args.binary.resolve()
    for m in (31,63,127,255,511,1023,2047):
        for variant in ('low','high','high_endpoint','cross','full','outside','negative'):
            path=args.out/f'mod-{m}-{variant}.sb3'
            write_sb3(path,fixture(m,variant))
            expected,_=run(b,path,'off',False)
            for tier in ('eager','hotspot'):
                for flag in (False,True,None):
                    actual,_=run(b,path,tier,flag)
                    assert actual==expected,(m,variant,tier,flag,actual,expected)
            if m in (31,255,1023):
                ir={}
                for flag in (False,True,None):
                    _,result=run(b,path,'eager',flag,True)
                    label='default' if flag is None else str(int(flag))
                    (args.out/f'{m}-{variant}-{label}.ll').write_text(result)
                    ir[label]=result
                # The experiment is deliberately OFF by default.
                # Embedded constant pointer addresses vary per invocation,
                # so compare only instruction feature counts.
                features=lambda s:(s.count('select i1'),
                    s.count('icmp eq i32'),s.count('icmp uge i32'),
                    s.count('urem '))
                assert features(ir['default'])==features(ir['0']),(m,variant)
                print('IR',m,variant,
                    'off',features(ir['0']),'on',features(ir['1']),flush=True)
        print('PASS',m,'7 variants, interpreter/eager/hotspot',flush=True)
    print('PASS all bounded remainder interval differential tests',flush=True)

if __name__=='__main__':main()
