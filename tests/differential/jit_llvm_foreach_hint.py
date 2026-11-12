#!/usr/bin/env python3
"""Exact for-each LLVM loop-hint checks against the interpreter baseline."""
from __future__ import annotations
import json, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tests/differential'),str(ROOT/'tests/bench/algos')]
from jit_foreach_ssa import fixture, run


def main():
  binary=Path(sys.argv[1]).resolve()
  folder=ROOT/'build/multi-bench-20261110/llvm-foreach-test'
  folder.mkdir(parents=True,exist_ok=True)
  for warp in (True,False):
    project=folder/('warp.sb3' if warp else 'nonwarp.sb3')
    keys=fixture(project,warp)
    expected=run(binary,project,'off',False,keys)
    for mode in ('eager','hotspot'):
      for ssa in (False,True):
        for hint in (False,True):
          got=run(binary,project,mode,ssa,keys,{
             'XYO_JIT_LLVM_FOREACH_HINT': '1' if hint else '0',
             'XYO_JIT_LAZY_MODE':'off',
             'XYO_JIT_LLVM_REPEAT_HINT':'0',
             'XYO_REPEAT_UNROLL':'0'})
          assert got==expected,(warp,mode,ssa,hint,{
             k:(expected[k],got[k]) for k in keys if expected[k]!=got[k]})
          print('PASS',warp,mode,ssa,hint,flush=True)
  print('PASS all 16 foreach configurations and 2 interpreter baselines',flush=True)

if __name__=='__main__':main()
