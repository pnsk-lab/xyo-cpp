# warp ネスト途中での IF / IF-ELSE フレーム付き LLVM OSR

2026-11-09 / OSR-004

## スコープ

既存の `XYO_JIT_OSR_NESTED_DEEP=1` に `XYO_JIT_OSR_NESTED_BRANCH=1` を併用すると、warp 手続きの内側の直列ブロック列と、`control_if` / `control_if_else` の実行中の分岐フレームを含む限定的な source continuation から、最内側のカウント付き `repeat` の残り部分を LLVM に移行できる。

ネストは最大 **warp 呼出し8段 + 分岐4フレーム + counted repeat 1** の静的制限を持つ。各手続き・各分岐の実行中の StackFrame は移行前から最後までインタプリタが所有し、LLVM が担当するのは最後の repeat 本体の未実行分だけ。

例:

```text
root
  -> outer warp call
       -> if-else (THEN または ELSE)
            -> inner warp call
                 -> if / if-else (THEN または ELSE)
                      -> repeat (残り回数だけ LLVM 実行)
                 -> inner suffix
       -> outer suffix
```

この実験は `DEEP=1` が必須で、通常のデフォルトでは無効。

## 安全性・復帰方式

- `src/jit/nested_warp_osr.hpp`: 直列 statement / 実行予定 IF arm を静的に探索し、frame ID、branch arm（1=then、2=else、0=手続き呼出し）、最内側引数フレーム位置を出力。入れ子手続きの定義再訪による再帰、非 warp、未宣言引数参照、呼出しを含む counted loop は拒否。
- `src/jit/jit.hpp` と `src/jit/backend_llvm.cpp`: 非同期 LLVM 完了後も frame path と branch-arm 配列および引数フレーム位置を保持。既存メタデータには追記のみ。
- `src/engine/runtime.cpp`: 現在のスタックフレーム ID を全件照合。各手続き引数の署名と実行済み call、分岐フレームの opcode と期待 arm、`InterpreterOsrSnapshot::sourcePath()` の実際の arm、各手続きの returnBlockId、source revision と repeat カウンタがすべて一致した時だけネイティブに移行。
- `src/jit/rt.cpp`: `stackFrames[size-2]` と仮定せず、LLVM メタデータで検証済みの最内側 `procedures_call` フレームを参照し、GC ルート付きの原本の引数を直接/SSA 読込。未知・undefined は元の reporter へ戻る。
- `src/engine/sequencer.cpp`: ネイティブ終了時に frame path を再検証し、実行済み repeat の次へ進む。

### 発見・修正した重要な復帰バグ

最内側 repeat が IF arm の最後の statement の場合、`repeat->next` は空。従来の OSR 完了処理は単に `goToNextBlock()` を呼び、空のスタックフレームを残していた。次回の `stepThreadBound()` 開始時、空フレームだけが pop され、残った `control_if_else` を再評価して同じ repeat に再進入し、**LLVM 移行と復帰を無限に繰り返した**。

これを、ネイティブ終了直後にインタプリタの通常の制御フロー巻き戻しと同じ空末尾フレームの連鎖処理を行うよう修正。非 loop / 非 waitingReporter フレームしか巻き戻さず、途中に未対応の制御フレームが見つかれば推測で続行しない。800万回のループを LLVM で移行・実行後、後続の Scratch ブロックに一度だけ進み正常終了することを実測。

この修正は **分岐を含まない既存の2・3・最大8呼出し OSR** にも有効で、ループが手続きの最後の statement となる場合の同種の再実行を防ぐ。

## 有効化

```bash
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_DEEP=1 \
XYO_JIT_OSR_NESTED_BRANCH=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_NESTED_HOIST_ARGS=1 \
XYO_JIT_OSR_TIMING=1 \
  ./board/osr-branch-verified-v2-xyo play project.sb3
```

## 検証

新規 `tests/differential/jit_nested_warp_branch_osr.py` と CTest `xyo-nested-warp-branch-osr`：**1/1 PASS（43.96秒）**。

- outer IF-ELSE の THEN / ELSE × inner IF-ELSE の THEN / ELSE：**4組すべて実際の depth=5 の LLVM OSR と正常復帰**。最初の組は reporter 経由・直接・SSA 読込で検証。
- 単一 `control_if` の THEN：直接読込・SSA のどちらも実際の native promotion と復帰が成功。
- `XYO_JIT_OSR_NESTED_BRANCH=0`：分岐を含む OSR の選択を拒否。
- 未宣言引数名、途中の非 warp 手続き：OSR を拒否し、Interpreter と最終結果が一致。
- 分岐が外側だけの4フレーム：直接読込・SSA で depth=4 の OSR と正常復帰。
- `XYO_BLOCK_COUNT=1`：Interpreter、reporter、直接、SSA すべて **40,000,030** 実行ブロックで一致。
- 文字列引数（日本語・絵文字）+ `XYO_GC_THRESHOLD=1`：直接・SSA で **341回の GC**、完全な最終値と各 suffix の一致。
- 初期テストではループの反復再実行を検出し、**テストを止めて修正した**。この版のテストは native の本物の移行ログと最終状態を必須とする。

整合検証済みビルド:

```text
board/osr-branch-verified-v2-xyo
binary SHA256: cc7d9b11ea427427afe42e70a8bd8abef5f23e8c413950cbde7ed3c12a73ad00
source SHA256: df4dd0b7ff6de0f6f072952bcc37322ce67ace6609fa7cb92a87be7eef201df0
```

## 未対応

- 再帰、分岐条件の途中評価状態、手続き呼出しを含む制御ループ、非 warp yield、任意の source PC、一般 SSA / effect rollback、深さ制限の撤廃には対応していない。
- コンパイルされた継続エントリは選択された `repeat` の残りを実行するだけで、既に実行した procedure prefix / IF 条件を LLVM 上で再評価しない。
- コンパイル時と移行時の source revision は一致を要求するが、ネイティブ実行開始後の任意編集を巻き戻す一般デオプトは未実装。

コミット・プッシュは行っていない。

## 旧経路・GUI 最終回帰

- 今回のビルドで従来の最大8 warp 呼出し経路の全検証ケース PASS（深さ4/6/8/9、強制GC343、block count 40,000,058）。
- 従来の3フレーム回帰の全検証ケース PASS（強制GC341、block count 40,000,024）。
- 従来の2フレーム・数値引数では native promotion/restore を確認。日本語引数の一部実行では非同期コンパイルがループ終了までに完了せず、OSR発生必須の assertion に失敗。実行出力は Interpreter と一致し、これは未移行を成功扱いしないテストの仕様による。追加 O1 単独実行でも最終値は一致したが native promotion は再現できず、従来の全2フレームテスト成功とは主張しない。
- `XYO_JIT_OSR_NESTED_BRANCH=1` / DEEP / ARG_READS / DIRECT / HOIST 併用で GUI Scratch parity は off / hotspot / eager 各 **2,123/2,123**。
- Python `py_compile`、`git diff --check` 成功。

CTest #57 の最終結果は **1/1 PASS（43.96秒）**。
