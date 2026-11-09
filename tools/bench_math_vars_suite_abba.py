#!/usr/bin/env python3
"""Paired ScratchBench vars-suite mode comparison (CPU pinned).

The suite contains vars_setchange, vars_many, and the broadcast-triggered
vars_local (which executes the same arithmetic in a different context).
This avoids confusing a standalone procedure microbenchmark with actual
suite scheduling/compiler behavior.
"""
import argparse
import json
import math
import os
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNELS = ("vars_setchange", "vars_many", "vars_local")

def run(binary: Path, project: Path, mode: str, cpu: int) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_NUMERIC_REPEAT_U32_MOD": str(int(mode != "i64")),
        "XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD": str(int(mode == "s32")),
        "XYO_JIT_MODE": "hotspot",
        "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
    })
    p = subprocess.run(
        ["taskset", "-c", str(cpu), str(binary), "run", str(project),
         "--print-list", "output", "--fps", "30"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=100,
    )
    if p.returncode:
        raise RuntimeError(f"mode={mode}: {p.stderr[-900:]}")
    rows={}
    for line in p.stdout.splitlines():
        parts=line.split(" | ")
        if len(parts)==6 and parts[0] in KERNELS:
            rows[parts[0]]=dict(rate=float(parts[3]), checksum=parts[5])
    assert len(rows)==len(KERNELS), p.stdout[-1100:]
    assert "DONE" in p.stdout.splitlines(),p.stdout[-300:]
    return rows

def main() -> None:
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary",type=Path,default=ROOT/"build/math016-final-xyo")
    ap.add_argument("--project",type=Path,default=ROOT/"build/math016-vars-suite.sb3")
    ap.add_argument("--groups",type=int,default=4)
    ap.add_argument("--old-mode",choices=("i64","u32","s32"),default="i64")
    ap.add_argument("--new-mode",choices=("i64","u32","s32"),default="s32")
    ap.add_argument("--cpu",type=int,default=12)
    ap.add_argument("--out",type=Path,default=ROOT/"build/math016-vars-suite-abba.json")
    args=ap.parse_args()
    binary=args.binary.resolve()
    project=args.project.resolve()
    all_rows=[]
    for group in range(args.groups):
        pattern=(False, True, True, False) if group%2==0 else (
            True, False, False, True)
        for slot,enabled in enumerate(pattern):
            mode=args.new_mode if enabled else args.old_mode
            result=run(binary,project,mode,args.cpu)
            all_rows.append(dict(group=group,slot=slot,enabled=enabled,
                                 mode=mode,values=result))
            args.out.write_text(json.dumps(all_rows,indent=2)+"\n")
            print(group,slot,mode,
                  *(f"{k} {v['rate']:.1f}" for k,v in result.items()),
                  flush=True)
    for key in KERNELS:
        groups=[]
        for i in range(args.groups):
            records=[x for x in all_rows if x["group"]==i]
            old=[x["values"][key]["rate"] for x in records if not x["enabled"]]
            new=[x["values"][key]["rate"] for x in records if x["enabled"]]
            groups.append(math.sqrt((new[0]/old[0])*(new[1]/old[1])))
        checksums={r["values"][key]["checksum"] for r in all_rows}
        assert len(checksums)==1,(key,checksums)
        print("RESULT",key,"median_ratio",statistics.median(groups),
              "wins",sum(r>1 for r in groups),"/",args.groups,
              "group_ratios",groups,"checksum",next(iter(checksums)))
if __name__ == "__main__":
    main()
