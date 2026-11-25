# performance.sb3 長時間実行でのメモリ急増 (2026-11-05)

同一SHA256 `e76c4a1a595e8a22af72a499a530300edb86a735ccc8701a12ba6cdb35623a93` の `debug/projects/performance.sb3` を Linux/x86_64 の `build/xyo` で測定。対象SB3は4 targets / 2310 blocks / 初期空のリスト22個。測定は `tools/profile_memory.py` が `smaps` をサンプリング。強制停止したものは未完走であり、パリティ成功を意味しない。

| モード | 結果 | 時間 | sampled peak RSS | heap at peak |
|---|---|---:|---:|---:|
| Interpreter, full run | 正常完走 | 32.0s | 116060 KiB | 66916 KiB |
| HotSpot auto, full run | RSS 1GiB limit | 14.9s | 1049036 KiB | 934852 KiB |
| HotSpot, list-limit=200000 | RSS 1GiB limit | 17.2s | 1050236 KiB | 937088 KiB |
| HotSpot, XYO_PARFOR=0 | RSS 1GiB limit | 27.3s | 1050332 KiB | 931376 KiB |
| HotSpot, lazy off, timing enabled | RSS 512MiB limit | 16.1s | 550496 KiB | 473292 KiB |

HotSpot auto では約14.22秒の 201604 KiB から14.87秒の1049036 KiBへ約0.65秒でRSSが急増。ほぼ `[heap]` の増加。 `XYO_GC_DEBUG=1` 指定しても試験停止まで収集ログがない（GCはこの経路で走っていない、あるいは到達していない可能性。未断定）。

`XYO_PARFOR=0` でも再現するため並列リスト予約処理が唯一の原因ではない。リスト要素数を20万に制限しても再現した。HotSpot `--jit-lazy off` でも再現したので強制遅延JIT特有の問題でもない。 `XYO_JIT_TIMING=1` では例として `~Mv` の funcs=80, ir=84584, emit=23ms, opt=737ms, lookup=1448ms と出た。これらはコンパイルコストの証拠だがヒープ急増の確定した原因ではない。

**次の切り分け:** `heaptrack` / `malloc` トレース / `gdb` で急増時の確保スタックを採取。GC heap, STL, LLVMコンパイル内部の区別がまだ必要。異常なリストgrowのほか、JITがScratchより多数回ループしている可能性も検証する。上限なしで実行するとOOMリスクが高いため再現時は必ずRSS上限を設定する。

計測データは `bench-output/memory-performance-{full-hotspot,full-interpreter,hotspot-listcap,hotspot-no-parfor,gc-debug,jit-lazy-off}.json`。
