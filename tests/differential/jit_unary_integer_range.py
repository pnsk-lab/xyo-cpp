#!/usr/bin/env python3
"""Proven bounded integer SSA range propagation through Scratch unary ops.

No rewrite is performed on the numeric reporter. Its exact integer range is
supplied to later SSA consumers, and LLVM decides whether to simplify the IR.
"""
from __future__ import annotations
import argparse,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from gen_algo_bench import S_add,S_change,S_set,add,div,item,num,op,round_,floor_,sub,text,var,make_project,write_sb3
from jit_list_proven_i32_index import ForEachGen


def mathop(name,x):return op('operator_mathop',{'OPERATOR':name},NUM=x)


def expr(mode):
    i=var('i')
    return {
      'round':round_(i),
      'floor':floor_(i),
      'ceil':mathop('ceiling',i),
      'abs':mathop('abs',sub(i,num(1200))),
      'abs_positive':mathop('abs',i),
      'round_sub':round_(sub(i,num(2))),
      'floor_div':floor_(div(i,num(2))),
      'round_half':round_(add(i,num(0.5))),
      'floor_negative':mathop('floor',sub(i,num(700))),
      'ceiling_fraction':mathop('ceiling',div(i,num(3))),
      'ceiling_pow2':mathop('ceiling',div(i,num(2))),
      'floor_pow2':mathop('floor',div(add(i,num(1)),num(2))),
      'ceiling_pow256':mathop('ceiling',div(i,num(256))),
    }[mode]


def fixture(count,mode):
    g=ForEachGen(['i','sum'],['values','output'])
    body=[S_change('sum',item('values',expr(mode)))]
    g.script([S_set('sum',num(0)),('foreach','i',num(count),body),
              S_add('output',var('sum')),S_add('output',var('i'))])
    values=list(range(1,2001))
    return make_project(g.vars.keys(),g.lists.keys(),g,{'values':values})


def run(binary,project,jit,flag,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_LIST_PROVEN_I32_INDEX='1',
               XYO_JIT_LAZY_MODE='off',XYO_PARFOR='0',XYO_GPU='0')
    if flag is not None:env['XYO_JIT_UNARY_INTEGER_RANGE']=str(int(flag))
    if dump:env['XYO_JIT_DUMP2']='1'
    p=subprocess.run([str(binary),'run',str(project),'--jit',jit,
                      '--print-list','output'],cwd=ROOT,env=env,text=True,
                     capture_output=True,timeout=60)
    if p.returncode:raise RuntimeError((project,jit,flag,p.returncode,p.stderr[-700:]))
    vals=tuple(line.strip() for line in p.stdout.splitlines() if line.strip())
    return vals,p.stderr


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=ROOT/'build/multi-bench-20261110/unary-int-range')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    for kind in ('round','floor','ceil','abs','abs_positive','round_sub','floor_div','round_half','floor_negative','ceiling_fraction','ceiling_pow2','floor_pow2','ceiling_pow256'):
        project=args.out/f'{kind}.sb3';write_sb3(project,fixture(500,kind))
        expected,_=run(binary,project,'off',False)
        for jit in ('eager','hotspot'):
            for flag in (False,True,None):
                got,_=run(binary,project,jit,flag)
                assert expected==got,(kind,jit,flag,expected,got)
        print('PASS',kind,expected,flush=True)
    conversion_calls={}
    for kind in ('round','floor','ceil','abs','floor_div','ceiling_fraction','ceiling_pow2','floor_pow2','ceiling_pow256'):
        conversion_calls[kind]={}
        for flag in (0,1,None):
            _,ir=run(binary,args.out/f'{kind}.sb3','eager',flag,True)
            label='default' if flag is None else str(flag)
            (args.out/f'{kind}-{label}.ll').write_text(ir)
            conversion_calls[kind][label]=ir.count('call i64 @llvm.x86.sse2.cvttsd2si64(')
            print('IR',kind,label,'cvtt_calls',conversion_calls[kind][label],flush=True)
    for kind in ('round','floor','ceil'):
        assert conversion_calls[kind]['0'] > conversion_calls[kind]['1'] == conversion_calls[kind]['default'],(kind,conversion_calls[kind])
    assert conversion_calls['abs']['0'] == conversion_calls['abs']['default'],conversion_calls['abs']
    for kind in ('floor_div','ceiling_fraction','ceiling_pow2','floor_pow2','ceiling_pow256'):
        assert conversion_calls[kind]['0'] == conversion_calls[kind]['1'] == conversion_calls[kind]['default'], (kind,conversion_calls[kind])
    print('PASS all unary integer range differential cases and IR assertions')

if __name__=='__main__':main()
