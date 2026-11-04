# v1.0.0 向け JIT 数値SSA最適化（2026-11-04）

## 実装範囲

`src/jit/codegen.cpp` に3つの一般的なコード生成最適化を追加。**いずれも既定で有効**だが、個別に従来経路へ戻せる。最適化に使う証明はLLVM IR内の不変なSSA値と、厳密な浮動小数点境界に限られ、実行時のScratch変数の観測値を無条件の型保証に転用しない。

### 1. 範囲ガードされた32bit整数剰余の符号付き変換

`XYO_JIT_MOD_SIGNED_I32=0` で無効化。

元の `fptoui.i32` を `fptosi.i32` に変更。変換前に必ず `0 < dividend <= INT32_MAX` および `1 <= divisor <= INT32_MAX` をチェックするため、符号付き32bit整数と符号なし32bit整数の変換結果が一致することは数学的に保証される。小数・範囲外・負数・負のゼロ・ゼロ除数は元の広域整数または浮動小数点経路に落とし、Scratchの剰余意味論とNaN正準化を維持する。

同一Releaseバイナリの設定ON/OFF、CPU12固定、1項目ずつ切り出した無改変ScratchBench、8組の交互測定。値は各ペアの `ops/ms` 比の中央値（1より大きいとONが高速）。

| 項目 | ON/OFF |
|---|---:|
| `math_arith` | **1.0445** |
| `algo_primes` | **1.0156** |
| `algo_sort` | 0.9978 |
| `app_raycast` | 1.0057 |
| `math_expsqrt` | 1.0048 |
| `list_read` | 0.9808 |
| `algo_matmul` | 1.0001 |
| `app_brainfuck` | 1.0030 |

`math_arith` は8ペアすべてでONが高速。既存 `list_read` で約2%の回帰が見られ、切替は残す。実行条件・生データは `build/v1-jit-signed32-paired.json` と `build/v1-jit-signed32-paired.log`。

### 2. 有限算術結果を変数更新に伝播

`XYO_JIT_FINITE_ARITH_DELTA=0` で無効化。

`data_changevariableby` では、JITが昇格させた数値変数のShadowが常にNaNではないことを保証する。従来は変数更新のデルタがリテラルか特定の誘導変数の場合にのみ、加算後のNaN修復を省略できた。ここに `arithmeticFinite(fb)` の証明を使う経路を追加した。

例えば`i % 7`は、ゼロ除数や無限大の計算を含めてもScratchのNaN正準化の後は必ず有限。有限のデルタとNaNではない累算値の足し算はNaNを発生させない（有限＋±∞=±∞は有効な値）。このときだけ元のNaN修復を省く。

### 3. SSA上の有限絶対値上界（区間解析の初段階）

`XYO_JIT_ARITH_RANGE_FACTS=0` で無効化。

LLVM f64 SSAに対して、有限な絶対値の上界 `|x| <= B` をコンパイル時に追跡する。定数から始め、次の演算について保守的な値域伝播を行う。

- `add/sub`: `B=A+B`
- `mul`: `B=A*B`
- `div`: 分母がゼロではない有限定数に限り `B=A/|d|`
- `mod`: 有限な除数上界 `B` があれば、Scratchの剰余調整・NaN修復後の結果は `|result|<=B`

計算は`long double`を使い、上界を保存する際は `std::nextafter(..., +Infinity)` で切り上げる。絶対値が `DBL_MAX/4` に達する境界の証明は放棄する。したがって通常の丸め誤差による上界の過小評価と、オーバーフローによるInfinity生成を回避する。明確な証明ができない演算結果は従来コードのまま。

記録は元のScratch変数や型プロファイルへは一切結びつけず、`knownNumD`に属するSSA値にのみ関連付ける。yield後の変数読み直しには既存の通常処理を適用する。

組み合わせ例： `sum += (i % 7) * 31 + 1`。通常は剰余が有限と分かってもその後の算術の有限性が失われるが、区間解析により式全体が有限と証明されるので、変数更新側のNaN判定を省ける。

## 診断用Scratchプロジェクトでの測定

`tests/bench/algos/gen_jit_finite_delta.py` で生成し、`XYO_DUMPVARS=json`で最終値を検証。3000万反復、warp、同じRelease実行ファイル、CPU12固定。3回のプロジェクト内計時値は約1ms刻みなので、このデータは大規模ベンチの統計的な精密推定ではない。

| 経路 | OFF | ON | チェックサム |
|---|---:|---:|---|
| `sum += i%7` に有限デルタ伝播 | 68〜69ms | 53〜54ms | 89999997 |
| `sum += (i%7)*31+1` に区間解析＋有限デルタ伝播 | 71ms | 56〜57ms | 2819999907 |

`sum += (i%7)*31+1` で有限デルタ伝播**のみ**ON・区間解析OFFは72〜73msであり、式全体への伝播が実際に必要だったことも確認した。

生ログ: `build/v1-jit-finite-delta-large-{0,1}.out`、`build/v1-jit-finite-delta-affine-{00,10,11}.out`。

## 回帰・意味論検証

- `tests/differential/jit_arith_range.py`: 正負0、±小数、32bit/53bit境界、最大級有限値、数値文字列、非数値文字列、±Infinity、ゼロ・負数・極小・巨大除数の**20ケース×4方式=80実行一致**。比較対象はInterpreter、旧JIT（有限事実OFF）、新HotSpot、新Eager。ログ `build/v1-jit-arith-edge.log`。
- 全CTest：有限事実と値域解析をONにして **8/8成功**（`build/v1-jit-range-ctest.log`）。
- ScratchBenchパリティ：HotSpot / Interpreter / Eager / AOT それぞれ **2123/2123**（`build/v1-jit-range-parity.log`）。
- ScratchBench performance 原文47項目を最適化ON/OFF両方で完走。時間依存スケジューラを除く**39項目のチェックサム一致**（`build/v1-jit-range-performance-{on,off}.out`）。
- 隔離した主要8項目を6組の交互A/Bで実測した結果、区間解析＋有限デルタ伝播の追加効果は既存ケースでは概ね±1%で、目立つ回帰は確認されなかった（`build/v1-jit-range-paired.json`）。
- 環境変数はJITコード生成時に読み込むだけで、生成されたホットループにモード選択分岐は発生しない。

## 不採用: 定数除数の64bitフォールバック共通関数化

広域整数・浮動小数点の定数除数フォールバックを共通の低頻度関数へ分離する試作も行った。しかし8ペアの結果で `algo_sort` +2.7%に対し `math_arith` -3.5%、`app_brainfuck` -4.4%。一般に採用できないので試作を**完全に撤回**した。測定生データ: `build/v1-jit-const-outline-paired.json`。

## v1.0.0 の追加拡張候補

- **ループ誘導変数の区間SSA:** yieldや別スクリプト更新を跨ぐ場合の証明失効を明示し、型付き整数値をループ内の演算・比較・剰余へ直接運ぶ。現在の区間解析は式内で完結し、可変Scratch変数を跨ぐ不変量の証明はまだ実装していない。
- **単相型のガード付き演算特化:** ランタイム型フィードバックを証明と分離し、数値/文字列専用JIT本体から、型変更時にはdeoptではなく共通フォールバックへ移る。
- **LLVM最適化の階層化:** 手続き単位の最適化レベル、コードサイズ制限、コンパイルコスト/実行時間の定量評価。既存のLazy ORC関連コードと整合性を取る。
- **文字列JITのinline alloc/escape:** consを大量生成する文字列演算について、GC保護と移動可能性を維持した安全な高速割当ABIを設計する。

既存の他担当/並行作業による`src/jit/jit.cpp`（Lazy ORC）や`src/jit/codegen.cpp`（整数性事実・Math.round）の試作は本記録とは独立しており、無断で上書き・削除しない。v1.0.0のタグは未作成。Gitコミット・pushも実施していない。
