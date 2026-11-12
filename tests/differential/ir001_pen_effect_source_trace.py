#!/usr/bin/env python3
"""IR-001 real LLVM pen_stamp source-PC / CommitOnceEffects parity."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/differential'))
from runtime_pen_journal import fixture
from ir001_effect_source_trace import expected_pcs

LINE=re.compile(
    r"\[ir001-effect-source\] kind=pen-stamp pc=(\d+) block=(\S+) "
    r"revision=(\d+) digest=([0-9a-f]+):([0-9a-f]+) "
    r"revision_matches=(\d) journal=(\d) published_delta=(\d+) "
    r"delivered_delta=(\d+) immediate_delta=(\d+) slice_delta=(\d+) "
    r"pending=(\d+) rooted=(\d+) exact_delivery=(\d) "
    r"event_published=(\d+) event_delivered=(\d+) event_slice=(\d+)"
)

def run(binary,source,journal,trace):
    env=dict(os.environ)
    env.update(XYO_DUMPVARS='json',XYO_PAR='0',XYO_EXIT_ON_DONE='1',
               XYO_JIT_ASYNC='0',XYO_EFFECT_JOURNAL='1' if journal else '0',
               XYO_EFFECT_JOURNAL_STATS='1' if journal else '0',
               XYO_IR001_TRACE_EFFECT_PC='1' if trace else '0')
    p=subprocess.run([str(binary.resolve()),'run',str(source.resolve()),
                      '--jit','eager','--frames','100'],
                     cwd=ROOT,env=env,capture_output=True,text=True,timeout=90)
    assert p.returncode==0,p.stderr[-2500:]
    at=p.stdout.find('{')
    assert at!=-1
    state,_=json.JSONDecoder().raw_decode(p.stdout[at:])
    stats=re.search(r'\[effect-journal\].*committed_effects=(\d+) immediate_effects=(\d+)',p.stderr)
    return state['Bench'],tuple(map(int,stats.groups())) if stats else None,p.stderr

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--native',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path)
    a=parser.parse_args()
    project=fixture(a.out/'ir001-pen-source-pc.sb3')
    expected=expected_pcs(project,'pen_stamp')
    assert len(expected)==2,expected
    base,stats,base_log=run(a.native,project,True,False)
    assert base=={'x':'7','done':'1'} and stats==(2,0),(base,stats)
    assert '[ir001-effect-source]' not in base_log
    state,stats,trace=run(a.native,project,True,True)
    assert state==base and stats==(2,0),(state,stats)
    records=LINE.findall(trace)
    assert len(records)==2,trace[-2300:]
    seen=set()
    for record in records:
        pc,block,rev,lo,hi,valid,journal,published,delivered,immediate,slices,pending,roots,exact,event_published,event_delivered,event_slice=record
        assert int(pc)==expected[block],(pc,block,expected)
        assert int(rev)>0 and (int(lo,16) or int(hi,16))
        assert valid=='1' and journal=='1'
        assert (published,delivered,immediate,slices,pending,roots,exact,event_published,event_delivered)==(
            '1','1','0','1','0','0','1','1','1'),record
        assert int(event_slice)>0
        seen.add(block)
    assert seen==set(expected)
    state,_,without=run(a.native,project,False,True)
    assert state==base
    records_off=LINE.findall(without)
    assert len(records_off)==2 and all(row[6]=='0' and row[7]=='0' for row in records_off)
    print("PASS IR-001 real LLVM pen_stamp source PC, 2 sites, journal on/off parity")

if __name__=='__main__':
    main()
