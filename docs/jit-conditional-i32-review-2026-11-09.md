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
