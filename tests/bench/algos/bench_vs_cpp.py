#!/usr/bin/env python3
"""xyo-cpp vs C++ 対決フルコース: algo + micro を同条件で走らせる.

- xyo: 既存 sb3 を build/xyo run (+ --jit hotspot) で実行.
  プロジェクト内 `days since 2000` の times 中央値を採用 (bench.py と同じ).
- C++: Scratch ブロックと 1:1 対応の素朴ループ実装 (duel.cpp) を
  `g++ -O2` ビルドして steady_clock で計測. SIMD intrinsic や
  OpenSSL 等のショートカットは使わない (faithful 部門).

使い方:
  python3 tests/bench/algos/bench_vs_cpp.py --medium
  python3 tests/bench/algos/bench_vs_cpp.py --medium --reps 1 --cpp-reps 3
"""
import argparse, json, os, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(HERE, "out")
GEN = os.path.join(HERE, "gen_algo_bench.py")
DUEL_SRC = os.path.join(HERE, "duel.cpp")

sys.path.insert(0, HERE)
from bench_vs_python import run_xyo

QUICK_SIZES = {
    "quicksort": 4000,
    "mergesort": 4000,
    "bubblesort": 500,
    "sieve": 100000,
    "fib": 27,
    "sha256": 16384,
    "micro_loop": 1000000,
    "micro_read": 1000000,
    "micro_write": 1000000,
    "micro_append": 1000000,
}
MEDIUM_SIZES = {
    "quicksort": 50000, "mergesort": 50000, "bubblesort": 2000,
    "sieve": 1000000, "fib": 29, "sha256": 16384,
    "micro_loop": 5000000, "micro_read": 5000000,
    "micro_write": 5000000, "micro_append": 5000000,
}
FULL_SIZES = {
    "quicksort": 200000, "mergesort": 200000, "bubblesort": 5000,
    "sieve": 2000000, "fib": 31, "sha256": 65536,
    "micro_loop": 50000000, "micro_read": 50000000,
    "micro_write": 50000000, "micro_append": 20000000,
}

def build_duel(compiler="g++", cxxflags="-O2 -std=c++20"):
    import tempfile
    out = os.path.join(tempfile.gettempdir(),
                       "xyo_duel_%d" % os.getpid())
    cmd = compiler.split() + cxxflags.split() + ["-o", out, DUEL_SRC]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError("compile failed:\n" + r.stderr[-2000:])
    ver = subprocess.run([compiler, "--version"], capture_output=True,
                         text=True).stdout.splitlines()
    return out, (ver[0] if ver else compiler) + " " + cxxflags

def run_cpp(duel, name, n, reps):
    p = subprocess.run([duel, name, str(n), str(reps)],
                       capture_output=True, text=True, timeout=900)
    if p.returncode != 0:
        raise RuntimeError("duel failed: " + p.stderr[-500:])
    check, times = None, []
    for line in p.stdout.splitlines():
        if line.startswith("check="):
            check = line[len("check="):]
        elif line.startswith("times:"):
            times = [float(x) for x in line[len("times:"):].split()]
    return statistics.median(times), times, check

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--medium", action="store_true")
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--reps", type=int, default=1,
                    help="xyo プロセス反復 (in-project 中央値を使うので 1 で可)")
    ap.add_argument("--cpp-reps", type=int, default=3)
    ap.add_argument("--algo", default="")
    ap.add_argument("--sizes", default="")
    ap.add_argument("--compiler", default="g++")
    ap.add_argument("--cxxflags", default="-O2 -std=c++20")
    ap.add_argument("--json", default=os.path.join(HERE, "results-vs-cpp.json"))
    args = ap.parse_args()
    if args.medium:
        sizes = dict(MEDIUM_SIZES)
    else:
        sizes = dict(QUICK_SIZES if args.quick else FULL_SIZES)
    for kv in filter(None, args.sizes.split(",")):
        k, v = kv.split("="); sizes[k] = int(v)
    if args.algo:
        sizes = {k: v for k, v in sizes.items() if k in args.algo.split(",")}

    duel, cxxinfo = build_duel(args.compiler, args.cxxflags)
    print("c++: %s" % cxxinfo)

    all_names = list(sizes.keys())
    gen_cmd = [sys.executable, GEN, OUT, "--sizes",
               ",".join("%s=%d" % (k, sizes[k]) for k in all_names),
               "--only", ",".join(all_names)]
    subprocess.run(gen_cmd, check=True, stdout=subprocess.DEVNULL)
    manifest = json.load(open(os.path.join(OUT, "manifest.json")))

    mode = "quick" if args.quick else ("medium" if args.medium else "full")
    print("=== xyo-cpp vs C++ 対決 (%s) ===" % mode)
    rows = []
    for name, n in sizes.items():
        x_times, x_check, x_wall, x_med, x_ok = [], "?", 0, float("nan"), False
        if name in manifest:
            path = os.path.join(OUT, manifest[name]["file"])
            for _ in range(args.reps):
                x_times, x_check, x_wall = run_xyo(args.xyo, path)
            if x_times:
                x_med = statistics.median(x_times)
            x_ok = (x_check == manifest[name]["expected_check"])
        c_med, c_ts, c_check = run_cpp(duel, name, n, args.cpp_reps)
        c_ok = (str(c_check) == str(manifest.get(name, {}).get("expected_check")))
        ratio = c_med / x_med if x_med and x_med > 0 else float("nan")
        winner = "xyo" if x_med < c_med else "C++"
        flag = "" if (x_ok and c_ok) else " !!CHECK!!"
        print("  %-11s n=%-8d xyo %8.1f ms | c++ %8.1f ms | c/xyo %6.2fx -> %s%s" % (
            name, n, x_med, c_med, ratio, winner, flag))
        rows.append({"algo": name, "n": n, "xyo_ms": x_med, "cpp_ms": c_med,
                     "ratio": ratio, "xyo_check_ok": x_ok,
                     "cpp_check_ok": c_ok, "cpp_check": c_check,
                     "xyo_times": x_times, "cpp_times": c_ts})
    print("\n--- duel table (ms, 小さい方が速い) ---")
    print("%-11s %9s %10s %10s %8s  winner" % ("algorithm", "n", "xyo JIT", "C++ -O2", "c/xyo"))
    for r in rows:
        w = "xyo" if r["xyo_ms"] < r["cpp_ms"] else "C++"
        print("%-11s %9d %10.1f %10.1f %8.2f  %s" % (r["algo"], r["n"], r["xyo_ms"], r["cpp_ms"], r["ratio"], w))
    print("\nc/xyo > 1 は xyo が速い (C++時間 / xyo時間)")
    if args.json and args.json != "-":
        json.dump({"sizes": sizes, "cxx": cxxinfo, "rows": rows},
                  open(args.json, "w"), indent=1)
        print("wrote", args.json)

if __name__ == "__main__":
    main()
