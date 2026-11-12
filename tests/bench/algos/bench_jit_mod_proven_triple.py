#!/usr/bin/env python3
"""Paired pinned Scratch benchmark for 1..3m proven modulo extension."""
from __future__ import annotations
import argparse,json,os,statistics,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tests/bench/algos'))
from gen_algo_bench import write_sb3
from bench_jit_mod_proven_wrap import fixture

def run(binary,project,enabled):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_MOD_PROVEN_TRIPLE_WRAP=str(int(enabled)),
       XYO_JIT_MOD_PROVEN_SMALL_WRAP='1',XYO_JIT_MOD_PROVEN_I32_WRAP='1',
       XYO_DUMPVARS='json',XYO_JIT_LAZY_MODE='off',XYO_PARFOR='0',XYO_GPU='0')
    p=subprocess.run(['taskset','-c','8',str(binary),'run',str(project),
                      '--jit','eager'],cwd=ROOT,env=env,
                     capture_output=True,text=True,timeout=160)
    if p.returncode:
        raise RuntimeError((project.name,enabled,p.returncode,p.stderr[-800:]))
    result=json.loads(p.stdout[p.stdout.index('{'):p.stdout.rindex('}')+1])['Bench']
    samples=[float(x) for x in result['times']]
    checks=[str(x) for x in result['checks']]
    assert len(samples)==3 and len(set(checks))==1 and statistics.median(samples)>0,(samples,checks)
    return {'ms':statistics.median(samples),'samples':samples,'checksum':checks[0]}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=ROOT/'build/multi-bench-20261111/proven-triple-mod/benchmark.json')
    ap.add_argument('--pairs',type=int,default=8)
    ap.add_argument('--scale',type=int,default=25)
    ap.add_argument('--cases',default='31,63,127,255,257,511,1023,4095,65535')
    args=ap.parse_args()
    args.out.parent.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    divisors=tuple(int(x) for x in args.cases.split(','))
    for m in divisors:
        n=3*m
        reps=max(1,int(args.scale*min(4000,300000/n)))
        write_sb3(args.out.parent/f'bench-m{m}.sb3',fixture(m,n,reps))
    rows=[]
    for pair in range(args.pairs):
        for m in divisors:
            for enabled in ((0,1) if pair%2==0 else (1,0)):
                value=run(binary,args.out.parent/f'bench-m{m}.sb3',enabled)
                row={'pair':pair+1,'m':m,'enabled':enabled,**value}
                rows.append(row)
                args.out.write_text(json.dumps(rows,indent=2)+'\n')
                print('RUN',pair+1,m,enabled,round(value['ms'],4),value['checksum'],flush=True)
    for m in divisors:
        old=[r for r in rows if r['m']==m and r['enabled']==0]
        new=[r for r in rows if r['m']==m and r['enabled']==1]
        assert len({r['checksum'] for r in old+new})==1,m
        ratios=[a['ms']/b['ms'] for a,b in zip(old,new)]
        print('SUMMARY',m,'geomean',round(statistics.geometric_mean(ratios),6),
              'wins',sum(r>1 for r in ratios),'/',len(ratios),
              'median_old',statistics.median(r['ms'] for r in old),
              'median_new',statistics.median(r['ms'] for r in new),flush=True)

if __name__=='__main__':main()
