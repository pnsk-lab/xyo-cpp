#!/usr/bin/env python3
"""Paired native Scratch benchmark for comparison inside proven for-each IF.

=0: original proof; =2: pure IF induction range; =1: plus known numeric compare.
"""
from __future__ import annotations
import argparse,os,random,json,statistics,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import S_add,S_call,S_change,S_delall,S_repeat,S_set,S_if,S_ifelse,days,item,lt,gt,eq,round_,floor_,make_project,mul,num,sub,var,write_sb3
from jit_list_proven_i32_index import ForEachGen


def project(n,kind):
    g=ForEachGen(['i','sum','t0','t1'],['values','times','checks'])
    header=g.define('bench',[],[S_set('sum',num(0))],warp=True)
    proc_id=g.blocks[header]['next']
    while g.blocks[proc_id]['next']:
        proc_id=g.blocks[proc_id]['next']
    loop=g.new('control_for_each',parent=proc_id,
               fields={'VARIABLE':g.field('VARIABLE','i')})
    g.blocks[proc_id]['next']=loop
    g.blocks[loop]['inputs']['VALUE']=g.emit_value(num(n),loop)
    i=var('i')
    if kind=='lt_round':
        body=[S_if(lt(round_(i),num(n//2)),[S_change('sum',item('values',i))])]
    elif kind=='gt_floor':
        body=[S_if(gt(floor_(i),num(n//2)),[S_change('sum',item('values',i))])]
    elif kind=='eq_round':
        body=[S_if(eq(round_(i),num(n//2)),[S_change('sum',item('values',i))])]
    elif kind=='if_else':
        body=[S_ifelse(lt(round_(i),num(n//2)),[S_change('sum',item('values',i))],[S_change('sum',num(1))])]
    elif kind=='nested':
        body=[S_if(gt(i,num(9)),[S_if(lt(round_(i),num(n//2)),[S_change('sum',item('values',i))])])]
    else:raise ValueError(kind)
    g.blocks[loop]['inputs']['SUBSTACK']=[2,g.chain(body,loop)]
    g.script([S_delall('times'),S_delall('checks'),S_call('bench'),
        S_repeat(num(3),[
            S_set('t0',days()),S_repeat(num(100),[S_call('bench')]),
            S_set('t1',days()),
            S_add('times',mul(sub(var('t1'),var('t0')),num(86400000))),
            S_add('checks',var('sum'))])])
    return make_project(g.vars.keys(),g.lists.keys(),g,
                        {'values':[1]*n})


def run(binary,project,mode):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_COMPARE_PROVEN_NUM=str(mode),XYO_JIT_UNARY_INTEGER_RANGE='1',
       XYO_JIT_LIST_PROVEN_I32_INDEX='1',XYO_JIT_LAZY_MODE='off',
       XYO_JIT_LLVM_REPEAT_HINT='0',XYO_PARFOR='0',XYO_GPU='0',XYO_DUMPVARS='json')
    p=subprocess.run(['taskset','-c','8',str(binary),'run',str(project),'--jit','eager'],
       cwd=ROOT,env=env,capture_output=True,text=True,timeout=100)
    if p.returncode:raise RuntimeError((project.name,mode,p.returncode,p.stderr[-900:]))
    raw=p.stdout
    data=json.loads(raw[raw.index('{'):raw.rindex('}')+1])['Bench']
    times=[float(x) for x in data['times']]
    checks=[str(x) for x in data['checks']]
    assert len(times)==3 and len(set(checks))==1 and all(0<=t<120000 for t in times) and statistics.median(times)>0,(project.name,mode,times,checks)
    return {'median_ms':statistics.median(times),'times_ms':times,'checksum':checks[0]}


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,
                    default=ROOT/'build/multi-bench-20261110/proven-compare/benchmark.json')
    ap.add_argument('--iterations',type=int,default=150000)
    ap.add_argument('--rounds',type=int,default=8)
    ap.add_argument('--modes',type=int,nargs='+',default=[0,3],
                    help='JIT comparison-range configurations to compare')
    args=ap.parse_args();out=args.out.parent;out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    cases=('lt_round','gt_floor','eq_round','if_else','nested')
    for kind in cases:write_sb3(out/f'bench-{kind}.sb3',project(args.iterations,kind))
    rng=random.Random(20261110)
    rows=[]
    for i in range(args.rounds):
        for kind in cases:
            schedule=list(args.modes)
            rng.shuffle(schedule)
            for mode in schedule:
                v=run(binary,out/f'bench-{kind}.sb3',mode)
                row={'round':i,'case':kind,'mode':mode,**v}
                rows.append(row)
                args.out.write_text(json.dumps(rows,indent=2)+'\n')
                print('RUN',i+1,kind,mode,'ms',round(v['median_ms'],3),'checksum',v['checksum'],flush=True)
    for kind in cases:
        cases_rows=[r for r in rows if r['case']==kind]
        by={m:[r for r in cases_rows if r['mode']==m] for m in args.modes}
        assert len({r['checksum'] for r in cases_rows})==1,kind
        for a,b in ((mode,args.modes[0]) for mode in args.modes[1:]):
            ratios=[x['median_ms']/y['median_ms'] for x,y in zip(by[b],by[a])]
            print('SUMMARY',kind,f'{a}/{b}',round(statistics.geometric_mean(ratios),5),
                  'wins',sum(v>1 for v in ratios),'/',len(ratios),
                  'median_ms',round(statistics.median(r['median_ms'] for r in by[a]),4),flush=True)

if __name__=='__main__':main()
