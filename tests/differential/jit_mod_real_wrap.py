#!/usr/bin/env python3
"""Modulo real-wrap fast-path differential cases including binary64 boundaries."""
from __future__ import annotations
import json, math, os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tests/bench/algos'), str(ROOT/'tests/differential')]
from gen_algo_bench import Gen, S_call, S_add, S_set, add, make_project, mod, num, text, var, write_sb3

DIVISORS = (2,3,4,7,12,255,256,65535,65536,65537)


def values(m):
    around = (float(m),float(2*m))
    vals = [
      -2*m, -m-0.5, -m, -0.5, -0.0, 0.0, 5e-324, 1e-300,
      0.1, 0.25, 0.5, 0.75, 0.999999999999, 1, 1.25,
      m/2, m-1.25, m-1, m-0.5, m-0.25, m,
      m+0.125, m+0.25, m+0.5, m+0.75,
      2*m-1.25, 2*m-1, 2*m-0.5, 2*m-0.25, 2*m,
      2*m+0.25, 2*m+0.5, 2*m+1, 3*m, 2147483647, 2**52,
    ]
    for x in around:
      vals.extend([math.nextafter(x, -math.inf),math.nextafter(x, math.inf)])
    return [num(x) for x in vals]+[text('Infinity'),text('NaN'),text('4.5'),text(' 12 '),text('not-a-number')]


def project(m):
    g=Gen(['x'],['out'])
    body=[]
    for value in values(m):
       body.extend((S_set('x',value),S_add('out', mod(var('x'),num(m))),
                    S_add('out',mod(add(var('x'),num(m)),num(m)))))
    g.define('go',[],body,warp=True)
    g.script([S_call('go')])
    return make_project(g.vars.keys(),g.lists.keys(),g)


def run(binary,sb3,mode,real):
    env={k:v for k,v in os.environ.items() if not k.startswith('XYO_')}
    env.update(XYO_JIT_MODE=mode,XYO_JIT_MOD_SMALL_WRAP='1',XYO_JIT_MOD_REAL_WRAP=str(int(real)),XYO_GPU='0',XYO_PARFOR='0')
    p=subprocess.run([str(binary),'run',str(sb3),'--jit',mode,'--print-list','out'],cwd=ROOT,env=env,text=True,capture_output=True,timeout=60)
    if p.returncode:raise RuntimeError((mode,real,p.stderr[-600:],p.stdout[-350:]))
    return tuple(s.strip() for s in p.stdout.splitlines() if s.strip())


def main():
    binary=Path(sys.argv[1]).resolve()
    out=ROOT/'build/life-codecompare-20261110/realwrap-diff'
    out.mkdir(parents=True,exist_ok=True)
    for m in DIVISORS:
      sb3=out/f'mod-{m}.sb3'
      write_sb3(sb3,project(m))
      expected=run(binary,sb3,'off',False)
      assert len(expected)==len(values(m))*2,(m,len(expected))
      for mode in ('eager','hotspot'):
        for real in (False,True):
          got=run(binary,sb3,mode,real)
          if got!=expected:
            first=next((i for i,(a,b) in enumerate(zip(expected,got)) if a!=b),None)
            raise AssertionError((m,mode,real,'first mismatch',first,expected[first],got[first]))
      print('PASS',m,len(expected),'values',flush=True)
    print('PASS ALL',len(DIVISORS),'divisors; Interpreter/Eager/HotSpot, integer-only vs real fast path',flush=True)

if __name__=='__main__':main()
