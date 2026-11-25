# v1.0.0: JIT数値最適化の拡張（2026-11-04）

## 実装の範囲

今回の変更は、特定のScratchBenchのブロック構成を照合するものではなく、**数値を生成するScratchの演算そのもの**に対するLLVM IRコード生成の拡張である。

変更: `src/jit/codegen.cpp`。単体テストの拡張: `tests/unit/test_type_profile.cpp`。独立した診断ベンチ: `tests/bench/algos/bench_jit_integral_facts.py`。

### 1. Math.round: hardware round-to-even + half-tie correction

元の `operator_round` は毎回 `xyo_round` ヘルパを呼んだ。単純な `floor(x + 0.5)` は負のゼロや境界値で互換性が破れる。LLVMでそのままjsRoundを分岐つきで展開した試作は `math_round` が約2.6%遅くなったので撤回した。

採用した方法:

1. `even = llvm.roundeven(x)`。ホストx86-64では実コードに `vroundsd` を確認。
2. `x - even == 0.5` に該当する「四捨五入すると大きい側へ丸めるべき、しかし偶数丸めでは小さい側へ落ちた」入力でだけ `even + 1.0` を採用。
3. その他は `even` のまま。負のゼロ、±Infinity、巨大な整数は保持。LLVMの通常FP演算であり、fast-mathは設定しない。

`emitToNumF64()` はNaNをScratchの規則に合わせて0へ直している。通常の `operator_round` は非NaN入力のみ。単純な符号・比較よりも、端数ちょうど0.5を検出することが重要。

- 既定 **ON**、`XYO_JIT_ROUND_INLINE=0` で旧ヘルパ呼出しに戻す。
- 同一Releaseバイナリ、CPU 12、8ペアの `math_round` 速度比 **1.3646倍**（ペア比分布 1.488, 1.364, 1.365, 1.361, 1.341, 1.372, 1.364, 1.366）。生ログ: `build/v1-jit-roundeven-paired.log`。
- 最初の `floor(x+0.5)` 直接展開は **0.9738倍**（不採用）。`build/v1-jit-round-paired.log`。

### 2. 有限なら整数というSSA性質の伝播

`integralWhenFinite` は各LLVMの **f64 SSA値**の集合。意味は「結果が有限ならIEEE doubleで正確な整数」。正か負か、値の大きさ、あるいは可変Scratch変数の型を勝手に仮定しない。

**証明元:**

- `operator_round`, `floor`, `ceiling`: 有限な結果は常に整数。これらのNaN正準化に付随する `select(isnan,0,...) `も整数性を保つ。
- 有限で整数のLLVM定数。
- 両オペランドが整数の加算・減算・乗算・剰余（NaNをScratch式の正準化で0へ戻したときも整数）。
- 整数を±1で除算。
- 別スクリプトのメモリに保持する変数や、インタプリタからの読み出し・型プロファイリングの値には伝播しない。

**使い方:** `operator_mod` の既存のガード（正の32bit範囲、浮動小数点負のゼロ、符号やNaNなど）を残し、そのガードの後で `fptoui/fptosi -> uitofp/sitofp -> fcmp oeq` と戻していた整数性の再確認だけを、SSAの整数証明がある場合に省く。入力範囲外では従来の64bit/浮動小数点フォールバックが必ず残る。

- 既定 **ON**、`XYO_JIT_INTEGRAL_FACTS=0` で以前の経路へ戻せる。
- `round(x) % 7` の生成IR: `fcmp oeq` 3→1、`uitofp` 2→1（JIT実オブジェクト保存済み）。
- オリジナルScratch演算だけで作った4つの診断プロジェクト。6ブロック、各ブロック3回のプロジェクト内計測の中央値:

| 組合せ | 旧実装（両OFF） | 丸めのみ最適化 | 丸め＋整数性伝播 |
|---|---:|---:|---:|
| `round(x) % 7` | 20 ms | 17 ms | 15 ms |
| `(round(x) + 3) % 5` | 22 ms | 18.5 ms | 16 ms |
| `floor(x) % 7` | 13 ms | 13 ms | 11 ms |
| `(round(x) + floor(x/3)) % 11` | 25 ms | 22 ms | 19 ms |

- 分離した診断用ベンチであり、実アプリ全般の加速率ではない。短い計測のためms単位の精度が低い。 `build/v1-jit-intfacts-ab.json` / `build/v1-jit-intfacts-ab.log`。

### 3. 非NaNを証明できる数値演算の正準化削除

`emitToNumF64` はNaNを0へ修復するので、その後の `round, abs, floor, ceil, atan, exp, pow(10,x)` はNaNを作らない。

- `emitCanonKnownNonNaN` により結果の `fcmp uno`、NaNボックス値の `select`、次回toNumberの `select` を省略。
- 一方、`sqrt`、`ln`、`log`、`tan`、`asin`、`acos`、`sin`、`cos` など、無効な入力がNaNに到達する可能性のある演算は従来の正準化を維持。
- 既定 **ON**、`XYO_JIT_PURE_MATH_FACTS=0` で従来の経路へ戻す。

同一Releaseバイナリ、8組の交互測定（丸め・整数SSAは両側ONに固定）で `math_round` **1.2137倍**、`math_expsqrt` **1.0447倍**、`app_raycast` **1.0596倍**。残りの複数ケースではほぼ差がない。生ログ: `build/v1-jit-mathfacts-paired.log`。

この約21%は「丸めをハードウェア命令にすること」の速度比とは独立して測定したもので、測定対象やノイズが異なるため厳密に単純な掛け算で総合速度比を出すことはできない。

## 回帰とパリティ検査

- `tests/unit/test_type_profile.cpp` にMath.round、丸めと剰余の合成、算術合成、6種類の非NaN数学関数を追加。
- 負のゼロ、±Infinity、NaN、ボックス化文字列、次の表現可能値 `nextafter(0.5, 0)` と `nextafter(-0.5,-1)`、2^52近傍、32bit境界、非正規化最小値を含む。
- 最適化ONのCTestは **8/8成功**（`build/v1-jit-final-ctest.log`）。
- ScratchBenchパリティは Interpreter / JIT HotSpot / JIT Eager / AOT が各 **2123/2123成功**（`build/v1-jit-mathfacts-parity.log`）。
- 元のScratchBench Performance（47項目）は、3つのスイッチをON/OFF両方に固定した2構成で **47項目すべて完走**、非スケジューラ39項目のchecksumが **39/39一致**。
- AOT `--obj` で生成したオブジェクトを検査し、`vroundsd` を確認。現ホストで未解決 `roundeven` libcallはなし。`build/v1-jit-round-aot.o`。AOTの生成先CPUへの移植性は別途ターゲット選択の検証を要する。

## 既定ONでの最終総合A/B（同一Releaseバイナリ）

`build/v1-jit-default-xyo` をCPU 12に固定し、3つの最適化をまとめて旧コード生成（3フラグすべて `=0`）へ切り替えて比較した。原本ScratchBenchの独立SB3をそのまま使用。8ペアの交互実行、ペアの速度比中央値。全ケースでチェックサム一致。

| ケース | 新JIT / 旧JIT |
|---|---:|
| `math_round` | **1.6674倍** |
| `math_expsqrt` | **1.0451倍** |
| `app_raycast` | **1.0634倍** |
| `list_read` | 1.0085倍 |
| `math_arith` | 1.0042倍 |
| `algo_sort` | 1.0006倍 |
| `str_join` | 1.0007倍 |
| `algo_primes` | 0.9979倍 |

`math_round` の各ペアの比は1.677, 1.662, 1.650, 1.691, 1.665, 1.670, 1.689, 1.661。数学関数の正準化削減も含めた総合値なので、別々の速度比を掛け合わせた推測ではない。他の差が数%以下のケースはノイズの可能性がある。

データ: `build/v1-jit-default-vs-old-paired.json`、`build/v1-jit-default-vs-old-paired.log`。既定ONのパリティを最終Releaseバイナリで再実行し、4経路すべて **2123/2123**。 `build/v1-jit-default-parity.log`。CTestもビルド競合解消後に **8/8成功**（`build/v1-jit-default-ctest-retry.log`）。性能スイート全47項目の完走、非scheduler39項目のchecksum一致も `build/v1-jit-full-{on,off}.out` で確認。

## 未実装・次の主要テーマ

- スクリプトをまたいで更新される可変変数に関する、ガード付きの整数SSA昇格とデオプト／復帰。
- ループ帰納変数の区間証明と、JIT生成のi32演算に原則乗せるTierの導入。yield、他スクリプト更新、動的な型変化を先に解決すること。
- LLVM関数単位での最適化予算配分と、遅延JITでのコード再コンパイル境界。
- 小さいScratchBenchを速くするだけでなく、本物のScratch作品、TurboWarp差分、起動・JITコンパイル費用を別々に計測する。

今回の変更はコミット、push、v1.0.0タグ作成を行っていない。
