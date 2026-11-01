#!/usr/bin/env python3
"""Run the algorithm benchmark on xyo and on the vendored scratch-vm
(TurboWarp fork), validate every result, and print a table.

  python3 tests/bench/algos/bench.py                 # full run
  python3 tests/bench/algos/bench.py --quick         # small sizes, smoke test
  python3 tests/bench/algos/bench.py --skip-interp   # JIT vs TurboWarp only

Modes
  tw-compiled   TurboWarp compiler on (scratch-vm's default, what the
                TurboWarp app uses)
  tw-interp     scratch-vm interpreter (compiler off)
  xyo-jit       xyo HotSpot (default): interpreter -> LLVM JIT promotion
  xyo-danger    xyo HotSpot with --danger (warp 500 ms budget disabled)
  xyo-interp    xyo interpreter only (--jit off)

Each project times its own algorithm with `days since 2000` (ms) after a
warm-up run, so the reported numbers exclude project load, dataset
generation and JIT/compile time.  All modes must produce the checksum the
Python reference in gen_algo_bench.py computes, otherwise the run is
reported as INVALID and the number is not used.

Writes tests/bench/algos/results.json (raw samples) unless --json -.
"""
import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(HERE, "out")
GEN = os.path.join(HERE, "gen_algo_bench.py")
RUN_TW = os.path.join(HERE, "run_tw.js")

DEFAULT_SIZES = {
    "quicksort": 200000,
    "mergesort": 200000,
    "bubblesort": 5000,
    "sieve": 2000000,
    "fib": 31,
    "sha256": 65536,
}
QUICK_SIZES = {
    "quicksort": 4000,
    "mergesort": 4000,
    "bubblesort": 500,
    "sieve": 100000,
    "fib": 27,
    "sha256": 16384,
}

MODES = ["tw-compiled", "xyo-jit", "xyo-jit-danger", "xyo-interp", "tw-interp"]
INTERP_MODES = {"xyo-interp", "tw-interp"}


def run_cmd(cmd, env=None, timeout=3600):
    e = dict(os.environ)
    if env:
        e.update(env)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=e)
    wall = (time.perf_counter() - t0) * 1000.0
    return p, wall


def parse_xyo(stdout):
    text = stdout.decode("utf-8", "replace")
    start = text.index("{")
    end = text.rindex("}")
    data = json.loads(text[start:end + 1])
    b = data["Bench"]
    return {
        "times": [float(x) for x in b.get("times", [])],
        "check": b.get("check"),
    }


def run_xyo(xyo, path, mode):
    extra = ["--jit", "off"] if mode == "xyo-interp" else ["--jit", "hotspot"]
    if mode == "xyo-jit-danger":
        extra.append("--danger")
    p, wall = run_cmd([xyo, "run", path] + extra,
                      env={"XYO_DUMPVARS": "json"})
    if p.returncode != 0:
        raise RuntimeError("xyo failed (%d): %s" %
                           (p.returncode, p.stderr.decode()[-400:]))
    r = parse_xyo(p.stdout)
    r["wall_ms"] = wall
    return r


def run_tw(path, mode):
    p, wall = run_cmd(["node", RUN_TW, path, "interp" if mode == "tw-interp"
                       else "compiled"])
    if p.returncode != 0:
        raise RuntimeError("node failed (%d): %s" %
                           (p.returncode, p.stderr.decode()[-800:]))
    r = json.loads(p.stdout.decode())
    r["wall_ms"] = wall
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--sizes", default="", help="e.g. quicksort=4000,fib=29")
    ap.add_argument("--algo", default="", help="comma separated algorithm filter")
    ap.add_argument("--reps", type=int, default=3,
                    help="process repetitions per mode (default 3)")
    ap.add_argument("--skip-interp", action="store_true")
    ap.add_argument("--micro", action="store_true",
                    help="also run the loop / list-access controls")
    ap.add_argument("--modes", default="",
                    help="comma separated subset of " + ",".join(MODES))
    ap.add_argument("--json", default=os.path.join(HERE, "results.json"))
    args = ap.parse_args()

    sizes = dict(QUICK_SIZES if args.quick else DEFAULT_SIZES)
    for kv in filter(None, args.sizes.split(",")):
        k, v = kv.split("=")
        sizes[k] = int(v)
    if args.micro:
        sizes["micro_loop"] = 50000000
        sizes["micro_read"] = 50000000
        sizes["micro_write"] = 50000000
        sizes["micro_append"] = 20000000
    algos = [a for a in filter(None, args.algo.split(","))]
    if algos:
        sizes = {k: v for k, v in sizes.items() if k in algos}

    # regenerate the fixtures so the table always matches the generator
    gen_cmd = [sys.executable, GEN, OUT,
               "--sizes", ",".join("%s=%d" % kv for kv in sizes.items())]
    if algos:
        gen_cmd += ["--only", ",".join(algos)]
    subprocess.run(gen_cmd, check=True, stdout=subprocess.DEVNULL)
    manifest = json.load(open(os.path.join(OUT, "manifest.json")))

    modes = [m for m in MODES
             if (not args.modes or m in args.modes.split(",")) and
             not (args.skip_interp and m in INTERP_MODES)]

    rows = []
    raw = {}
    for name in sizes:
        if name not in manifest:
            continue
        m = manifest[name]
        path = os.path.join(OUT, m["file"])
        row = {"algorithm": name, "n": m["n"],
               "expected_check": m["expected_check"], "modes": {}}
        for mode in modes:
            reps = 1 if mode in INTERP_MODES else args.reps
            samples, run_medians, walls, checks = [], [], [], set()
            for _ in range(reps):
                r = (run_xyo(args.xyo, path, mode) if mode.startswith("xyo")
                     else run_tw(path, mode))
                samples += r["times"]
                # A single in-project repetition can be polluted (e.g. V8's
                # major GC hits the 3rd rebuild of a 2M-item list).  Median
                # within a process first, then across processes.
                run_medians.append(statistics.median(r["times"]))
                walls.append(r["wall_ms"])
                checks.add(str(r["check"]))
            valid = checks == {m["expected_check"]}
            row["modes"][mode] = {
                "samples": samples,
                "run_medians": run_medians,
                "median_ms": round(statistics.median(run_medians), 3),
                "min_ms": round(min(samples), 3),
                "wall_ms": round(statistics.median(walls), 1),
                "check": sorted(checks),
                "valid": valid,
            }
            flag = "" if valid else "  !! CHECK MISMATCH"
            print("  %-11s %-14s n=%-8d %8.1f ms  (wall %6.0f ms)%s" % (
                name, mode, m["n"],
                row["modes"][mode]["median_ms"],
                row["modes"][mode]["wall_ms"], flag), flush=True)
        rows.append(row)
        raw[name] = row

    # ----------------------------------------------------------- table
    cols = [c for c in modes]
    header = ("%-11s %9s" % ("algorithm", "n") +
              "".join("%16s" % c for c in cols) + "%10s" % "xyo/TW")
    print("\n" + header)
    print("-" * len(header))
    for row in rows:
        line = "%-11s %9d" % (row["algorithm"], row["n"])
        for c in cols:
            v = row["modes"].get(c)
            line += "%16s" % ("%s%.1f" % ("" if v["valid"] else "INVALID ",
                                          v["median_ms"]) if v else "-")
        tw = row["modes"].get("tw-compiled")
        xy = row["modes"].get("xyo-jit")
        if tw and xy and tw["valid"] and xy["valid"]:
            line += "%10.2fx" % (tw["median_ms"] / xy["median_ms"])
        else:
            line += "%10s" % "-"
        print(line)
    print("\nnumbers are the median of the per-process medians of the "
          "in-project\n`days since 2000` samples (ms); xyo/TW = TurboWarp "
          "compiled / xyo JIT\n(>1 means xyo is faster)")

    info = {
        "cpu": cpu_model(),
        "node": node_version(),
        "llvm": llvm_version(),
        "xyo_build": args.xyo,
        "sizes": sizes,
        "modes": modes,
        "reps": args.reps,
    }
    out = {"meta": info, "results": raw}
    if args.json and args.json != "-":
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)
        print("\nwrote", args.json)


def cpu_model():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def node_version():
    try:
        return subprocess.run(["node", "--version"], capture_output=True,
                              text=True).stdout.strip()
    except OSError:
        return "n/a"


def llvm_version():
    for tool in ("llvm-config", "llvm-config-23", "llvm-config-18"):
        try:
            p = subprocess.run([tool, "--version"], capture_output=True,
                               text=True)
            if p.returncode == 0:
                return p.stdout.strip()
        except OSError:
            continue
    return "n/a"


if __name__ == "__main__":
    main()
