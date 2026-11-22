#!/usr/bin/env python3
"""xyo-cpp vs Python 対決フルコース: algo + micro を同条件で走らせる.

- xyo: 既存 sb3 を build/xyo run (+ --jit hotspot) で実行.
  プロジェクト内 `days since 2000` の times 中央値を採用 (bench.py と同じ).
- Python: Scratch ブロックと 1:1 対応の素朴ループ実装を perf_counter で計測.
  sorted()/hashlib 等の C ショートカットは使わない (faithful 部門).
  おまけで C ショートカット部門も参考計測する.

使い方:
  python3 tests/bench/algos/bench_vs_python.py --quick
  python3 tests/bench/algos/bench_vs_python.py --quick --reps 1 --py-reps 3
"""
import argparse, json, os, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(HERE, "out")
GEN = os.path.join(HERE, "gen_algo_bench.py")

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
FULL_SIZES = {
    "quicksort": 200000,
    "mergesort": 200000,
    "bubblesort": 5000,
    "sieve": 2000000,
    "fib": 31,
    "sha256": 65536,
    "micro_loop": 50000000,
    "micro_read": 50000000,
    "micro_write": 50000000,
    "micro_append": 20000000,
}

LCG_A, LCG_B, LCG_M = 25173, 13849, 65536
SEED0 = 12345

def lcg(n, seed=SEED0):
    s = seed
    out = [0]*n
    for i in range(n):
        s = (s * LCG_A + LCG_B) % LCG_M
        out[i] = s
    return out

# ---------------- Python faithful impls (Scratch と同じ手順) ----------------
def py_quicksort(a):
    # Hoare, 中央ピボット, 再帰 (Scratch の qs と同形)
    sys.setrecursionlimit(1000000)
    def qs(lo, hi):  # 0-based inclusive
        if lo < hi:
            pivot = a[(lo+hi)//2]
            i, j = lo, hi
            while i <= j:
                while a[i] < pivot: i += 1
                while a[j] > pivot: j -= 1
                if i <= j:
                    a[i], a[j] = a[j], a[i]
                    i += 1; j -= 1
            if lo < j: qs(lo, j)
            if i < hi: qs(i, hi)
    qs(0, len(a)-1)
    return a

def py_mergesort(a):
    aux = [0]*len(a)
    def ms(lo, hi):
        if lo < hi:
            mid = (lo+hi)//2
            ms(lo, mid); ms(mid+1, hi)
            i, j, k = lo, mid+1, lo
            while k <= hi:
                if i > mid:
                    aux[k] = a[j]; j += 1
                elif j > hi:
                    aux[k] = a[i]; i += 1
                elif a[i] <= a[j]:
                    aux[k] = a[i]; i += 1
                else:
                    aux[k] = a[j]; j += 1
                k += 1
            for k in range(lo, hi+1):
                a[k] = aux[k]
    ms(0, len(a)-1)
    return a

def py_bubblesort(a):
    n = len(a)
    i = 0
    while i < n:
        swapped = False
        j = 0
        while j < n-1-i:
            if a[j] > a[j+1]:
                a[j], a[j+1] = a[j+1], a[j]
                swapped = True
            j += 1
        if not swapped:
            break
        i += 1
    return a

def py_sieve(n):
    fl = [1]*(n+1)
    fl[0] = 0
    if n >= 1: fl[1] = 0
    i = 2
    while i*i <= n:
        if fl[i]:
            step = i
            start = i*i
            for j in range(start, n+1, step):
                fl[j] = 0
        i += 1
    return fl

def py_fib(n):
    if n < 2: return n
    return py_fib(n-1) + py_fib(n-2)

def py_sha256_faithful(n):
    # 純 Python の SHA-256 (hashlib は使わない): Scratch 版と同じ
    # FIPS 180-4 の圧縮関数を Python 整数演算で回す。メッセージは同じ LCG。
    K = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
         0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
         0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
         0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
         0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
         0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
         0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
         0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
         0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
         0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
         0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]
    H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
    msg = bytearray()
    s = 42
    for _ in range(n):
        s = (s * LCG_A + LCG_B) % LCG_M
        msg.append(s % 256)
    # padding (Scratch 版と同じ)
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    bits = n * 8
    msg += bits.to_bytes(8, 'big')
    M = 0xFFFFFFFF
    for off in range(0, len(msg), 64):
        blk = msg[off:off+64]
        W = [int.from_bytes(blk[i*4:(i+1)*4], 'big') for i in range(16)]
        for t in range(16, 64):
            x2 = W[t-2]
            r17 = ((x2 >> 17) | (x2 << 15)) & M
            r19 = ((x2 >> 19) | (x2 << 13)) & M
            s1 = r17 ^ r19 ^ (x2 >> 10)
            x15 = W[t-15]
            r7 = ((x15 >> 7) | (x15 << 25)) & M
            r18 = ((x15 >> 18) | (x15 << 14)) & M
            s0 = r7 ^ r18 ^ (x15 >> 3)
            W.append((W[t-16] + s0 + W[t-7] + s1) & M)
        a,b,c,d,e,f,g,h = H
        for t in range(64):
            r6 = ((e >> 6) | (e << 26)) & M
            r11 = ((e >> 11) | (e << 21)) & M
            r25 = ((e >> 25) | (e << 7)) & M
            S1 = r6 ^ r11 ^ r25
            ch = (e & f) ^ ((~e) & g & M)
            t1 = (h + S1 + ch + K[t] + W[t]) & M
            ra2 = ((a >> 2) | (a << 30)) & M
            ra13 = ((a >> 13) | (a << 19)) & M
            ra22 = ((a >> 22) | (a << 10)) & M
            S0 = ra2 ^ ra13 ^ ra22
            S0 &= M
            maj = (a & b) ^ (a & c) ^ (b & c)
            t2 = (S0 + maj) & M
            h,g,f,e,d,c,b,a = g,f,e,(d+t1)&M,c,b,a,(t1+t2)&M
        H = [(x+y) & M for x, y in zip(H, [a,b,c,d,e,f,g,h])]
    return ":".join(str(x) for x in H)

def check_sorted(a):
    s = sum(a); h = 0
    for v in a: h = (h*31+v) % 1000000007
    ok = all(x <= y for x, y in zip(a, a[1:]))
    return "%d:%d:%d" % (s, h, 1 if ok else 0)

def run_python(name, n, reps):
    ts = []
    check = None
    for _ in range(reps):
        if name == "quicksort":
            data = lcg(n)
            t0 = time.perf_counter(); py_quicksort(data); t1 = time.perf_counter()
            check = check_sorted(data)
        elif name == "mergesort":
            data = lcg(n)
            t0 = time.perf_counter(); py_mergesort(data); t1 = time.perf_counter()
            check = check_sorted(data)
        elif name == "bubblesort":
            data = lcg(n)
            t0 = time.perf_counter(); py_bubblesort(data); t1 = time.perf_counter()
            check = check_sorted(data)
        elif name == "sieve":
            t0 = time.perf_counter(); fl = py_sieve(n); t1 = time.perf_counter()
            count = sum(fl)
            h = 0
            for k in range(1, n+1):
                if fl[k]: h = (h*31+k) % 1000000007
            check = "%d:%d:1" % (count, h)
        elif name == "fib":
            sys.setrecursionlimit(1000000)
            t0 = time.perf_counter(); r = py_fib(n); t1 = time.perf_counter()
            check = "%d:1" % r
        elif name == "sha256":
            t0 = time.perf_counter(); d = py_sha256_faithful(n); t1 = time.perf_counter()
            check = d  # 既に "w0:w1:...:w7" 形式 (xyo と同じ)
        elif name == "micro_loop":
            i = 0
            t0 = time.perf_counter()
            for _ in range(n): i += 1
            t1 = time.perf_counter()
            check = str(i)
        elif name == "micro_read":
            L = [7]*1000; s = 0; i = 0
            t0 = time.perf_counter()
            for _ in range(n):
                i += 1
                if i > 1000: i = 1
                s += L[i-1]
            t1 = time.perf_counter()
            check = str(s)
        elif name == "micro_write":
            L = [0]*1000; k = 0; i = 0
            t0 = time.perf_counter()
            for _ in range(n):
                i += 1
                if i > 1000: i = 1
                k += 1
                L[i-1] = k
            t1 = time.perf_counter()
            s = sum(L); check = str(s)
        elif name == "micro_append":
            t0 = time.perf_counter()
            L = []
            ap = L.append; k = 0
            for _ in range(n):
                k += 1; ap(k)
            t1 = time.perf_counter()
            check = str(len(L))
        ts.append((t1-t0)*1000)
    return statistics.median(ts), ts, check

def run_xyo(xyo, path):
    e = dict(os.environ); e["XYO_DUMPVARS"] = "json"
    t0 = time.perf_counter()
    p = subprocess.run([xyo, "run", path, "--jit", "hotspot"],
                       capture_output=True, env=e, timeout=600)
    wall = (time.perf_counter()-t0)*1000
    if p.returncode != 0:
        raise RuntimeError("xyo failed: " + p.stderr.decode()[-500:])
    text = p.stdout.decode("utf-8", "replace")
    data = json.loads(text[text.index("{"):text.rindex("}")+1])
    b = data["Bench"]
    times = []
    for x in b.get("times", []):
        try: times.append(float(x))
        except (TypeError, ValueError): pass
    return times, str(b.get("check")), wall

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--medium", action="store_true")
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--py-reps", type=int, default=3)
    ap.add_argument("--algo", default="")
    ap.add_argument("--sizes", default="")
    ap.add_argument("--json", default=os.path.join(HERE, "results-vs-python.json"))
    args = ap.parse_args()
    MEDIUM_SIZES = {
        "quicksort": 50000, "mergesort": 50000, "bubblesort": 2000,
        "sieve": 1000000, "fib": 29, "sha256": 16384,
        "micro_loop": 5000000, "micro_read": 5000000,
        "micro_write": 5000000, "micro_append": 5000000,
    }
    if args.medium:
        sizes = dict(MEDIUM_SIZES)
    else:
        sizes = dict(QUICK_SIZES if args.quick else FULL_SIZES)
    for kv in filter(None, args.sizes.split(",")):
        k, v = kv.split("="); sizes[k] = int(v)
    if args.algo:
        sizes = {k: v for k, v in sizes.items() if k in args.algo.split(",")}

    # fixture 生成 (algo + micro を一括で: manifest 上書き対策)
    all_names = list(sizes.keys())
    gen_cmd = [sys.executable, GEN, OUT, "--sizes",
               ",".join("%s=%d" % (k, sizes[k]) for k in all_names),
               "--only", ",".join(all_names)]
    subprocess.run(gen_cmd, check=True, stdout=subprocess.DEVNULL)
    manifest = json.load(open(os.path.join(OUT, "manifest.json")))

    print("=== xyo-cpp vs Python 対決 (%s) ===" % ("quick" if args.quick else ("medium" if args.medium else "full")))
    print("xyo=%s  python=%s" % (args.xyo, sys.version.split()[0]))
    rows = []
    for name, n in sizes.items():
        # xyo
        x_times, x_check, x_wall, x_med, x_ok = [], "?", 0, float("nan"), False
        if name in manifest:
            path = os.path.join(OUT, manifest[name]["file"])
            x_times, x_check, x_wall = run_xyo(args.xyo, path)
            if x_times:
                # days since 2000 は ms 量子化: 中央値を取る
                x_med = statistics.median(x_times)
            x_ok = (x_check == manifest[name]["expected_check"])
        py_med, py_ts, py_check = run_python(name, n, args.py_reps)
        if name == "sha256":
            # xyo の check は 10 進 8 ワード、Python も同じ形式で比較
            py_ok = (str(py_check) == str(manifest.get(name, {}).get("expected_check")))
        else:
            py_ok = (str(py_check) == str(manifest.get(name, {}).get("expected_check", py_check)))
        ratio = py_med / x_med if x_med and x_med > 0 else float("nan")
        winner = "xyo" if x_med < py_med else "Python"
        print("  %-11s n=%-8d xyo %8.1f ms | py %8.1f ms | py/xyo %6.2fx -> %s %s" % (
            name, n, x_med, py_med, ratio, winner, "" if (x_ok and py_ok) else " !!CHECK!!"))
        rows.append({"algo": name, "n": n, "xyo_ms": x_med, "py_ms": py_med,
                     "ratio": ratio, "xyo_check_ok": x_ok, "py_check_ok": py_ok,
                     "xyo_times": x_times, "py_times": py_ts})
    print("\n--- duel table (ms, 小さい方が速い) ---")
    print("%-11s %9s %10s %10s %8s  winner" % ("algorithm", "n", "xyo JIT", "python", "py/xyo"))
    for r in rows:
        w = "xyo" if r["xyo_ms"] < r["py_ms"] else "Python"
        print("%-11s %9d %10.1f %10.1f %8.2f  %s" % (r["algo"], r["n"], r["xyo_ms"], r["py_ms"], r["ratio"], w))
    print("\npy/xyo > 1 は xyo が速い (Python時間 / xyo時間)")
    if args.json and args.json != "-":
        json.dump({"sizes": sizes, "rows": rows}, open(args.json, "w"), indent=1)
        print("wrote", args.json)

if __name__ == "__main__":
    main()
