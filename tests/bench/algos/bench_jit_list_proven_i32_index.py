#!/usr/bin/env python3
"""Paired Scratch for-each/list-read benchmark for proven 1-based i32 indices.

The measurements are taken by the Scratch project using daysSince2000;
performance and checksums are exported as ordinary Scratch lists.
"""
import argparse,json,os,random,statistics,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import S_add,S_call,S_change,S_delall,S_repeat,S_set,add,days,item,length,make_project,mul,num,sub,var,write_sb3
from jit_list_proven_i32_index import ForEachGen


def project(count,kind):
    g=ForEachGen(['i','sum','t0','t1'],['values','times','checks'])
    head=g.define('bench',[],[S_set('sum',num(0))],warp=True)
    tail=g.blocks[head]['next']
    while g.blocks[tail]['next']:
        tail=g.blocks[tail]['next']
    loop=g.new('control_for_each',parent=tail,
               fields={'VARIABLE':g.field('VARIABLE','i')})
    g.blocks[tail]['next']=loop
    g.blocks[loop]['inputs']['VALUE']=g.emit_value(num(count),loop)
    index=var('i') if kind=='direct' else add(var('i'),num(1))
    body=[S_change('sum',item('values',index))]
    if kind=='length':body.insert(0,S_change('sum',length('values')))
    g.blocks[loop]['inputs']['SUBSTACK']=[2,g.chain(body,loop)]
    g.script([
      S_delall('times'),S_delall('checks'),S_call('bench'),
      S_repeat(num(3),[
        S_set('t0',days()),
        S_repeat(num(100),[S_call('bench')]),
        S_set('t1',days()),
        S_add('times',mul(sub(var('t1'),var('t0')),num(86400000))),
        S_add('checks',var('sum')),
      ])
    ])
    return make_project(g.vars.keys(),g.lists.keys(),g,
                        {'values':[1]*count})


def evaluate(binary,path,mode):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_LIST_PROVEN_I32_INDEX=str(int(mode)),
               XYO_JIT_MODE='eager',XYO_JIT_LAZY_MODE='off',
               XYO_JIT_LLVM_REPEAT_HINT='0',XYO_GPU='0',XYO_PARFOR='0',
               XYO_DUMPVARS='json')
    cmd=['taskset','-c','8',str(binary),'run',str(path),'--jit','eager']
    p=subprocess.run(cmd,cwd=ROOT,env=env,text=True,capture_output=True,timeout=90)
    if p.returncode:raise RuntimeError((path.name,mode,p.returncode,p.stderr[-700:]))
    raw=p.stdout; data=json.loads(raw[raw.index('{'):raw.rindex('}')+1])['Bench']
    times=[float(x) for x in data['times']]
    checks=[str(x) for x in data['checks']]
    assert len(times)==3 and len(set(checks))==1 and all(0<x<120000 for x in times), (path.name,mode,times,checks)
    return {'median_ms':statistics.median(times),'samples_ms':times,'checksum':checks[0]}


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=ROOT/'build/multi-bench-20261110/list-proven-i32/benchmark.json')
    ap.add_argument('--iterations',type=int,default=120000)
    ap.add_argument('--blocks',type=int,default=6)
    args=ap.parse_args()
    binary=args.binary.resolve();args.out.parent.mkdir(parents=True,exist_ok=True)
    kinds=('direct','compound','length')
    for kind in kinds:write_sb3(args.out.parent/f'bench-{kind}.sb3',project(args.iterations,kind))
    rng=random.Random(20261110);results=[]
    for block in range(args.blocks):
        schedule=[(kind,flag) for kind in kinds for flag in (0,1)]
        rng.shuffle(schedule)
        for kind,flag in schedule:
            v=evaluate(binary,args.out.parent/f'bench-{kind}.sb3',flag)
            row=dict(block=block,case=kind,enabled=flag,**v)
            results.append(row)
            args.out.write_text(json.dumps(results,indent=2)+'\n')
            print('RUN',block,kind,flag,v,flush=True)
    for kind in kinds:
        off=[r for r in results if r['case']==kind and r['enabled']==0]
        on=[r for r in results if r['case']==kind and r['enabled']==1]
        assert len({r['checksum'] for r in off+on})==1,(kind,'checksum mismatch')
        ratios=[o['median_ms']/n['median_ms'] for o,n in zip(off,on)]
        print('SUMMARY',kind,'ratio',statistics.geometric_mean(ratios),'wins',sum(r>1 for r in ratios),'off_med',statistics.median(o['median_ms'] for o in off),'on_med',statistics.median(o['median_ms'] for o in on),flush=True)

if __name__=='__main__':main()
