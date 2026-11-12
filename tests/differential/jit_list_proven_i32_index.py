#!/usr/bin/env python3
"""Differential and IR tests for proven integer indexing through a read-only for-each.

The optimization never guesses a list's element type or length. The only
additional fact is that the induction variable is a positive exact integer
within the original for-each's proven finite range.
"""
from __future__ import annotations
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/bench/algos'))
from gen_algo_bench import Gen,S_add,S_change,S_set,add,item,length,make_project,num,text,var,write_sb3

class ForEachGen(Gen):
    def emit_stmt(self,stmt,parent=None):
        if stmt[0]=='foreach':
            _,index,bound,body=stmt
            bid=self.new('control_for_each',parent=parent,
                         fields={'VARIABLE':self.field('VARIABLE',index)})
            self.blocks[bid]['inputs']['VALUE']=self.emit_value(bound,bid)
            body_head=self.chain(body,bid)
            if body_head:self.blocks[bid]['inputs']['SUBSTACK']=[2,body_head]
            return bid
        return super().emit_stmt(stmt,parent)


def fixture(count, list_values, mode='direct'):
    g=ForEachGen(['i','sum','limit'],['values','output'])
    idx=var('i') if mode=='direct' else add(var('i'),num(1))
    body=[S_change('sum',item('values',idx))]
    if mode=='length': body=[S_change('sum',length('values')),
                            S_change('sum',item('values',var('i')))]
    if mode=='last': body=[S_change('sum',item('values',text('last'))),
                          S_change('sum',item('values',var('i')))]
    if mode=='index_write': body=[S_set('i',num(-2)),
                                 S_change('sum',item('values',var('i')))]
    g.script([
        S_set('sum',num(0)), S_set('limit',num(count)),
        ('foreach','i',var('limit') if mode=='dynamic' else num(count),body),
        S_add('output',var('sum')),S_add('output',var('i')),
    ])
    return make_project(g.vars.keys(),g.lists.keys(),g,{'values':list_values})


def run(binary,project,jit,flag,dump=False):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_LAZY_MODE='off',XYO_GPU='0',XYO_PARFOR='0',
               XYO_JIT_DUMP2='1' if dump else '')
    if flag is not None:env['XYO_JIT_LIST_PROVEN_I32_INDEX']=str(int(flag))
    if not dump:del env['XYO_JIT_DUMP2']
    p=subprocess.run([str(binary),'run',str(project),'--jit',jit,
                      '--print-list','output'],cwd=ROOT,env=env,
                     capture_output=True,text=True,timeout=75)
    if p.returncode:raise RuntimeError((project,jit,flag,p.returncode,p.stderr[-650:]))
    vals=tuple(line.strip() for line in p.stdout.splitlines() if line.strip())
    return vals,p.stderr


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--binary',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=ROOT/'build/multi-bench-20261110/list-proven-i32')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    scenarios=[
      ('numbers',500,[i for i in range(1,501)],'direct'),
      ('one_past_end',501,[i for i in range(1,501)],'direct'),
      ('short_list',511,[1,2,3,4,5],'direct'),
      ('fraction_bound',38.2,list(range(45)),'direct'),
      ('zero',0,list(range(45)),'direct'),
      ('negative',-3,list(range(45)),'direct'),
      ('compound',129,list(range(150)),'compound'),
      ('mixed',129,[1, '12','0','abc',3.5,'','-4',0],'direct'),
      ('dynamic',55,list(range(65)),'dynamic'),
      ('length',79,list(range(85)),'length'),
      ('last',29,[1,2,3,4,5],'last'),
      ('index_write',20,list(range(30)),'index_write'),
    ]
    for name,n,items,mode in scenarios:
       path=args.out/f'{name}.sb3';write_sb3(path,fixture(n,items,mode))
       expected,_=run(binary,path,'off',False)
       for jit in ('eager','hotspot'):
           for flag in (False,True,None):
               got,_=run(binary,path,jit,flag)
               if got!=expected:raise AssertionError((name,jit,flag,expected,got))
       print('PASS',name,expected,flush=True)
    path=args.out/'numbers.sb3'
    cvtt={}
    for flag in (False,True,None):
      _,ir=run(binary,path,'eager',flag,dump=True)
      label='default' if flag is None else str(int(flag))
      (args.out/f'optimized-{label}.ll').write_text(ir)
      cvtt[label]=ir.count('call i64 @llvm.x86.sse2.cvttsd2si64(')
      print('IR',label,'proven_fptoui',ir.count('fptoui double'),'sse_cvtt_calls',cvtt[label],flush=True)
    assert cvtt['0'] > cvtt['1'] == cvtt['default'],cvtt
    print('PASS',len(scenarios),'scenarios, interpreter/eager/hotspot, flag OFF/ON/default',flush=True)

if __name__=='__main__':main()
