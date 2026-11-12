#!/usr/bin/env python3
"""Paired Scratch workload for exact-integer interval joining across IF/ELSE.

The test invokes actual Scratch warp procedures and measures inside-project
elapsed time, excluding load/JIT startup. It checks unchanged final values.
"""
from __future__ import annotations
import argparse,json,os,random,statistics,subprocess,sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_call,S_change,S_delall,S_ifelse,S_repeat,S_set,
    add,days,div,floor_,item,lt,make_project,mul,num,round_,sub,text,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen


def project(n:int,case:str,repeats:int):
    g=ForEachGen(['i','temp','sum','start','finish'],['values','times','checks'])
    i=var('i')
    if case=='both_integer':
        left=round_(i);right=floor_(add(i,num(1)))
    elif case=='same_integer':
        left=round_(i);right=floor_(i)
    elif case=='mixed_string':
        left=round_(i);right=text('5')
    elif case=='fractional':
        left=round_(i);right=div(i,num(2))
    elif case=='two_negative':
        left=round_(sub(i,num(2)));right=floor_(sub(i,num(1)))
    else:raise ValueError(case)
    body=[S_set('temp',text('bad')),
          S_ifelse(lt(i,num(n//2)),[S_set('temp',left)],[S_set('temp',right)]),
          S_change('sum',item('values',var('temp')))]
    g.define('work',[],[S_set('sum',num(0)),('foreach','i',num(n),body)],warp=True)
    g.script([S_delall('times'),S_delall('checks'),S_call('work'),
        S_repeat(num(3),[
            S_set('start',days()),S_repeat(num(repeats),[S_call('work')]),
            S_set('finish',days()),
            S_add('times',mul(sub(var('finish'),var('start')),num(86400000))),
            S_add('checks',var('sum')),
        ])])
    return make_project(g.vars.keys(),g.lists.keys(),g,{'values':[1]*(n+3)})


def run(binary:Path,fixture:Path,mode:int):
    env={key:val for key,val in os.environ.items() if not key.startswith('XYO_')}
    env.update(XYO_JIT_PHI_INTEGER_RANGE='1' if mode else '0',
       XYO_JIT_LAZY_TYPED_SSA='1',XYO_JIT_LOCAL_VAR_FORWARD='1',
       XYO_JIT_UNARY_INTEGER_RANGE='1',XYO_JIT_LIST_PROVEN_I32_INDEX='1',
       XYO_JIT_COMPARE_PROVEN_NUM='3',XYO_JIT_LAZY_MODE='off',
       XYO_JIT_LLVM_REPEAT_HINT='0',XYO_PARFOR='0',XYO_GPU='0',
       XYO_DUMPVARS='json')
    p=subprocess.run(['taskset','-c','8',str(binary),'run',str(fixture),
       '--jit','eager'],env=env,cwd=ROOT,text=True,capture_output=True,
       timeout=120)
    if p.returncode:raise RuntimeError((fixture.name,mode,p.returncode,p.stderr[-900:]))
    raw=p.stdout
    data=json.loads(raw[raw.index('{'):raw.rindex('}')+1])['Bench']
    values=[float(s) for s in data['times']]
    checks=[str(s) for s in data['checks']]
    assert len(values)==3 and len(set(checks))==1 and all(0<=t<120000 for t in values) and statistics.median(values)>0,(fixture.name,mode,values,checks)
    return {'median_ms':statistics.median(values),'samples_ms':values,'checksum':checks[0]}


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=ROOT/'build/multi-bench-20261110/phi-integer-range/benchmark.json')
    ap.add_argument('--size',type=int,default=80000)
    ap.add_argument('--repeats',type=int,default=70)
    ap.add_argument('--rounds',type=int,default=8)
    args=ap.parse_args()
    output=args.out;output.parent.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    cases=('both_integer','same_integer','mixed_string','fractional','two_negative')
    for case in cases:
        write_sb3(output.parent/f'bench-{case}.sb3',project(args.size,case,args.repeats))
    rng=random.Random(20261111);rows=[]
    for i in range(args.rounds):
        for case in cases:
            modes=[0,1] if i%2==0 else [1,0]
            for mode in modes:
                r=run(binary,output.parent/f'bench-{case}.sb3',mode)
                entry=dict(round=i+1,case=case,mode=mode,**r)
                rows.append(entry)
                output.write_text(json.dumps(rows,indent=2)+'\n')
                print('RUN',i+1,case,mode,round(r['median_ms'],4),'ms',r['checksum'],flush=True)
    for case in cases:
        pairs={m:[r for r in rows if r['case']==case and r['mode']==m]
               for m in (0,1)}
        assert len({r['checksum'] for m in (0,1) for r in pairs[m]})==1,case
        for a,b in ((1,0),):
            ratios=[x['median_ms']/y['median_ms']
                    for x,y in zip(pairs[b],pairs[a])]
            print('SUMMARY',case,f'{a}/{b}',
                  'geomean',round(statistics.geometric_mean(ratios),6),
                  'wins',sum(x>1 for x in ratios),'/',len(ratios),
                  'median_a',statistics.median(r['median_ms'] for r in pairs[a]),
                  'median_b',statistics.median(r['median_ms'] for r in pairs[b]),flush=True)

if __name__=='__main__':main()
