# MATH-019 — 条件付き整数SSAの安全な32bit特化（2026-11-09）

## 実装と適用範囲

`src/jit/conditional_ssa.inc` に、条件付き warp repeat の整数SSAを `i64` から `i32` に特化する試験経路を追加。

**既定 OFF**。`XYO_JIT_CONDITIONAL_I32=1` を指定した時だけ、次のすべてを満たすループで32bitを使う。既存の `XYO_JIT_CONDITIONAL_SSA=0` は条件付きSSA自体を無効化する。

- 入場時に変数値が整数・Number・負のゼロでない・絶対値が 2^20 以下であることを検査
- 既存の64反復期限まで、更新回数上界24・1更新の最大絶対値65536から、全変数の絶対値が 2^20 + 64×24×65536 以下と証明
- さらに各条件式の加算・減算を木の後行順で絶対値伝播し、中間演算すべてが INT32_MAX 以下
- 更新が単調非減少であること、すべての剰余の被除数が非負と証明できること
- 本体は平坦な `if` だけで、`else` や入れ子の `if` を含まない

いずれかが不成立なら従来の64bit SSAへフォールバックする。試験対象の条件付き SSA が入口ガード不一致の場合は従来の Scratch JIT 本体を使用する。Scheduler tick、yield、Scratch変数の公開タイミングは変更しない。

32bit対象では PHI、加減算、比較、定数剰余の整数幅を統一する。`math_compare` の LLVM IR は `phi i64`×3・`urem i64`×3から `phi i32`×3・`urem i32`×3へ変化。負数・多重条件の高速化を無理に適用しない。

## 棄却した広い適用条件と回帰

最初は絶対値の上界だけで全条件ループを32bit化した。負数 `index=-40` から `-2` ずつ減らし `index%7` を評価する最小再現で、従来 `acc=-306` に対し32bit版は `acc=-750` と不一致。変換直後の生 LLVM IR には剰余・分岐が存在したが、最適化済みIRでは条件分岐が除去されていた。LLVM/契約のどちらに起因するかは未確定なので、32bitでの負数や符号付き剰余の経路は不採用。

また正の値だけの入れ子 `if/else` (`loops_ifelse`) でも、32bit版は同一バイナリ12組測定で従来比約0.779倍と大きく悪化。平坦でない条件分岐は全て64bitに維持するよう制限した。

最小再現: `build/math019-negative-small.sb3`、ログ `build/math019-negative-small-{0,1}.err/out`。デバッグ用raw LLVM IR: `build/math019-raw-negative.err`。試験用raw IRロギングコードは本体から削除済み。

## 性能

同一バイナリ、CPU12固定、Scratch-benchカーネル単独化で i32 OFF/ONの8組比較。切替スクリプトは `tools/bench_math_conditionals.py --toggle i32`。

| 項目 | ON/OFF速度比中央値 | 状態 |
| --- | ---: | --- |
| math_compare | **1.0272x** (+2.72%) | 平坦・非負剰余なのでi32 |
| loops_ifelse | **1.0001x** | 入れ子elseのためi64のまま |
| math_collatz | 0.9970x | 大きな変化なし |
| math_mandel | 1.0007x | 大きな変化なし |

実測結果の全チェックサム一致。`build/math019-flat-abba.json` / `build/math019-flat-abba.log`。とくに同一バイナリから生成した IR で、`loops_ifelse` はOFF/ONどちらも `phi i64`×3・`urem i64`×1、`math_compare` だけi32に変化したことを確認。記録は `build/math019-flat-*-ir-*.err`。

上記は8組の**ペア測定**の中央値であり、単一プロジェクトや全ユーザーの速度向上を保証しない。

## 正確性

`tests/differential/jit_conditional_repeat_ssa.py` を拡張し、正・負・小数・文字列・負のゼロ・2^20境界、64/65/127/128/129反復、非単調更新、別変数参照、符号付き剰余、巨大な22項加算（静的境界でi64維持）、10項加算、平坦な複数剰余if（i32有効）、剰余の被除数に負数の可能性がある式（i64維持）を検証。warp/nonwarp、Eager旧/新、HotSpot、新経路LLVM O2でInterpreterと一致。`build/math019-final-diff.log`。

Scratch-bench parity: HotSpot/Eager/Interpreter/AOT それぞれ **2123/2123**。独立検証バイナリ `build/math019-flat-xyo`、ログ `build/math019-flat-parity.log`。

本来の全47 performanceを同一バイナリでi32 OFF/ON完走。**非schedulerのチェックサム相違0**。`build/math019-flat-full-{off,on}.log`。単純な連続測定の全項目の速度比はホスト変動があるため速度の主たる根拠にはしない。

環境変数を明示的な **opt-in** にした最終バイナリ `build/math019-final-xyo` でも、上記拡張差分テストと `math_compare` のi64/i32命令種別・チェックサム一致を再確認した。`build/math019-final-diff.log`、`build/math019-final-compare-*.err`。

## 現在の統合ブロッカー

通常の共有ビルドは、他担当が編集している `src/render/sdl_renderer.cpp` の `realDlssEnabled_` 未宣言エラーで停止（ヘッダでは一部条件付き定義）。レンダラーソースやコンパイル設定は変更しなかった。数学コードの `codegen.cpp.o` のみCMakeのオブジェクト生成規則で再コンパイルし、既存 `libxyo.a` の**コピー**に置換してCLIを独立リンクした。共有ライブラリ/CLIは上書きしない。

独立リンクした `xyo-tests` は `stack smashing detected` で異常終了し、32bit機能OFFでも再現。一方、既存の `build/tests/xyo-tests` は終了コード0。共有のC++ライブラリやヘッダとの構成差が疑われるが未確定のため、**今回の変更についてCTest成功を主張しない**。完全な一貫ビルドでのCTest通過が既定ON昇格の条件。

## 採否・変更ファイル

**試験機能としてのみ採用、既定OFF**。

- `src/jit/conditional_ssa.inc`: i32境界証明・ガード・生成コード。非対象ループは旧i64。
- `tests/differential/jit_conditional_repeat_ssa.py`: 境界値と選択/非選択差分。
- `tools/bench_math_conditionals.py`: i32 ON/OFF比較。
- 本ドキュメント。

MATH-016/MATH-018の既存数値SSAは維持。共有 `src/jit/codegen.cpp` / RT/IR/描画の他エージェントの差分は保持。未commit、未push。


## 継続検証（境界値の直積）

`tests/differential/jit_conditional_repeat_ssa.py` に平坦な正剰余
条件付きループの24通りを追加。反復回数 63/64/65/129 と
初期 index -1/0/1/1048575/1048576/1048577 を直積化して、
64-tick chunk 境界、非負条件、±2^20入口ガードを同時に検査した。
`build/math019-final-xyo` で warp/nonwarp 各5経路
（Interpreter基準、Eager旧・新、HotSpot新、LLVM O2新）を比較し、
**10/10経路PASS・差分0**。記録:
`build/math019-extended-boundary.log`。

この検証は既存の独立CLIで行ったため、共有ソース全体の
整合ビルドやCTestのブロッカーを解消したことを意味しない。
既定は `XYO_JIT_CONDITIONAL_I32=OFF` のまま。


## 整合ビルドと CTest 再検証

独立リンクによるスタック破損を切り分けるため、
`build-math019-integrated` に CMake/Ninja で全ソースを
新規コンパイル。LLVM 23.1.4、全ターゲットのビルドが成功。
共有ソースは変更していない。

描画担当側の `SdlRenderer::draw()` で
`realDlssEnabled_` を無条件に参照しているが、
DLSS無効時にはメンバが宣言されないため、当該検証ビルドだけ
`-DCMAKE_CXX_FLAGS=-DrealDlssEnabled_=false` を使用した。
これは暫定的なビルド回避策であり、描画の本修正ではない。
設定ログ `build/math019-integrated-config.log`、
ビルド記録 `build/math019-integrated-build.log` および
`build/math019-integrated-all-build.log`。

`xyo-tests` CTest（新規ビルド、同一バイナリ）:
- i32 OFF: 1/1 PASS、43.27秒
- i32 ON: 1/1 PASS、74.96秒

ログ `build/math019-integrated-xyo-tests-{off,on}.log`。
前回の独立リンク版で見られた `stack smashing detected` は
再現せず。ABI不整合が原因だったかは確定していない。
同じ整合ビルドのCLIで増強した warp/nonwarp 差分テストも
10/10 PASS、ログ `build/math019-integrated-diff.log`。

全CTestでは最初にxyo/xyo-testsだけビルドした状態で
Not Runが多数発生したため、その結果は棄却。
全ターゲットを追加ビルドしてから再実行する。
既定ONへの昇格は全CTestと性能回帰を確認して判断する。


## 全CTest再検証と残課題

全ターゲットをビルドした後、i32 ON で109件のCTestを
並列度8で実行。**107/109 PASS、2件失敗**。
ログ: `build/math019-integrated-all-ctest-on.log`。
失敗内訳:
- `xyo-nested-warp-inflight-deopt`: OSRで観測可能なyieldを確認できず。
- `xyo-native-simple-deopt-parity`: 120秒のCTest制限に到達。
  ログでは複数の実復帰サブケースが成功している。
MATH-019直接対象の `xyo-jit-conditional-repeat-ssa` はPASS。

`xyo-nested-warp-inflight-deopt` は単独・並列負荷なしで
i32 OFF **PASS 47.75秒**、i32 ON **PASS 55.31秒**。
ログ: `build/math019-integrated-osr-off.log`、
`build/math019-integrated-osr-on-isolated.log`。
したがって全CTestのOSR失敗は同時負荷等で再現性が変化する。
i32有効化による決定的な意味論破壊は確認されなかったが、
原因は未確定。

既定ON昇格は保留。まず全CTestの並列度とタイムアウトを整理し、
安定した性能評価（交互比較）を確認する。共有ソースは変更せず。


## 最新整合ビルドでの交互性能測定

`build-math019-integrated/xyo`（新規整合ビルド）を
CPU 12固定で使用し、`tools/bench_math_conditionals.py`
`--toggle i32 --pairs 6 --cases compare,ifelse,collatz,mandel` を実行。
6組のペア測定は順序を擬似乱数で交互化し、同一バイナリで
`XYO_JIT_CONDITIONAL_I32=0/1` のみを変更。
各項目のチェックサムは全試行一致した。

| ケース | ON/OFF中央値 | 個別ペア比 |
|---|---:|---|
| math_compare | **1.0264x** | 1.0294, 1.0192, 1.0262, 1.0266, 1.0239, 1.0294 |
| loops_ifelse | 1.0124x | 0.9920, 1.0352, 0.9915, 1.0171, 1.0119, 1.0129 |
| math_collatz | 1.0095x | 1.0021, 0.9944, 1.0086, 1.0180, 1.0105, 1.0151 |
| math_mandel | 0.9995x | 0.9996, 1.0034, 0.9968, 0.9966, 0.9995, 1.0044 |

`math_compare` は6組すべてでi32 ONが速く、
前回8組の1.0272xとほぼ一致。
他3ケースは特化対象外または速度差が測定変動と区別できず、
高速化の効果として数えない。
`build/math019-integrated-abba.json` と
`build/math019-integrated-abba.log` に保存。


## 復帰テストの長時間単独実行とi32因果切り分け

全CTestで120秒を超えた `xyo-native-simple-deopt-parity` を、
並列負荷なし・上限360秒で直接実行した。多数のroot-frame復帰
サブケースを通過した後、制限時間ではなく
`root_nocall_simple: root-only producer without test phase gate failed`
というアサーションで停止した。
`build/math019-native-resume-isolated.log`。

該当する2ケースだけを切り出し、同一整合ビルドで
i32 OFF/ONを交互に実行して比較した。
`root_nocall_simple` と `root_nocall_nested` はいずれも
Interpreterとの最終状態は一致する一方で、
i32 OFF/ONの**両方**で
`adopted=1` と `[native-gc-after-adopt]` が0回だった。
期待は各1回で、復帰経路の試験条件を満たしていない。
`build/math019-native-resume-targeted.log`。

したがって、このケースの再現はi32有効化固有ではない。
OSR/RT所有領域には変更しない。単独テストの誤判定か、
本番ルートの移行条件不足かは引き続き独立調査が必要。
MATH-019の数学的差分テストはPASS、既定i32 OFF維持。


## 追加80件の決定的境界・乱択差分テスト（14:00以降）

`tests/differential/jit_conditional_repeat_ssa.py` に固定シード
`0x0192026` の乱択ケース48件と、非負整数の平坦な複数剰余IFで
i32経路を積極的に通す32件を追加。
反復回数0/1/2/31/63/64/65/127/128/129/130/191、
初期値の-2^20付近/2^20付近、大整数、-0、小数、
数値文字列/NaN/Infinity/非数値文字列を混在させた。

同一整合ビルド `build-math019-integrated/xyo` を使い、
warp/nonwarpそれぞれInterpreter基準、
Eager i64 / i32、HotSpot i32、LLVM O2 / O3 i32
の6経路、計12経路を比較して**12/12 PASS・差分0**。
`build/math019-seeded-80-o3-diff.log` に記録。

テストだけを拡張した。i32生成ロジック・既定値は不変更。
全CTestのOSR/RT関連課題とは切り離し、i32既定OFFを維持。

増強版CTest登録経路も i32 ON で **1/1 PASS（91.23秒）**。
記録: `build/math019-seeded-80-ctest.log`。


## LLVM IR上の選択・フォールバック回帰検証

`tests/differential/jit_conditional_i32_selection.py` を新規追加。
独立した最小SB3を生成し、`XYO_JIT_DUMP2=1` で最適化済み
LLVM IRを取得する。無関係なScratch repeat i32命令ではなく、
`rep.cond.iter`、`rep.cond.var`、`rep.cond.urem`
という条件付きSSA固有の名前を検査する。

- 平坦な非負剰余if：i32 OFFで`phi i64`/`urem i64`、
  ONで`phi i32`/`urem i32`。剰余演算は3個を確認。
- ネストしたif/else：i32 ON/OFFとも`phi i64`で、
  誤ってi32経路を選択しないことを確認。
- LLVM O2/O3 × i32 OFF/ON × 2形状の8件で**8/8 PASS**。
  すべての実行結果もON/OFF間で一致。

実行：
`python3 tests/differential/jit_conditional_i32_selection.py
build-math019-integrated/xyo`

この検証により、値の一致に加えてi32経路が実際に使われること、
非対象の条件分岐を誤って縮小しないことが機械的に検査できる。
数学のコード生成処理・既定値は変更せず、既定OFFを維持。


## i32選択の否定条件追加とCTest登録（続報）

`tests/differential/jit_conditional_i32_selection.py` を5形状へ拡張。
各形状をLLVM O2/O3、i32 OFF/ONで実行し、合計20/20 PASS。
出力値の一致に加えて条件付きSSAの型／不採用を検査。

| 形状 | i32 OFF | i32 ON |
|---|---|---|
| 平坦な非負剰余（3つの剰余） | i64 SSA | i32 SSA |
| 非負剰余＋10項の範囲内中間式 | i64 SSA | i32 SSA |
| 同条件＋22項の巨大中間式 | SSA不採用 | SSA不採用 |
| 負の剰余入力を含む平坦なif | i64 SSA | i64 SSA |
| ネストしたif/else | i64 SSA | i64 SSA |

**注意**：22項のケースは、i32範囲判定によるi64への
縮小拒否を直接証明するのではなく、現在のパーサー／
プラン生成段階で条件付きSSA自体が不採用になる。
範囲証明の境界に直接到達する追加ケースとは区別する。

共有の `tests/CMakeLists.txt` には既存の他者の変更を保持して
`xyo-jit-conditional-i32-selection` の登録だけを追記。
CMake再構成成功。正式CTest **1/1 PASS（0.84秒）**。
ログ：`build/math019-ir-selection-expanded.log`、
`build/math019-ir-selection-ctest.log`、
`build/math019-selection-cmake.log`。
本体のLLVMコード生成や既定i32 OFFは不変更。


## 21/22項によるi32範囲証明の直接検証

既存の22項式は48ノードの構文解析制限によりSSA自体が不採用だった。
今回、単一の剰余比較にバランス木21/22項加算を配置し、構文解析
制限内で中間値上界のi32安全性判定を検証した。

kScalarMax = 1048576 + 64 * 24 * 65536 = 101711872。
21項 = 2135949312 は INT32_MAX 以下で、i32 ONなら
phi i32 と urem i32 を生成。22項 = 2237661184 は
INT32_MAX 超過のため、条件付きSSAは維持して
phi i64 と urem i64 を生成した。

いずれもrep.cond.ssa.head と剰余命令1個が実際に存在し、
パーサー不採用とは区別できる。LLVM O2/O3とi32 OFF/ON、
6形状計24/24 PASS・実行結果一致。
正式CTestも1/1 PASS (1.09秒)。
ログ: build/math019-ir-boundary-direct.log、
build/math019-ir-boundary-ctest.log。
生成器本体・既定OFFは変更なし。



## i32実行時ガード入力の差分テスト

`tests/differential/jit_conditional_i32_selection.py` に
`guard_matrix` を追加。条件付きSSAの構造はi32選択可能な
平坦な非負剰余ifに固定し、実行時入力を11種類に変更：
0、-1、-0、小数1.5、非数値文字列、数値文字列、
2^20、2^20+1、2^40、他方の状態変数の小数0.5と
境界超過2^20+1。

各入力をInterpreterとi32 ONのLLVM O2/O3で実行し、
**22/22 PASS・最終状態一致**。最適化済みIR内の
i32条件付きSSAと高速／低速基本ブロックの存在も確認。
この検証は、各実行で実際にどちらの基本ブロックが通ったかを
トレースしたものではない。ガードの意味論検証と経路実行の
直接証明を区別すること。

既存の6形状×O2/O3×i32 OFF/ONの24例もPASS。
CTest登録名 `xyo-jit-conditional-i32-selection` は
**1/1 PASS (2.08秒)**。
ログ：`build/math019-runtime-guard-matrix.log`、
`build/math019-runtime-guard-ctest.log`。
i32本体と既定OFF設定は不変更。


## i32高速・低速経路の実測トレース

条件付きSSAコード生成にLinux限定かつ明示opt-in
`XYO_JIT_CONDITIONAL_I32_TRACE=1` を追加。
`plan.integer32State` の時だけ、生成するLLVM IRの高速・
低速基本ブロックへ`write(2, ...)`を挿入し、
`[math019-i32] fast/slow`を標準エラーへ記録。
通常実行時はIRへ診断呼出しを一切生成しない。
`tests/differential/jit_conditional_i32_selection.py` は
`XYO_MATH019_VERIFY_GUARD_TRACE=1` 時だけ実際の通過記録を
解析・検証する（通常CTestは診断なしで動作）。

LLVM23では`CreateGlobalStringPtr`は使えないため、
`CreateGlobalString`を使用。
一貫ビルドの再試行は共有描画ヘッダーの
`render::DlssOpaqueCoverage`未宣言で失敗。
他担当の描画ソースは変更せず、最新のcodegen.cpp.oのみを
既存整合アーカイブへ差し替えた専用
`build-math019-integrated/math019-guard-trace-xyo`
を分離リンクした。これは全面再ビルドではなく
診断機能の局所検証用である。
ビルド失敗ログは`build/math019-guard-trace-build.log`。

O2/O3で両方一致、各入力の`fast/slow`実測値：
- 0: **3/2**
- -1: **3/1**
- -0: **3/2**
- 小数1.5: **0/130**
- 非数値文字列: **3/2**
- 数値文字列: **3/2**
- 2^20: **0/130**
- 2^20+1: **0/130**
- 2^40: **0/130**
- 第2変数が小数: **0/130**
- 第2変数が2^20超: **0/130**

負数や文字列でもScratch通常の変更演算を経て条件を満たせば
後続チャンクは高速化される。分岐通過数は静的文字列定数を
数えるのではなく、標準エラーの実行時行のみを数えている。
全22入力モードがInterpreterと最終状態一致。
i32 OFFでtraceフラグを立てても記録ゼロ、
i32 ONでtraceフラグを立てなければ記録ゼロも確認した。

実行:
`XYO_MATH019_VERIFY_GUARD_TRACE=1 python3
tests/differential/jit_conditional_i32_selection.py
build-math019-integrated/math019-guard-trace-xyo`

通常CTestも`xyo-jit-conditional-i32-selection`:
**1/1 PASS (2.07秒)**。
ログ`build/math019-guard-trace-selection.log`、
`build/math019-guard-trace-ctest.log`。
i32特化は引き続き既定OFF、診断も既定OFF。


## 実行時ガードのチャンク横断境界（続報）

実行時ガード用入力を11種から13種へ拡張した。
`2^20-2=1048574` を初期値とすると、
反復中に高速・低速の両経路を通過（LLVM O2/O3とも
**fast=1、slow=67**）する。一方
`2^20-1=1048575` では初回の状態更新がガードを超えるため
**fast=0、slow=130**。両方Interpreterと最終状態一致。
この2条件を直接トレースで自動検証し、先の13入力×O2/O3
合計26件の実行時診断・結果一致をPASSした。
IR選択用の6形状×O2/O3×i32 OFF/ON=24件もPASS。

トレースを無効化したi32 ONの最適化済みIRには
診断用 `call i64 @write(...)` が**生成されない**ことも
追加のテストアサーションで確認した。診断有効時には実際の
分岐通過行が現れる。性能への副作用を避けるため、
i32と診断は引き続き既定OFFのまま。

検証:
`XYO_MATH019_VERIFY_GUARD_TRACE=1 python3
tests/differential/jit_conditional_i32_selection.py
build-math019-integrated/math019-guard-trace-xyo`
=> PASS。
通常CTestは`xyo-jit-conditional-i32-selection` が
**1/1 PASS（2.30秒）**。
ログ:
`build/math019-guard-crossing-trace.log`,
`build/math019-guard-crossing-ctest.log`。

共有ソース中の描画ヘッダーに別担当変更のビルド障害が残るため、
MATH-019用の隔離バイナリを利用。描画担当の差分は改変せず。


## 64-tickチャンク境界の分岐遷移列テスト

`tests/differential/jit_conditional_i32_selection.py` の
生成器へ`repeat_count`を追加し、診断明示ON時だけ
`chunk_boundary_matrix` を実行。
入力はindex=0、acc=0、平坦な非負剰余ループ。
1/2/31/63/64/65/127/128/129/130/191反復の11ケースを
Interpreterとの最終値一致に加え、実際のガード通過順序で検証。

`S`=slow、`F`=fast とすると：
- 1: S、2: SS
- 31/63/64: SSF
- 65/127/128: SSFF
- 129/130/191: SSFFF

LLVM O2/O3の両方で一致（11×2=**22/22 PASS**）。
既存の実行時ガード13入力×O2/O3=26ケース、
IR選択6形状×O2/O3×i32 ON/OFF=24ケースも
全PASS。診断時の分岐イベント抽出処理を
`trace_events`へ共通化。

明示診断の実行：
`XYO_MATH019_VERIFY_GUARD_TRACE=1
python3 tests/differential/jit_conditional_i32_selection.py
build-math019-integrated/math019-guard-trace-xyo`
ログ`build/math019-chunk-boundary-trace.log`。

通常モードでは追加の経路ログ・検査は走らず、
整合バイナリ`build-math019-integrated/xyo`での
直接テストPASS、正式CTestは
**1/1 PASS（2.43秒）**。
ログ`build/math019-chunk-boundary-default.log`と
`build/math019-chunk-boundary-ctest.log`。

トレース回数の断定はこの固定fixtureと現在の
スケジューラのチャンク配置に対する回帰期待値。
任意のScratchプロジェクトに一般化しない。
数学本体・既定OFFの変更なし。


## Warp・非Warpの64-tick分岐遷移の対照検証

`tests/differential/jit_conditional_i32_selection.py` の
`create_fixture()` に `warp` 引数を追加し、
既存の11種類の反復回数をWarp / 非Warp両方で検証した。
共通の平坦な非負剰余条件を用いる。どちらも生成IRには
i32剰余命令が存在するが、非Warpの実行時ガードは
スケジューラを尊重し、高速チャンクを通さない。

- Warp: 1 `S`; 2 `SS`; 31/63/64 `SSF`;
  65/127/128 `SSFF`; 129/130/191 `SSFFF`
- 非Warp: 各ケース `S` が反復回数ぶん、fast=0。
  例：64回はslow=64、65回はslow=65、
  130回はslow=130、191回はslow=191

`repeat(1)` は最適化済LLVM IRから
`rep.cond.iter = phi i32` が消えるため、
1回反復ではPHI存在を要求せず、i32剰余命令と
実際の分岐トレースを確認する。
O2/O3 × Warp/非Warp ×11回数で **44/44 PASS**、
すべてInterpreter最終値一致。
以前の実行時ガード26件、IR選択24件もPASS。
正式CTest **1/1 PASS（2.70秒）**。

ログ：`build/math019-chunk-warp-nonwarp-trace.log`、
`build/math019-chunk-warp-nonwarp-ctest.log`。
テスト生成器とアサーションのみ変更。
数学生成器は不変更、i32・診断いずれも既定OFF。


## 決定的乱択によるWarp・非Warpのガード遷移検証

`tests/differential/jit_conditional_i32_selection.py`に
`randomized_chunk_matrix`を追加。固定seed `0x019C0DE`で
24件を生成し、2^20境界と63/64/65/129反復の指定4件を追加。
28入力×Warp/非Warp×LLVM O2/O3の計112実行で
Interpreterと最終状態が一致し、O2/O3間の実行時ガード遷移列も一致。
非Warpでは高速チャンク0回、低速経路が反復回数ぶん実行される。
Warpではガード入口数が反復回数を超えないことも検証する。
全28ケースPASS。

前段の固定11回数×Warp/非Warp×O2/O3（44実行）、
実行時ガード13入力×O2/O3（26実行）、IR幅選択
6形状×O2/O3×i32 ON/OFF（24実行）も同一実行でPASS。
正式CTest `xyo-jit-conditional-i32-selection`:
**1/1 PASS（2.61秒）**。
ログ`build/math019-randomized-chunk.log`、
`build/math019-randomized-chunk-ctest.log`。
追加の乱択は診断を明示ONにした時のみ実行。
コンパイラー本体と既定OFFは不変更、未commit/push。


## 動的反復回数のLLVM SSA不採用検証

`XYO_JIT_OPT_LEVEL` の読み取りを
`src/jit/backend_llvm.cpp::jitIrOptimizationLevel()`で確認。
`2`はLLVM O2、`3`はLLVM O3へ正しく対応している。

`tests/differential/jit_conditional_i32_selection.py`に
`create_fixture(dynamic_count=True)`と
`dynamic_count_matrix`を追加。`repeat(変数limit)`に
実行時設定値0/1/2/63/64/65/130を渡す。
7種類×Warp/非Warp×LLVM O2/O3＝**28/28 PASS**、
最終状態はInterpreter一致。
最適化済IRに`rep.cond.ssa.head`と条件付きi32 PHIが
存在せず、診断を明示有効化しても`[math019-i32]`の
実行時イベントが出ないことを検査。
静的反復回数と動的回数を混同して不正に狭幅化しない。

以前の固定境界44実行、実行時ガード26実行、
決定的乱択112実行、IR選択24実行もPASS。
正式CTest **1/1 PASS（4.36秒）**。
ログ`build/math019-dynamic-fallback-trace.log`、
`build/math019-dynamic-fallback-ctest.log`。
検証はMATH-019の隔離ビルドを利用、
i32本体・診断本体・既定OFFは不変更。


## 動的repeat式・異常回数のフォールバック検証

動的回数の形態を`repeat(limit)`に加えて
`repeat(limit + 1)`、`repeat(limit - 1)`へ拡張。
最終的な回数は-2, 0, 1, 2, 2.5, 63, 64, 65, 130の9種類。
これらをWarp/非Warp×LLVM O2/O3で検証した。
3種類の式×9回数×2 Warp設定×2 LLVM最適化レベル
= **108/108実行 PASS**。54形状のすべてで
Interpreterとの最終状態一致。
最適化済LLVM IRに`rep.cond.ssa.head`および
条件付きi32 PHIが存在せず、明示診断ONでもガード通過記録なし。

O2/O3の指定は`jitIrOptimizationLevel()`で
文字列`2`/`3`から各LLVM最適化レベルへ変換されることを確認済。
固定ガード26実行、固定チャンク44実行、
決定的乱択112実行、幅選択24実行もPASS。
通常CTestは**1/1 PASS（9.93秒）**。
記録`build/math019-dynamic-expression-trace.log`、
`build/math019-dynamic-expression-ctest.log`。
変更はテスト生成器とレビュー記録であり、
数学生成器本体と既定OFF設定は不変更。


## 17:47以降の統合ビルド再試行とi32 ON/OFF再計測

`cmake --build build-math019-integrated --target xyo-cli -j 6` を再実行したが、共有描画コードの `src/render/sdl_renderer.hpp:192` にある `render::DlssOpaqueCoverage` 未宣言で失敗。型定義は `src/render/dlss_opaque_coverage.hpp` にあり、現在の `sdl_renderer.hpp` から参照されていない。描画コードには手を加えず、ビルドログは `build/math019-integrated-retry-1747.log` に保存した。統合ビルド成功とは扱わない。

MATH-019用の既存隔離バイナリ `build-math019-integrated/math019-guard-trace-xyo` による `tools/bench_math_conditionals.py --toggle i32 --pairs 4 --cases compare,ifelse,collatz,mandel` を実行。
- `math_compare`: ON/OFF中央値 **1.0279x**、4ペア比 1.0148 / 1.0270 / 1.0287 / 1.0366
- `loops_ifelse`: **1.0057x**、4ペア比 1.1058 / 1.0035 / 1.0080 / 0.9755
- `math_collatz`: **0.9842x**、4ペア比 0.9707 / 0.9684 / 1.0036 / 0.9977
- `math_mandel`: **1.0117x**、4ペア比 1.0228 / 1.0120 / 1.0115 / 0.9974

全チェックサム一致。ログ `build/math019-1747-paired.log`、構造化結果 `build/math019-1747-paired.json`。4組であり、特に非対象カーネルの比率はホストノイズ込みの観測値。既定ON昇格は統合ビルド通過、全体CTest、別環境での検証を条件とし保留。i32/診断とも既定OFF、未コミット・未プッシュ。


## 18時台：ヘッダー強制読み込みで新規一貫CLIビルド・関連CTest成功

共有の`src/render/sdl_renderer.hpp`は依然
`render::DlssOpaqueCoverage`の宣言を直接参照していない。
担当外ソースの編集を避け、
`-include /home/kicky/dev/cpp/xyo-cpp/src/render/dlss_opaque_coverage.hpp`
を一時CMakeの`CMAKE_CXX_FLAGS`に追加して
`board/.math019-integrated/cmake/`に
RelWithDebInfoの新規CLIを全コンパイル。
旧ビルドのオブジェクト差し替えは行わず、
`xyo-cli`のビルド成功。
コンパイル構成は既存の
`-DrealDlssEnabled_=false`回避設定も継承。
ログ：`board/.math019-integrated/results/config.log`、
`board/.math019-integrated/results/build-cli.log`。

最新CLIで
`xyo-jit-conditional-repeat-ssa` **PASS 83.00秒**、
`xyo-jit-conditional-i32-selection` **PASS 7.11秒**、
合計 **2/2 PASS (90.11秒)**。
ログ：`board/.math019-integrated/results/ctest-math019.log`。

診断トレースを明示ONで実行した新ビルドの選択テストも
全件PASS。24件の幅選択、26件の入力ガード、
固定チャンク44実行、乱択112実行、動的回数の
54形状×O2/O3＝108実行が成功。
ログ：`board/.math019-integrated/results/trace-math019.log`。

一方、完全CTest向けの`xyo-tests`リンクは
`tests/CMakeLists.txt`の
`file(GLOB TEST_SOURCES ... unit/*.cpp)` によって
独立`main`を持つ複数のstandalone smokeを同時に収集し、
`main`の重複定義となる。
さらに`xyo_jit_guard_slow`の重複定義あり。
`xyo-tests`をビルドできなかったため、
**全CTest成功とは主張しない**。テスト担当領域の
ソースを一切修正せず、事実を掲示板へ報告する。

共有の`build-math019-*`と`build/math019-*`は
並行作業中に消失したため、現在の記録を
`board/.math019-integrated/`へ移した。
通常構成のビルド・全CTest完了まで既定ON昇格は保留。
i32・診断は既定OFF、未commit/push。


## 18時台：Omnibusテストリンク障害のCMake注入回避と全141件CTest

共有`tests/CMakeLists.txt`は`file(GLOB TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)`で`xyo-tests`へ独立`main()`を持つ7件を混入させるため、既存`test_cast.cpp`の`main()`と衝突する。加えてテスト用`xyo_jit_guard_slow`と実ランタイムの実装が重複定義になる。**共有CMakeLists、他担当のテストソースは不変更**。

作業用`board/.math019-integrated/filter-unit-mains.cmake`を作成し、`-DCMAKE_PROJECT_INCLUDE=/home/kicky/dev/cpp/xyo-cpp/board/.math019-integrated/filter-unit-mains.cmake`でroot CMake projectに遅延処理を登録。`xyo-tests`の`SOURCES`だけから独立mainの7件を除外（39→32）。この7件を検証したと主張してはいけない。描画ヘッダーの既知の宣言不足は引き続き`-include .../dlss_opaque_coverage.hpp`でコンパイラー側から回避。これらの設定は通常構成の代替ではない。

この専用設定で`xyo-tests`のリンク成功、単独CTest **1/1 PASS 47.68秒**。さらに`cmake --build board/.math019-integrated/cmake -j6`による**全ターゲットのビルド成功**。全CTest`ctest --test-dir board/.math019-integrated/cmake -j6 --output-on-failure`は **141件中138 PASS・3 FAIL（98%、706.21秒）**。MATH-019の2件、`xyo-tests`、`xyo-event-driven-scheduler`、`xyo-jit-string-pair-compare`はPASS。

失敗したテスト：
- `#34 xyo-nested-warp-quad-yield-deopt`: 89.26秒でguarded実行中`FAIL native LLVM never yielded at an observable safe point`。OSR-009掲示板で再現共有。
- `#56 xyo-native-simple-deopt-parity`: 120秒TIMEOUT。単独`-j1`再実行でも120秒TIMEOUT。`interleaved_shared_variables`などの途中チェックまでPASSしているが最後まで完走しない。IR-001掲示板に共有。
- `#136 xyo-ir001-live-effect-source-broadcast`: `ir001_effect_source_trace.py:193`のdrift行アサーション不一致。単独`-j1`でも同一drift行で失敗、再現性あり。IR-001掲示板に共有。

ログ：`board/.math019-integrated/results/reconfigure-filter.log`、`build-xyo-tests.log`、`ctest-xyo-tests.log`、`build-all.log`、`ctest-all.log`、`ctest-deopt-isolated.log`、`ctest-ir001-isolated.log`。全CTest不合格のためMATH-019のi32特化は引き続き既定OFFとする。既存の診断も既定OFF。未commit/未push。

### 全CTest失敗の単独再検証（補足）

`xyo-nested-warp-quad-yield-deopt` (#34) は
同一専用ビルドの`ctest -j1 -R '^xyo-nested-warp-quad-yield-deopt$'`
で **1/1 PASS（69.89秒）**。
全件`-j6`時の失敗は負荷競合・非決定性の可能性があるが
未確定で、他条件下の回帰を否定しない。
ログ`board/.math019-integrated/results/ctest-quad-isolated.log`。

`xyo-native-simple-deopt-parity` (#56) は
同じ単独`-j1`でも120秒TIMEOUT、
`xyo-ir001-live-effect-source-broadcast` (#136) は
同じドリフトアサーションでFAIL。
したがって**全141件の実行結果は138 PASS/3 FAIL**のまま、
単独再実行を踏まえると「単独で再現した失敗」は後者2件。


## opt-in 環境変数の完全一致・fail-closed修正（19時台）

`src/jit/conditional_ssa.inc` の二つの実験用フラグ
`XYO_JIT_CONDITIONAL_I32` と
`XYO_JIT_CONDITIONAL_I32_TRACE` は
従来 `flag[0] == '1'` だけで有効化しており、
`10` や `1garbage` でも有効になる問題があった。
`std::strcmp(flag, "1") == 0` へ変更し、
**明示的に値が完全に`1`である場合のみ有効**にした。
設計上の「未知値は従来i64へフォールバック」に合致する。

テスト生成器 `tests/differential/jit_conditional_i32_selection.py`
に`optin_flag_matrix`を追加。
i32有効化フラグは未設定/空/0/00/2/10/1garbage/1を
LLVM O2/O3で確認して **16/16 PASS**。
`1`のみ最適化済IRが`phi i32`、他は`phi i64`、
全ケースInterpreter結果一致。
診断フラグは未設定/空/0/10/1garbage/1 × O2/O3
**12/12 PASS**。
`1`だけ実行時分岐イベントを出力し、
他は最適化済IRに診断用`call i64 @write`も残さない。
従来のIR幅選択、境界値、乱択、動的repeatテストも
すべてPASS。
`xyo-jit-conditional-i32-selection`正式CTest
**1/1 PASS（9.52秒）**。
専用ビルドは`board/.math019-integrated/cmake/xyo`。
ログ：`board/.math019-integrated/results/build-strict-flags.log`、
`strict-flags-selection.log`、`strict-flags-trace.log`、
`strict-flags-ctest.log`。

同じ隔離CLIでIR-001失敗ケース
`xyo-ir001-live-effect-source-broadcast`を
`XYO_JIT_CONDITIONAL_I32=0`/ `=1` の両方で確認。
両方とも同一のdriftアサーションの3行でFAIL。
MATH-019切り替えにかかわらず再現することを示すが、
これのみで独立性を完全証明するものではない。
ログ：`board/.math019-integrated/results/ir001-i32-{0,1}.log`。
通常ビルドと全141件CTestの障害は未解消のため、
i32・診断とも既定OFF、未commit/push。
