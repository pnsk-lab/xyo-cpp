# JIT算術SSAでNaN正準化を省く試験（2026-11-03）

## 狙い

ScratchBenchの `math_arith` は通常の `xyo_to_num`、`xyo_eq` の呼び出しを伴わない数値計算でもTurboWarpに遅れていた。従来は個々の `operator_add/subtract/multiply/divide` に対して、結果がNaNかどうかの `fcmp uno` と正準化処理を生成していた。

今回、Scratchブロックの特別扱いはせず、生成途中の **LLVM f64 SSA値についての保守的な数値性質** を伝播し、NaNが数学的に不可能な場合だけ余分な正準化を生成しない。

## 実装

変更箇所: `src/jit/codegen.cpp`。

- `arithmeticFinite(v)`: 有限なLLVM浮動小数点定数、または有限性を確実に証明したSSA値だけをtrueとする。
- `arithmeticFiniteNonzero(v)`: 有限かつ非ゼロの浮動小数点定数だけをtrueとする。
- `emitCanonKnownNonNaN(d)`: 通常のNaN正準化を省き `bitcast` したboxed Valueを返し、既存の `knownNum/knownNumD/knownNumOk` の不変条件を維持する。
- `emitArithNumeric` は `emitToNumF64` 済みの非NaN引数について、加減算なら片側が有限、乗算なら両側が有限または一方が有限非ゼロ定数、除算なら除数が有限非ゼロ定数の場合に特化する。
- moduloは一般にNaNを生むので既存の `emitCanon` を維持する。一方、modulo後の **toNumberの結果は有限**（NaNならScratch正準値として次のtoNumberで0になる）という事実を、そのSSA値にだけ記録する。

`XYO_JIT_SAFE_ARITH_CANON=0` を設定してJIT/AOTコンパイルすると、特化を無効化できる。変数を数値だと仮定するのではなく、式の各オペランドがScratch変換後に非NaNであることを使うため、ランタイムの型ガードや外部書込みに依存しない。値域情報は可変なScratch変数の再読込をまたいで伝播させない。

**未実装**: ループ帰納変数や各変数の整数値域を証明して `i32` 演算へ移す全面的な値域解析。今回の改善はその前提となるNaN/有限性の転送である。

## 同一Releaseバイナリの比較

Intel i7-12700、CPU 12固定、ScratchBenchの各カーネルを同じソースから単独SB3へ複製し、同一実行ファイルで `XYO_JIT_SAFE_ARITH_CANON=0` と既定ONを比較。各ケース8組、組内ON/OFFを交互に入れ替え、組ごとのops/ms比の中央値を示す。計測には各SB3内のタイマを使い、起動・コンパイル時間は含まない。全チェックサム一致。元のScratchBenchの測定内容は変更していない。

| ワークロード | ON/OFF速度比 |
| --- | ---: |
| `math_arith` | **1.127倍** |
| `math_round` | **1.142倍** |
| `math_nbody` | **1.153倍** |
| `app_raycast` | **1.123倍** |
| `list_read` | **1.219倍** |
| `algo_sort` | **1.164倍** |
| `algo_primes` | **0.953倍（約4.7%回帰）** |
| `str_join` | 0.987倍（実質変化なし） |

統計的に厳密な改善率を主張するには、より長時間のワークロードと複数ブロック間の不確実性評価が必要。プロジェクト固有の最適化条件は足していない。特に `algo_primes` の回帰は今後のコード生成調査対象。

生データ: `build/finite-paired-controlled.json`、実行ログ `build/finite-paired-controlled.log`。別バイナリ間比較の参考値は `build/finite-arith-ab.json`、同一バイナリの非ペア比較は `build/finite-arith-controlled-ab.json`。

## 生成機械語の変化

同一の `math_arith` 単独SB3で、`XYO_JIT_DUMP2=1` と `XYO_JIT_DUMP_OBJECT` により最適化済みLLVM IRとORC JITオブジェクトを保存した。対象手続き `xyo_script_0_p2` の静的計数:

| 指標 | OFF | ON |
| --- | ---: | ---: |
| LLVM IR行数 | 865 | 857 |
| `fcmp ord` | 31 | 27 |
| 機械語命令数 | 895 | 869 |
| `movsd` | 45 | 28 |
| `callq` | 24 | 24 |

これらは静的な命令数であり、実行された回数ではない。結果は `build/finite-ssa-{on,off}.ll`、`build/finite-ssa-{on,off}.asm` に保存。

## パリティ・全体実行・安全性

- `tests/unit/test_type_profile.cpp` を拡張し、加減乗除（0乗算・0除算のNaN事例を含む）、`(x % 3) + 2`、文字列・boolean・NaN・±Infinity・-0を interpreter / JIT のフィードバックON/OFFで照合。
- `ctest --test-dir build --output-on-failure -j4`: **最終ビルドでは7/7成功**（途中の検証時は6/6）。
- `XYO_BIN=.../build/probe-finite-optimized-xyo bun run bench --runtime xyo-cpp --suite parity --no-variants --offline`: HotSpot JIT **2123/2123成功**。当日の他の未コミット変更も含むリリースビルドでの結果であることに注意。
- オリジナルのScratchBench performance.sb3はON/OFF双方とも **全47カーネル完走**。全39非scheduler項目についてchecksum一致（schedulerは実時間依存のため除外）。

差し戻しが必要なら `XYO_JIT_SAFE_ARITH_CANON=0` で比較できる。既存のワークツリーにある他者・他ターンの未コミット変更は保持し、コミット／pushは行っていない。
