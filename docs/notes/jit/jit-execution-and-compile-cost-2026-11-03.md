# JITの型付き真偽判定とコンパイル費用の調査（2026-11-03）

## 変更

### 数値SSAを真偽判定まで保持

`src/jit/codegen.cpp` の `emitTruthyV` に2経路を追加した。

1. **証明済みの数値**: `emitCanon` が発行する `knownNum` と `knownNumD` の組を使い、NaN-boxのタグ比較や `xyo_truthy` のfallback分岐を発行せず、`fcmp one double value, 0` に直接変換する。NaNは `knownNumD` で0へ正規化される。Infinityはtrue、+0/-0はfalse。
2. **型プロファイルによる数値予測**: `likelyNumLoads` では `bits < kNan`（signed）を満たす実際の数値だけを `fcmp one` で判定し、文字列・boolean・undefinedその他は `xyo_truthy` へ戻す。予測は型保証ではない。特にScratchの文字列`"0"`／`"false"`はfalseだが、`"00"`や`"0.0"`はtrueであり、数値への変換結果だけでは同等に扱えない。

`tests/unit/test_type_profile.cpp` を拡張し、`if (raw + 0)` と `if (raw)` の双方を通じて、Interpreter / Eager JIT（数値型プロファイルON/OFF）を比較する。コンパイル前後の型変化と `NaN`, `±Infinity`, `-0`, 数値文字列、空白、`false`等も確認する。

`build/jit-truthy-ir.ll`（最適化後LLVM IR）では、直接数値の条件が `fcmp` で処理され、動的に型が変わりうる条件にのみ `xyo_truthy` の呼び出しが残っている。

### JITコンパイル内訳の計測

`src/jit/jit.cpp` に `XYO_JIT_TIMING=1` を追加。各モジュールのIR生成・LLVM最適化・ORC登録・機械語生成とシンボル解決（lookup）にかかった時間、初期IRの関数数と命令数をstderrへ出す。通常時はログを出力しない。

`XYO_JIT_CODEGEN_OPT=less|none` は診断用の**明示的な**コンパイルオプション。IR最適化レベル（既定O3）とは独立して、ORCバックエンドの機械語最適化レベルだけを切り替える。既定動作は元のLLJITBuilderから変更しない。

## コンパイル時間の測定

同じi7-12700、Linux、同じScratchBenchパリティSB3（2123項目）。66個のJITモジュールをコンパイルした1回の測定。コンパイル時間のみで、プロジェクトロードなどは含めない。

| LLVM IR / backend | emit | LLVM opt | ORC submit | lookup | 合計 |
| --- | ---: | ---: | ---: | ---: | ---: |
| O3 / default | 147.0 ms | 1476.3 ms | 0.5 ms | 2028.4 ms | **3657.2 ms** |
| O2 / default | 140.4 ms | 1424.3 ms | 0.5 ms | 1960.1 ms | **3530.4 ms** |
| O3 / Less | 147.1 ms | 1448.0 ms | 0.7 ms | 2071.5 ms | **3674.4 ms** |
| O3 / None | 144.8 ms | 1415.7 ms | 0.4 ms | 334.9 ms | **1900.1 ms** |

最大の単一JITモジュール（トップブロックID `~qC`）がdefaultで3197.3msを占めた。このトップブロックは、parityプロジェクトのSprite1で、直接到達可能ブロック1011個と手続き呼出し275個を含む。詳細は `build/jit-timing-parity.log` ほか。

O2はO3に対して速度改善が小さい。機械語生成で最適化を止めると初回コンパイルを短縮するが、以下の通りその代償は大きい。

## コンパイル時間と実行速度の交換条件

同じPerformance SB3の47ワークロード（各1実行）について、`XYO_JIT_CODEGEN_OPT=none` と既定のJITの実行速度を比較。ワークロード固有の特化やSB3の書換えは一切しない。

- **None / default の幾何平均 = 0.537倍**（全47項目の等重み）。`loops_*` 7項目では0.278倍、`algo_*` 6項目では0.398倍、`str_*` 4項目では0.851倍。`sched_*` は実時間・スケジューラ依存のため解釈に注意する。
- 非scheduler項目のチェックサム不一致は発見されなかった。
- 生の結果は `build/jit-backend-perf-default.log` と `build/jit-backend-perf-none.log` に保存。
- 各設定1回なので細かいパーセント差を確証として扱わない。ただし複数の汎用ループでの大幅な低速化を考慮し、**機械語最適化Noneへの既定変更はしない**。

## 設計判断と次段階

- 現在のJITの実行時のボトルネックは、動的値の反復ガード／同期と文字列・GCが主な候補。証明済み数値・実測型フィードバック・型変化fallbackを区別するSSAベースの伝播を進める。
- コンパイル遅延は、LLVMの機械語生成と大きな単一モジュールに集中する。O2/Noneへ一律に下げると実行性能が悪化するため、**大きな手続きを分離し、ホットな関数をO3で、コールドな関数を軽量に処理する方式**またはtieringを検討する。複数スクリプトの同時コンパイルには現在の`ThreadSafeContext`とGCルートの競合・公開順序を精査する必要がある。
- 数値SSAのtruthiness改善は実装済みだが、実プロジェクト全体での速度向上量は独立したA/B測定をしていない。性能向上率を推測で記載しない。

## 検証

- `ctest --test-dir build --output-on-failure`: 6/6成功（真偽判定の変更後）
- `scratch-bench` official parity: 2123/2123（HotSpot）
- 参考: `build/jit-truthy-ssa-fixture.sb3` および `build/jit-truthy-ir.ll` はユニットテスト用のScratch入力から生成したIR確認用ファイル
