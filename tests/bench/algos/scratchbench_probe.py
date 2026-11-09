#!/usr/bin/env python3
"""Isolated ScratchBench causal probes; keeps the vendored suite unchanged."""
import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "scratch-bench" / "performance" / "project").is_dir())
SB = ROOT / "scratch-bench"
SOURCE = SB / "performance" / "project"
OUT = ROOT / "build" / "scratchbench-cause-probes"
XYO = Path(os.getenv("XYO_PROBE_BIN", str(ROOT / "build" / "xyo")))
FRACTCH = SB / "node_modules/.bin/fractch"

CASES = {
    # All equivalence transformations below depend on these kernels' domains.
    # They are diagnostic counterfactuals, NOT general Scratch rewrite rules.
    "primes": ("algo_primes", 600, None),
    "sieve": ("algo_sieve", 5000, None),
    "primes_divfloor": ("algo_primes", 600, "divfloor"),
    "primes_cmp_mod": ("algo_primes", 600, "cmp_mod"),
    "primes_cmp_flag": ("algo_primes", 600, "cmp_flag"),
    "primes_cmp_both": ("algo_primes", 600, "cmp_both"),
    "primes_truthy": ("algo_primes", 600, "truthy"),
    "arith": ("math_arith", 5000, None),
    "arith_divfloor": ("math_arith", 5000, "divfloor"),
    "join": ("str_join", 20000, None),
    "join_batch4": ("str_join", 20000, "batch4"),
    "raycast": ("app_raycast", 320, None),
    "life": ("app_life", 1, None),
    "raycast_geom": ("app_raycast", 320, "geom"),
    "raycast_truthy": ("app_raycast", 320, "truthy"),
    "raycast_zero_truthy": ("app_raycast", 320, "truthy"),
    "raycast_all_truthy": ("app_raycast", 320, "truthy"),
    "clonechurn": ("sched_clonechurn", 1, None),
    "waituntil": ("sched_waituntil", 1, None),
    "looptick": ("sched_looptick", 1, None),
    "threads": ("sched_threads", 1, None),
    "pingpong": ("sched_pingpong", 1, None),
    "fanwait": ("sched_fanwait", 1, None),
    "fanout": ("sched_fanout", 1, None),
    "list_append": ("list_append", 10000, None),
}

def build(name):
    key, ops, variant = CASES[name]
    base = OUT / "source" / name
    if base.exists():
        shutil.rmtree(base)
    shutil.copytree(SOURCE, base)
    main = base / "Stage" / "main.fractch"
    body = main.read_text()
    for suite in ("loops", "vars", "math", "strings", "lists", "algos", "apps", "sched"):
        assert f"  @suite_{suite}();" in body
        body = body.replace(f"  @suite_{suite}();", "")
    if key.startswith("sched_"):
        snippet = f"  @sched_spawn();\n  @{key}();"
    else:
        snippet = (f'  @bench_begin();\n  repeat 7 {{\n'
                   f'    @{("k_" + key)}();\n    @bench_round();\n'
                   f'  }}\n  @bench_end("{key}", {ops});')
    body = body.replace('  @log_text("DONE");', snippet + '\n  @log_text("DONE");')
    main.write_text(body)
    if variant:
        file = base / "Stage" / {
            "algo_primes":"algos.fractch","math_arith":"math.fractch",
            "str_join":"strings.fractch","app_raycast":"apps.fractch"}[key]
        src = file.read_text()
        if name == "primes_truthy":
            old = "k_n == 0"
            new = "!k_n"
        elif name in ("raycast_zero_truthy", "raycast_all_truthy"):
            old = "if rc_rdx == 0"
            new = "if !rc_rdx"
        elif name == "raycast_truthy":
            old = "until rc_hit == 1"
            new = "until rc_hit"
        elif name == "primes_divfloor":
            old = 'k_i % k_j == 0'
            new = '(k_i - floor(k_i / k_j) * k_j) == 0'
        elif name.startswith("primes_cmp_"):
            replacements = {
                "primes_cmp_mod": ("k_i % k_j == 0", "k_i % k_j < 1"),
                "primes_cmp_flag": ("k_n == 0", "k_n < 0.5"),
                "primes_cmp_both": ("k_i % k_j == 0", "k_i % k_j < 1"),
            }
            old, new = replacements[name]
        elif name == "arith_divfloor":
            old = '(k_a * 31 + k_i) % 1000003'
            new = '(k_a * 31 + k_i) - floor((k_a * 31 + k_i) / 1000003) * 1000003'
        elif name == "join_batch4":
            old = 'repeat 20000 {\n      k_s = k_s ++ "ab";\n    }'
            new = 'repeat 5000 {\n      k_s = k_s ++ "abababab";\n    }'
        elif name == "raycast_geom":
            old = 'lists["rc_map"][rc_my * 8 + rc_mx + 1] != 0'
            new = '(rc_mx == 0 || rc_mx == 7 || rc_my == 0 || rc_my == 7 || (rc_mx == 5 && rc_my >= 2 && rc_my <= 4) || (rc_mx == 3 && rc_my == 5))'
        assert src.count(old) == 1, (name, src.count(old))
        src = src.replace(old, new)
        if name == "primes_cmp_both":
            assert src.count("k_n == 0") == 1
            src = src.replace("k_n == 0", "k_n < 0.5")
        if name in ("raycast_zero_truthy", "raycast_all_truthy"):
            assert src.count("if rc_rdy == 0") == 1
            src = src.replace("if rc_rdy == 0", "if !rc_rdy")
        if name == "raycast_all_truthy":
            assert src.count("until rc_hit == 1") == 1
            src = src.replace("until rc_hit == 1", "until rc_hit")
        file.write_text(src)
    archive = OUT / "projects" / (name + ".sb3")
    archive.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(FRACTCH), "check", str(base)], cwd=SB, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    subprocess.run([str(FRACTCH), "to", str(archive), "from", str(base)], cwd=SB, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print("BUILT", name, archive, flush=True)


def run_case(name, mode, env_extra=None, fps=30):
    key,_,_ = CASES[name]
    archive = OUT / "projects" / (name + ".sb3")
    env = dict(os.environ)
    for k in ("XYO_PROF","XYO_RT_PROF","XYO_GC_DEBUG","XYO_STR_DEBUG","XYO_LISTSTATS"):
        env.pop(k,None)
    env.update(env_extra or {})
    if mode == "xyo":
        cmd = [str(XYO), "run", str(archive), "--print-list", "output"]
        if fps: cmd.extend(["--fps", str(fps)])
        env["XYO_JIT_MODE"]="hotspot"
    else:
        cmd = ["node", str(SB / "runtimes/node-vm.mjs"), "turbowarp", str(archive)]
        env["BENCH_COMPILER"]="1"
    started=time.monotonic()
    p=subprocess.run(cmd, cwd=SB, env=env, capture_output=True, text=True,timeout=120)
    elapsed = time.monotonic()-started
    output = p.stdout.splitlines()
    rows = []
    for line in output:
        clean = line.removeprefix("[PROJECT] ")
        fields = clean.split(" | ")
        if len(fields)==6 and fields[0]==key:
            rows.append(fields)
    got = rows
    return {
        "mode":mode, "name":name, "fps":fps, "env":env_extra or {},
        "exit":p.returncode, "done":any(line.endswith("DONE") for line in output),
        "elapsed_s":round(elapsed,3), "rate":float(got[-1][3]) if got else None,
        "checksum":got[-1][5] if got else None,
        "ops":got[-1][2] if got else None,
        "total_ms":got[-1][1] if got else None,
        "stderr_tail":p.stderr[-5000:],"stdout_tail":p.stdout[-1000:]
    }


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("action",choices=["make","run"])
    ap.add_argument("--cases",default=",".join(CASES))
    ap.add_argument("--reps",type=int,default=3)
    ap.add_argument("--mode",choices=["xyo","tw","both"],default="both")
    ap.add_argument("--fps",type=int,default=30)
    ap.add_argument("--gc",default="")
    ap.add_argument("--debug",action="store_true")
    args=ap.parse_args()
    names=args.cases.split(",")
    for name in names: assert name in CASES,name
    if args.action=="make":
        for n in names:build(n)
        return
    env = {"XYO_GC_THRESHOLD":args.gc} if args.gc else {}
    if args.debug:env.update({"XYO_GC_DEBUG":"1","XYO_STR_DEBUG":"1"})
    modes = ["xyo","tw"] if args.mode=="both" else [args.mode]
    rows = []
    for rep in range(args.reps):
        # Alternate order to minimize relative drift.
        for name in (names if rep%2==0 else list(reversed(names))):
            for mode in (modes if rep%2==0 else list(reversed(modes))):
                r=run_case(name,mode,env if mode=="xyo" else {},fps=args.fps)
                rows.append(r)
                print(f'RUN {rep+1}/{args.reps} {name:18s} {mode:3s} rate={r["rate"]} checksum={r["checksum"]} done={r["done"]} wall={r["elapsed_s"]}s',flush=True)
                if r["exit"]!=0 or not r["done"] or r["rate"] is None:
                    print("FAIL",r["stderr_tail"],r["stdout_tail"],flush=True)
    OUT.mkdir(parents=True,exist_ok=True)
    label = hashlib.sha256(",".join(names).encode()).hexdigest()[:10]
    suffix = '-' + label + ('-'+args.gc if args.gc else '')+'-'+args.mode+'-'+str(args.fps)
    output=OUT / ('results'+suffix+'.json')
    output.write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n')
    for name in names:
        for mode in modes:
            values=[x['rate'] for x in rows if x['name']==name and x['mode']==mode and x['rate'] is not None and x['done']]
            if values:print('SUMMARY',name,mode,round(statistics.median(values),3),len(values),flush=True)
    print('SAVED',output,flush=True)

if __name__=="__main__":
    main()
