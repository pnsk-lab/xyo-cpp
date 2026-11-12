#!/usr/bin/env python3
"""Paired in-project timing for forwarding an integer-interval PHI into IF.

Runs without XYO_JIT_IF_PHI; the narrow branch extension enables itself
only when there is a prior proven integer SSA value actually read in the IF.
"""
from __future__ import annotations
import argparse,json,os,statistics,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import (S_add,S_call,S_change,S_delall,S_if,S_ifelse,
    S_repeat,S_set,add,days,floor_,item,lt,make_project,mul,num,
    round_,sub,text,var,write_sb3)
from jit_list_proven_i32_index import ForEachGen


def project(n:int,kind:str,repetitions:int):
    g=ForEachGen(['i','temp','sum','start','end'],['values','times','checks'])
    i=var('i')
    other=text('5') if kind=='mixed' else floor_(add(i,num(1)))
    first=S_ifelse(lt(i,num(n//2)),[S_set('temp',round_(i))],
                   [S_set('temp',other)])
    read=S_change('sum',item('values',var('temp')))
    if kind=='read':
        nextIf=S_if(lt(var('temp'),num(n//2)),[read])
    elif kind=='other_write':
        nextIf=S_if(lt(var('temp'),num(n//2)),
                    [S_set('sum',num(1)),read])
    elif kind=='write':
        nextIf=S_if(lt(var('temp'),num(n//2)),
                    [S_set('temp',text('5')),read])
    elif kind=='mixed':
        nextIf=S_if(lt(var('temp'),num(n//2)),[read])
    else:raise ValueError(kind)
    g.define('work',[],[S_set('sum',num(0)),
        ('foreach','i',num(n),[S_set('temp',text('bad')),first,nextIf])],
        warp=True)
    g.script([S_delall('times'),S_delall('checks'),S_call('work'),
        S_repeat(num(3),[
            S_set('start',days()),
            S_repeat(num(repetitions),[S_call('work')]),
            S_set('end',days()),
            S_add('times',mul(sub(var('end'),var('start')),num(86400000))),
            S_add('checks',var('sum')),
        ])])
    return make_project(g.vars.keys(),g.lists.keys(),g,{'values':[1]*(n+5)})


def measure(binary:Path,project:Path,enabled:bool):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_PHI_RANGE_BRANCH_FORWARD=str(int(enabled)),
        XYO_JIT_PHI_INTEGER_RANGE='1',
        XYO_JIT_LAZY_TYPED_SSA='1',XYO_JIT_LOCAL_VAR_FORWARD='1',
        XYO_JIT_LIST_PROVEN_I32_INDEX='1',XYO_JIT_UNARY_INTEGER_RANGE='1',
        XYO_JIT_LAZY_MODE='off',XYO_PARFOR='0',XYO_GPU='0',
        XYO_DUMPVARS='json')
    p=subprocess.run(['taskset','-c','8',str(binary),'run',str(project),
                     '--jit','eager'],env=env,cwd=ROOT,
                     capture_output=True,text=True,timeout=120)
    if p.returncode:
        raise RuntimeError((project.name,enabled,p.returncode,p.stderr[-900:]))
    out=p.stdout
    state=json.loads(out[out.index('{'):out.rindex('}')+1])['Bench']
    samples=[float(x) for x in state['times']]
    checks=[str(x) for x in state['checks']]
    assert (len(samples)==3 and len(set(checks))==1 and
            all(0<=t<120000 for t in samples) and
            statistics.median(samples)>0), (project.name,samples,checks)
    return {'duration_ms':statistics.median(samples),
            'samples_ms':samples,'checksum':checks[0]}


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
        default=ROOT/'build/multi-bench-20261110/phi-branch-forward/benchmark.json')
    ap.add_argument('--size',type=int,default=100000)
    ap.add_argument('--repetitions',type=int,default=100)
    ap.add_argument('--pairs',type=int,default=8)
    args=ap.parse_args();args.out.parent.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve();kinds=('read','other_write','write','mixed')
    for kind in kinds:
        write_sb3(args.out.parent/f'bench-{kind}.sb3',
                  project(args.size,kind,args.repetitions))
    rows=[]
    for idx in range(args.pairs):
        for kind in kinds:
            modes=(0,1) if idx%2==0 else (1,0)
            for enabled in modes:
                result=measure(binary,args.out.parent/f'bench-{kind}.sb3',
                               bool(enabled))
                row={'pair':idx+1,'case':kind,'enabled':enabled,**result}
                rows.append(row)
                args.out.write_text(json.dumps(rows,indent=2)+'\n')
                print('RUN',idx+1,kind,enabled,
                      round(result['duration_ms'],4),flush=True)
    for kind in kinds:
        old=[r for r in rows if r['case']==kind and r['enabled']==0]
        new=[r for r in rows if r['case']==kind and r['enabled']==1]
        assert len({r['checksum'] for r in old+new})==1,kind
        ratios=[a['duration_ms']/b['duration_ms'] for a,b in zip(old,new)]
        print('SUMMARY',kind,'geomean',round(statistics.geometric_mean(ratios),6),
              'wins',sum(x>1 for x in ratios),'/',len(ratios),
              'off',statistics.median(r['duration_ms'] for r in old),
              'on',statistics.median(r['duration_ms'] for r in new),flush=True)

if __name__=='__main__':main()
