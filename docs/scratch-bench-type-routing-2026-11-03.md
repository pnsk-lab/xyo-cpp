# ScratchBench: JIT変数の型経路（2026-11-03）

対象: `scratch-bench/performance/project` とローカルの `build/xyo` （当日の未コミット変更を含む）。診断用の一時的なLLVMコード生成ログと、JITガードの実行時カウンタで確認した。計測用のソースコードへの挿入は作業後に除去する。

## 要点

- 全体スイートのトップスクリプトをJITで生成する際、74個の手続きについて経路を特定できた。これは「生成された経路」であり、すべてが実際に実行されたことを意味しない。
- `math_arith` の `k_a/k_b/k_i`、`algo_primes` の `k_i/k_j/k_n/k_acc`、`app_raycast` の22変数など、内側の主要な計算では従来の **ループ内数値影（f64）** が多い。
- 一方、`sched_*`、`algo_fib`、`algo_ack`、`list_search` などには **今回の型プロファイル用数値分岐** が生成される。
- `str_join` の `k_s` は **文字列のBoxed昇格**、`str_contains` の `k_s` もBoxed昇格。この変数に数値特化は選ばれない。
- `list_read` は数値影（`k_i`）だけでなく、**リストヘッダ昇格** も使用している。
- プロファイル有効時はNaN-boxガード後に数値変換。型が不一致ならScratchの一般変換・比較へ戻る。既存の数値影はこれより優先される。

## コンパイル時の観測（主にステージ上の手続き）

| 分類 | 主な適用先 |
| --- | --- |
| ループ内数値影 | `loops_until/while/foreach/ifelse`、`vars_setchange/many`、`math_arith/round/trig/nbody/mandel/collatz`、`algo_sort/sieve/matmul/primes`、`app_life/raycast/projection` |
| 今回の型予測も併用 | 上記各手続きの主にタイマ `b_t0`・予算 `b_budget`・一部の外側変数 |
| 型予測を使うが数値影が少ない | `algo_fib/ack`、`list_append/insdel/search`、`sched_*` |
| 文字列Boxed昇格 | `str_join` の `k_s`、`str_contains` の `k_s`、`str_cast` の `k_t`、`app_brainfuck` の `bf_c` |
| 数値影とBoxed昇格の混在 | `app_brainfuck`（例：`bf_dp` は数値影、`bf_c` はBoxed） |

`k_*` の各手続き内で選択された型予測は、SB3初期値（数値0など）に基づく場合もある。HotSpotはループを持つスクリプトを初回起動時からJITするため、十分なインタプリタ実行を経た型プロファイルとは限らない。

## 実際の分岐回数（独立した単独SB3での実行）

列は `profile.number / profile.other / 比較の数値判定成功 / 比較の汎用判定` の順。**各ベンチを単独に切り出した計測の実測値であり、全体スイートでの累計ではない**。

| カーネル | 数値変換fast | 変換fallback | 数値比較fast | 比較fallback |
| --- | ---: | ---: | ---: | ---: |
| `math_arith` | 23,611 | 7 | 11,809 | 0 |
| `algo_primes` | 113,292 | 7 | 56,653 | 0 |
| `app_raycast` | 228,741 | 70 | 60,610 | 504 |
| `str_join` | 2,652 | 7 | 1,333 | 0 |
| `sched_threads` | 49,063 | 0 | 49,068 | 5 |
| `sched_waituntil` | 77,830 | 5 | 38,925 | 5 |
| `list_search` | 2,642,400 | 17,630 | 8,815 | 0 |
| `app_brainfuck` | 899,458 | 14,284 | 14,284 | 0 |

ここでの `str_join` のfast回数は、主に外周の計時・ループ制御に関する数値であり、`k_s = k_s ++ "ab"` の2万回反復が数値特化されたという意味ではない。

## JIT variantの検証

`algo_primes` の同じ単独SB3について、
`--jit hotspot` と `--jit eager` は `XYO_JIT_TYPE_PROFILE=1` で型予測分岐を生成し、`=0` で生成しなかった。`--jit off` ではどちらも型予測分岐がない。6通りすべてのチェックサムは `110` で一致した。AOTは設計上、実行時型予測を埋め込まない。

## 制約と残る問題

一時的に型追跡を埋め込んだバイナリでは全体スイート実行がセグメンテーション違反で停止したため、全体スイートの「ガード実行回数」は取得できていない。一方、**計測用コードを取り除いて再ビルドした通常バイナリでは、全体スイート（47カーネル + ヘッダ/DONE）を最後まで完走し、終了コード0を確認した**。型追跡あり単独SB3では複数のカーネルが完走した。コンパイル時の経路マッピングは `build/type-trace-full.err`、単独実行の記録は `build/type-route-isolated.json`、`build/type-route-isolated-additional.json` などにある。

`scratch-bench/src/runtimes/xyo.ts` は標準でGitHubリリース版を使用する。ローカル実装を正式ベンチで使う場合は `XYO_BIN=/absolute/path/to/build/xyo` または `XYO_SOURCE=source` を指定すること。ベンチのJIT/Eager/Interpreter/AOTのvariantを混同しない。

**改善の優先度**: ループ内の数値影のデータ依存・変換コスト、文字列用Boxed経路、数値演算そのものの命令数をまず見ること。タイマ用の型ガードの改善だけでは最内周ループへの効果が限定的。
