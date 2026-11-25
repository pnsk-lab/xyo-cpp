# OSR-010：固定短回数の内側 `repeat` を含む warp LLVM 途中復帰

2026-11-09

## 目的と適用範囲

OSR-008/009 の LLVM→Interpreter 実復帰は「最も外側の選択済み native counted `repeat` の一反復全体が完了した後」に保存されたソース位置・カウンターに限定していた。内側 `repeat` が yield すると外側本体がまだ未完了で、外側の loopCounter を単純に復元しても副作用を重複させる。

OSR-010 は、**単一の内側 `repeat` が静的に1〜8反復と証明され、その本体が高々4個の `data_changevariableby` のみ**の場合に限定し、内側の LLVM スケジューラ yield を抑制する。外側 `repeat` は従来どおりネイティブ Fiber を yield し、終了済みの一反復が GC・変数に反映された境界だけを実行可能なデオプト点として利用する。

一般の内側ループにおける命令途中復帰ではない。内側の反復全体が小さく、外側本体とまとめて scheduler 原子的に処理される**制約付きの実装**である。

## 有効化

既定は OFF。`XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1` と `XYO_JIT_OSR_NESTED_SOURCE_PROOF=1` の双方が必要で、前者を有効にした場合でも条件外の内側ループはネイティブ OSR を選択しない。

```bash
export XYO_JIT_MODE=hotspot
export XYO_JIT_ASYNC=1
export XYO_JIT_OSR=1
export XYO_JIT_OSR_NESTED=1
export XYO_JIT_OSR_NESTED_DEEP=1
export XYO_JIT_OSR_NESTED_PARENT_REPEAT=1
export XYO_JIT_OSR_NESTED_ARG_READS=1
export XYO_JIT_OSR_NESTED_DIRECT_ARGS=1
export XYO_JIT_OSR_NESTED_HOIST_ARGS=1
export XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
export XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
export XYO_JIT_OSR_NESTED_INNER_BOUNDED=1
```

## 実装

- `src/jit/backend_llvm.cpp`：既存の `safeNestedLoopBody` に加え、`safeBoundedNativeInnerRepeat` で native 対象ループの**直接の制御経路**を再検査。1個だけの内側 `repeat`（TIMES は `math_number` / 整数系の純リテラルで、整数かつ1〜8）、その本体は `data_changevariableby` 高々4個、外側の連続ステートメントは `data_setvariableto` / `data_changevariableby` のみ。ソースグラフが欠落・循環・16ブロック超・予想外の制御構造なら拒否。Source proof がない場合も拒否。動的TIMES、長大・多重の内側ループ、メッセージ送信や未実装の副作用は拒否。
- `src/jit/codegen.cpp`：条件を満たす standalone OSR の内側ループだけ、通常の個別 `emitLoopYield()` を省略し、内部のGPU、par-for、特殊な一括処理/ループ展開も抑える。内側の論理反復と実行ブロック数の計数は通常どおり。選択された外側ソースループでは `emitNestedOsrYieldCheckpoint` が引き続き反復終了後の残数を記録。
- ソース変更を検出した際の Fiber 破棄、元 Interpreter のスタックフレーム・GC 引数の保持、`ExecContext::loopCounter` の復元、変更後の `BlockCached` 使用は **OSR-008 の既存処理をそのまま利用**。IR-001 / RT-001 が所有する共通 CallFrame やデオプト ABI は変更しない。

## 実行テスト

`tests/differential/jit_nested_warp_bounded_inner_deopt.py` が実際の SB3 を生成する。

- 外側 root `repeat(3)`（TIMES 変数は途中で17へ変化）、warp手続き内の `repeat(4,000,000)`、その本体に `repeat(2)`、内部の数値加算と日本語・絵文字引数による文字列更新。
- `tests/platform/nested_warp_bounded_inner_deopt_smoke.cpp` が `Runtime::step()` と実際の native Fiber の yield を観測し、内側で実行中の `data_changevariableby` の対象を `sum`→`branchCount` に変更。通常の `Blocks::resetCache()` でソース改訂番号・内容ダイジェストも更新。
- 検証ガードあり：変更が検出されると、**次の LLVM 命令を実行せず**元 Interpreter の外側ソースループで続行。編集直前に確定していた sum 値 `sumAtEdit` と最終 sum が完全一致。新しい `branchCount = 48,000,000 - sumAtEdit`。
- ガードなし：古い LLVM が次の native yield 後も元の `sum` に書き込む対照実験。後続の root 側呼出しでは編集済みソースが反映される。
- 最初の native yield および実際の **4回の native yield をまたぐ**ケースを検証。4回目は高速な場合、複数の native 呼出しにまたがることがあるため、実際の promotion / normal restore 件数を計測して完全な対応を要求する。
- `XYO_BLOCK_COUNT=1` では Interpreter/ガードありなし/編集なしすべてで **132,000,043** 個の Scratch ブロックを実行（通常の数値変数・文字列・親手続きの suffix も一致）。`XYO_BLOCK_COUNT=0` の高速化経路でも全 Scratch 変数の一致・副作用の保存が成功。
- `INNER_BOUNDED=0` の従来動作、および内側 `repeat(10)` では **ネスト LLVM OSR を拒否**し、Interpreter と全変数が一致することを実測で確認。

新規テストは SDL2 に依存しない：

```bash
ctest --test-dir board/.osr-silent-edit-headless \
  -R '^xyo-nested-warp-bounded-inner-deopt(-fast)?$' \
  --output-on-failure
```

関連ファイル：
- `src/jit/backend_llvm.cpp`、`src/jit/codegen.cpp`
- `tests/platform/nested_warp_bounded_inner_deopt_smoke.cpp`
- `tests/differential/jit_nested_warp_bounded_inner_deopt.py`
- `tests/CMakeLists.txt`

## 制限と次の課題

- 固定短回数でない内側ループは依然として対象外。外側本体の途中で任意の native yield を許してソース PC と複数の live counter を復元するには、IR-001 / RT-001 の**複数ループ継続状態**との正式な統合が必要。
- 内側ループの yield を抑えるため、この実験フラグは Scratch warp のスケジューリング頻度を微小に変更する。最大8反復・本体最大4命令という制限があるが、通常モードで広く使う前に詳細な時間意味論と極端に重い算術／拡張命令を検証する必要がある。
- ユーザーの一般的な同時並行ソース編集や任意の SSA／GC中断位置は対象外。編集の発生は実際の Scratch 直列スケジューラの Fiber 停止点だけを想定。
- OSR-010 はオプトインの実験的機能。既定のスケジューラ・GPU・その他 JIT 経路の動作を変えない。

コミット・プッシュは行っていない。

## 最終検証（2026-11-09）

- 新規 `xyo-nested-warp-bounded-inner-deopt`: **1/1 PASS 71.90s**（実行ブロック数 ON）。実験OFFの安全拒否、固定 `repeat(2)` のネイティブ採用・ソース変更後の正確な復帰、内側 `repeat(10)` の拒否をすべてテスト。
- 新規 `xyo-nested-warp-bounded-inner-deopt-fast`: **1/1 PASS 71.24s**（実行ブロック数 OFF）。ネイティブ高速化が残る条件でも同じ経路を確認。
- 旧 `xyo-nested-warp-silent-edit`: PASS 28.30s、`xyo-nested-warp-inflight-deopt`: PASS 39.74s、`xyo-nested-warp-inflight-body-edit`: PASS 46.16s、`xyo-nested-warp-inflight-body-edit-fast`: PASS 46.52s。**既存4/4、新規2/2、合計6/6 PASS**。
- ブロック数 ON の固定内側2反復版は、Interpreter、通常Native、ガード無効Native、ガード有効デオプトの全モードで **132,000,043 ブロック**。3回の warp 呼出し、変化する外側TIMES、元のScratchの後続命令、GC管理の日本語絵文字文字列引数を含む。
- 短い内側ループのせいで4回目の native yield が次の親呼出しに跨るケースがある。テストでは**観測した4回の実 native yield**と、その間の各 native promotion/normal restore を完全に数える。4回が同じ native 呼出しに属することは要求も主張もしない。
- 凍結された一貫ビルド `board/osr-bounded-inner-verified-v2-smoke`：バイナリ SHA256 `9030f30be14ff9b8830831f060477b5e6ac1ac729791e1ac8b8b0e86642c9889`、ソース digest `66dfc484f72448dd3716025cf85dc6c4c719313f854df7831af60fbdfb0a5f98`。最後の凍結ビルドは `ninja: no work to do` でテスト済み実行ファイルを複製した。
- 本テストは SDL2 OFF のヘッドレス C++ 実行ファイルを使用。**この変更を含む GUI parity 2123件は未実施**。

全てオプトイン、IR-001/RT-001 の所有ソースは未変更。共有ツリーに別担当の LLVM SSA 活性解析追加も同時に存在するが、その差分を破棄・上書きしていない。
