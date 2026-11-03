# JIT可変除数の低頻度剰余経路アウトライン化（2026-11-03）

## 狙いと変更内容

`src/jit/codegen.cpp` の `operator_mod` は、可変除数で次の3経路を生成する。

1. 正の32bit整数の商・剰余（`urem i32`）
2. 32bit条件を満たさない正確な整数で、絶対値が2^52未満のときの `srem i64`
3. 小数・ゼロ・Infinity・その他はScratch互換の浮動小数点剰余 `frem` と符号補正

もともとは3経路を毎回のJIT手続き本体へインライン生成しており、32bitの高速経路がほぼ常に使われるプログラムでも、64bitフォールバックの分岐と演算コードがホット関数内に存在した。

今回は**32bit高速経路を変えず、経路2と3をまとめて1つのモジュール内部関数 `__xyo_mod_var_wide(double,double)` に分離**した。LLVM属性 `noinline` / `cold` を付与。可変除数の剰余を含むJIT/AOTモジュールで1関数だけ生成し、複数の剰余演算から共有する。

- 常に `i32` のガードが優先。32bitの正の整数なら従来どおり `urem i32` を実行。
- それ以外は共通の低頻度関数に渡し、もともとの範囲・整数性をチェックして `srem i64` または `frem` で処理。
- 負数と負のゼロ、無限大・NaN・ゼロ除数の挙動も変更しない。
- 呼び出し元は従来の `XYO_JIT_MOD_NAN_SPLIT` のNaN正準化を維持。
- この方式は特定のベンチマークやScratchブロック列には依存しない。
- **既定で有効**、`XYO_JIT_MOD_OUTLINE_WIDE=0` で旧インライン経路へ戻す。判定はコンパイル時のみであり、JIT実行中に環境変数は読まない。

## 同一Releaseバイナリでの交互A/B

- Intel Core i7-12700、CPU 12固定
- ScratchBench原文から生成した単独SB3。各ケース8ペア、ペア内のON/OFF順を交替
- 同じReleaseバイナリ `build/probe-mod-outline-xyo`、`XYO_JIT_MOD_OUTLINE_WIDE=1` と未設定を比較。下表はペアごとの `ops/ms` 比の中央値
- プロジェクト内部の計時区間のみ。起動・LLVMコンパイル時間は除外
- すべての実行で終了コード0、対象チェックサム一致

| 対象 | アウトライン ON / OFF |
| --- | ---: |
| `algo_primes` | **1.1019倍** |
| `algo_sort` | 0.9939倍 |
| `math_arith` | 0.9958倍 |
| `app_raycast` | 1.0001倍 |
| `math_expsqrt` | 0.9983倍 |
| `list_read` | 1.0023倍 |
| `algo_matmul` | 0.9988倍 |

`algo_primes` の8つのペア比は [1.095, 1.115, 1.102, 1.117, 1.102, 1.079, 1.084, 1.108]。他の小差は測定ノイズの可能性があるため、有意な改善や回帰とはみなさない。

生ログ: `build/mod-outline-paired.log`、構造化結果: `build/mod-outline-paired.json`。

## 機械語・IRの確認

`XYO_JIT_DUMP2=1`、`XYO_JIT_DUMP_OBJECT` で `algo_primes.sb3` の両経路を検査。

- OFF: `xyo_script_0_p2` は0x1b2b = **6955バイト**
- ON: 同関数は0x1afe = **6910バイト**、共有ヘルパ `__xyo_mod_var_wide` は0xe2 = **226バイト**
- ON: JITオブジェクトには独立した `__xyo_mod_var_wide` が1つだけ生成され、`noinline` を維持

メモ: この最適化の主因が、単にコードサイズの差なのか、ブロック配置やレジスタ割当・命令キャッシュの違いなのかは厳密には切り分けていない。生成関数は少し小さくなるが、共有ヘルパを含めた総サイズが必ず減るわけではない。

原データ: `build/mod-outline-{on,off}-asm.ll` と `build/mod-outline-{on,off}-asm-0.o`。

## 採用しなかった候補

- **64bit経路を完全に省略してfmodにfallback**：変数除数の `algo_primes` では3〜4%改善する一方、固定除数の `algo_sort` では10%以上悪化。高速化範囲は狭いうえ、64bit整数の性能を損なうため既定では不採用。独立切替試験 `build/mod-wide-var-paired.json`。
- **浮動小数点除算と積によるi32剰余の再構成**：整数補正で正確な結果にする試作だが、`algo_primes` で約9%低速化。試作コードを除去した。記録 `build/mod-recip-paired.json`。
- **i32ガードへの分岐重み付け**：`algo_sort` 等ではわずかに改善したものの `math_arith` で約3.4%悪化。一般化しないため試作コードを除去。記録 `build/mod-hints-paired.json`。

## 検証

最終コード `build/probe-mod-outline-final-xyo`：

- 通常の `cmake --build build --target xyo xyo-tests` 成功
- `ctest --test-dir build --output-on-failure -j4`: **7/7成功**
- ScratchBenchパリティ（ローカルバイナリを `XYO_BIN` で指定、すべての実行モード）：
  - インタプリタ **2123/2123**
  - JIT HotSpot **2123/2123**
  - JIT Eager **2123/2123**
  - AOT **2123/2123**
- 元のScratchBench performance.sb3を、既定ONおよび `XYO_JIT_MOD_OUTLINE_WIDE=0` の両方で実行：**47/47カーネル完走**、時間依存のschedulerを除く**39/39のチェックサム一致**
- `git diff --check` を確認すること
- 既存のワークツリーの他の未コミット変更は保持し、コミット・pushは行っていない

ログ: `build/mod-outline-final-ctest.log`, `build/mod-outline-final-parity-all.log`, `build/mod-outline-final-full-{on,off}.out`。
