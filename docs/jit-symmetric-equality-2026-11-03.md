# JIT等値比較の左右対称高速化（2026-11-03）

## 採用変更

`src/jit/codegen.cpp` の `emitEq` を変更。既存の「数値リテラルが右辺で、左辺には変数の数値SSA/比較可能性ビットが存在する」経路を、**数値リテラルが左辺のときにも**使えるようにした。

入力ブロックは従来どおり左から右へ順序通り評価してから、コンパイル済みの数値変換結果 `knownNumD` と `knownNumOk` を使う。型が数値比較に参加できない場合（非数値文字列、NaNなど）は `xyo_eq` にフォールバックする。数値型プロファイルを型保証として扱わない。

診断用のオプトアウトは `XYO_JIT_DISABLE_EQ_SYMMETRIC=1`。従来からの `XYO_JIT_DISABLE_EQ_NUMERIC` も引き続き有効。

## 性能検証

既存ScratchBenchの素数判定を単独実行したSB3を用意。さらに `k_n == 0` のオペランド順序だけを `0 == k_n` に反転した診断用SB3を作成。計算式の意味とチェックサムは変わらない。

同じビルドの `build/jit-eq-proven-frozen-xyo` を使用し、8回の交互ON/OFF比較をCPU10固定で実行。候補に含まれていた別の「動的SSAペア比較」は `XYO_JIT_DISABLE_EQ_SHADOW_PAIR=1` で両条件とも無効にし、**左右対称化の効果だけ**を検証した。

| 条件 | Symmetric OFF | Symmetric ON | ON/OFF |
| --- | ---: | ---: | ---: |
| `0 == k_n` へ反転 | 50,626 ops/ms | 54,714 ops/ms | **1.0807倍** |
| オリジナル `k_n == 0` | 54,859 ops/ms | 55,025 ops/ms | **1.0030倍** |

各8回中央値、チェックサム110がすべて一致。元のScratchBenchファイルは変更していない。生成した診断用ファイルは `build/scratchbench-cause-probes/projects/primes_reversed.sb3`、測定記録は `build/jit-eq-symmetric-controlled-20261103.json` と `.log`。

## 不採用の試行

- **一般の動的値に対するraw double型ガード**: 全体で大きな遅延を起こし不採用。
- **型プロファイルのある動的値だけへのガード**: 素数判定・レイキャスト・算術で効果は概ね±1%程度。独立した4回ペアでは有意な改善を確認できず不採用。
- **両辺の動的SSA数値キャッシュを使うガード付き比較**: 隔離した文字列比較で約6%改善したが、全39項目の幾何平均は約0.997倍、ループ系に複数の回帰があり不採用。
- **非NaNが静的に証明できる両辺への限定**: 隔離した6ワークロード4回比較で有意な汎用改善がなく、複雑度に見合わないため不採用。

最終採用コードは左右対称のリテラル比較のみ。

## 意味論のテスト

`tests/unit/test_type_profile.cpp` に、`0 == raw` と `raw == 0`、文字列リテラル `"0"` を使った左右比較、および `raw == x` の動的比較を追加。Eager JITの型フィードバック有無・コンパイル後の値変更と、Interpreterの結果を照合。NaN、±Infinity、±0、真偽値、数値文字列、非数値文字列等を含む。

最終コードの CTest: **7/7 PASS**（`build/jit-symmetric-final-ctest-20261103.log`）。
HotSpot JIT Parity: **2123/2123 PASS**（`build/jit-symmetric-final-hotspot-parity-20261103.log`）。
Eager JIT Parity は4回中2回 2123/2123、2回 2122/2123。失敗したのは確率的な `Operators: Pick Random Rules 2 #5` で、左右対称最適化を無効にした同一バイナリの対照3回でも1回再現。**この失敗は今回の変更による新規回帰とは確認されず、乱数テストの安定性は引き続き調査対象**。ログ `build/jit-symmetric-final-eager-*.log`、`build/jit-symmetric-off-eager-*.log`。
最終HotSpot JIT性能 `build/jit-symmetric-final-performance-20261103.log` は47項目完走、非scheduler39項目でチェックサム39/39一致。既存の別プロセス測定との幾何平均比は約1.009倍にとどまり、全体速度の有意な改善は主張しない。

未コミットの既存作業を維持。コミット、タグ、pushは行っていない。
