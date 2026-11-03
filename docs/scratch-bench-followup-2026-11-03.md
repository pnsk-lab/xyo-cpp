# ScratchBench追試：再帰JITのスタックフレーム省略（2026-11-03）

## 実験条件

- xyo-cpp: `main` / v0.5.0 の `09883aa` からのローカル変更、i7-12700、Linux、LLVM JIT HotSpot
- ScratchBench: 同梱 `scratch-bench/`、performance source hash `e8bd5be3e7345088`、47項目
- xyoの比較対象: 同じRelease設定（`XYO_DISABLE_RT_PROF=ON`）で再帰最適化前1回・後2回の中央値
- TurboWarp: 2.1.46（VM commit `f26ff67`）、Node 26、compiler有効、手動直接実行1回
- 同梱の `bun run bench` では今回TurboWarpが0終了してもDONEが得られず失敗判定となったため、`BENCH_COMPILER=1 node runtimes/node-vm.mjs turbowarp .cache/build/performance.sb3` で直接出力を取得した。生ログ `build/twmanual.log` に全47結果とDONEがある
- 性能は環境負荷や測定ノイズで動く。TurboWarp比較は1回なので有意差ではなく改善箇所の探索用
- schedulerは実時間依存で、checksumが一致しないこと自体は異常ではない

## 変更：純粋なwarp再帰のScratchフレーム省略

JIT再帰のたびに呼ばれる `xyo_proc_push` / `xyo_proc_pop` は、Scratchの6祖先までの再帰判定・warpMode・中断中状態のために必要な場合がある。しかし、次の条件をすべて満たす場合に限り、native callの外にあるScratchスタックにフレームを作る必要がない。

- 呼び出しグラフから到達する全手続きがwarp定義である
- 直接のyield/compat呼出しがない
- 手続き内の命令が、スカラー変数・算術・比較・if・引数レポータ・手続き呼び出しの保守的な許可集合に限られる
- 再帰呼び出し先とその先も同じ条件を満たす

`src/jit/codegen.cpp` の `canElideWarpRecursiveFrames` がこの判定を行う。それ以外の再帰は従来どおりフレームを維持する。yield判定は引き続き保守的。

### 実測

| 項目 | 変更前 ops/ms | 変更後 ops/ms | 高速化 |
|---|---:|---:|---:|
| `algo_fib` | 44,518 | 408,088 | **9.17倍** |
| `algo_ack` | 41,243 | 179,421 | **4.35倍** |
| `algo_primes` | 14,819 | 15,276 | 1.03倍（ノイズ域） |
| `str_join` | 61,526 | 62,119 | 1.01倍（ノイズ域） |
| `math_arith` | 91,103 | 89,760 | 0.99倍（ノイズ域） |

決定論的な全項目で変更前後のchecksum一致を確認（schedulerの時間依存項目は除く）。

`algo_primes`向けにJITの剰余高速経路へ分岐ヒントを付ける実験も行ったが、13,887→13,739 ops/ms（0.99倍）で改善しなかったので撤回した。

## 改善後にも残る弱点（TurboWarpの直接実行1回との比較）

| 項目 | xyo / TurboWarp | 優先調査 |
|---|---:|---|
| `algo_primes` | 0.261倍 | 可変除数の整数剰余、タグ判定、レジスタ割当 |
| `sched_waituntil` | 0.274倍 | wait until評価と再開のスケジューリング経路 |
| `sched_threads` | 0.310倍 | スレッド生成・開始・回収の固定費 |
| `app_raycast` | 0.341倍 | 演算・分岐・配列アクセスの内訳 |
| `str_join` | 0.350倍 | cons生成、GCスラブの確保・sweep |
| `math_arith` | 0.418倍 | f64タグ付け、剰余、SSA値のスピル |
| `app_brainfuck` | 0.490倍 | 分岐・リストアクセス・入力変換 |

改善後にTurboWarpより速い代表例: `algo_fib` 2.77倍、`algo_ack` 1.70倍、`list_insdel` 2.02倍、`str_contains` 1.83倍。ベンチの時間依存項目は参考値。

## 他の改善・テスト

- `src/core/string_util.cpp`: `SStr::isAscii` を深いropeでもスタックオーバーフローしない反復的postorder計算に変更し、atomic fetch_orでキャッシュを公開
- Unicodeのlowercase展開（例: `İ` → `i` + 合成点）で `contains` が誤判定する早すぎる長さチェックを修正
- `tests/unit/test_sstr_cons.cpp`: 60,000段の左右深いrope、非ASCIIの伝播、Unicode展開の回帰テスト追加
- `CMakeLists.txt`: Releaseの既定では実行時プロファイラをコンパイル除去。診断したいときは `-DXYO_DISABLE_RT_PROF=OFF`。RelWithDebInfoの既定は従来どおり維持
- 過去の15ブロック/1200測定の要因実験 `docs/string-factorial.md` ではプロファイラ除去が文字列10種の幾何平均で約5.6%高速化

## 再現手順

```sh
cd scratch-bench
XYO_SOURCE=source bun run bench --runtime xyo-cpp --suite performance --runs 2 --no-variants --offline
BENCH_COMPILER=1 node runtimes/node-vm.mjs turbowarp .cache/build/performance.sb3
```

保存したローカルの結果ディレクトリ:

- `scratch-bench/results/xyo-release-prof-off-baseline/`
- `scratch-bench/results/xyo-release-warp-recursive/`
- `scratch-bench/results/xyo-release-recursion-parity/`
- `build/release-baseline.log` / `build/release-warp-recursive.log`

パリティ追試は2122/2123成功。最適化前も同じ2122/2123、同じMotion: If On Edge Bounceの1件だったので、今回の変更による回帰は確認されていない。CTestも6/6成功。
