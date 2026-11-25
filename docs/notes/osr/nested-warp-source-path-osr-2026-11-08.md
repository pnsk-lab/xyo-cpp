# 最大8段の warp 手続き呼出し経路に対する LLVM OSR

2026-11-08 / OSR-003

## 対応範囲

従来の root → warp → repeat（2フレーム）と root → warp → warp → repeat（3フレーム）を、最大 **8個の warp 手続き呼出し + 1個のカウント付き repeat（計9フレーム）**まで一般化した。`XYO_JIT_OSR_NESTED_DEEP=1` が必要。通常は既定 OFF。

Scratch の直列 NEXT 接続だけを探索し、途中の `procedures_call` は warp かつ通常の非 reporter 手続きでなければならない。`control_repeat` の残り部分について既存の副作用・入力 whitelist を満たすときだけ、別途生成した LLVM 継続エントリを呼ぶ。

```text
event_whenflagclicked
  -> warp call #0       （既存 Interpreter のフレーム）
      -> warp call #1   （引数値・復帰先は元のまま）
          ...
          -> warp call #7
              -> control_repeat の残りだけ LLVM で実行
              -> 元のインタプリタへ復帰
          -> すべての呼出し元 suffix を各1回実行
```

## 変更と検証上の要点

- `src/jit/nested_warp_osr.hpp`: 任意の正しい静的 callsite 配列を最外側から最内側へ生成する有界探索に置換。再帰的な同一手続き定義・非 warp・reporter call は拒否。最大8呼出し、各層最大16引数、各手続きの直列文は最大128個。
- `src/jit/backend_llvm.cpp`、`src/jit/jit.hpp`: 非同期コンパイル結果に正確な呼出し元ブロックIDの配列と `Blocks::revision()` を保存する。古い1/2段のメタデータ文字列は互換性のため残す。
- `src/engine/runtime.cpp`: 現在の実インタプリタスタックのすべての ID、各 warp 手続きの署名・保留リポータ無し・実行済み call 状態、`InterpreterOsrSnapshot::validFor()`、`sourcePath()` と `returnBlockId` を照合してから移行。コンパイル元のソース revision が一致しない場合は拒否。
- `src/jit/rt.cpp`: 元の `stackFrames[stack.size()-2]->params` を読み取る直接・SSA 入力経路をフレーム数2..9に対応。元の引数フレームを保持し、GC ルートを勝手に別の所有形へ移さない。`undefined` は各読出し位置で従来の reporter にフォールバックする。
- `src/engine/sequencer.cpp`: LLVM の残りループ終了時に呼出し元 ID 配列全体を再検証し、最後のループフレームだけ `goToNextBlock()` する。戻り先の各手続きは、元のインタプリタで後続ブロックを実行する。

この経路は新しい JitCtx / Thread の ABI フィールドを追加せず、通常 JIT の起動方式も変更しない。コンパイル時と途中移行時のソース revision を照合するが、**既に LLVM を実行し始めた後でソースが編集された場合の一般的なデオプトは実装していない**。

## 有効化

```bash
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_DEEP=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_NESTED_HOIST_ARGS=1 \
XYO_JIT_OSR_TIMING=1 \
  ./build/xyo play project.sb3
```

## 確認結果

`tests/differential/jit_nested_warp_stack_osr.py`: 実際の LLVM 移行と呼出し元への復帰をチェックし、すべての Scratch 変数を Interpreter と比較。

| 対象 | 結果 |
| --- | --- |
| warp 3・5・7・8段 | reporter / 直接 / SSA の各方式で移行・復帰すべて PASS（計12ケース） |
| 日本語・絵文字引数名 / 異なる階層の numeric 引数 | 完全一致 |
| 内側 non-warp、未宣言引数名、深さ上限8段超過 | 途中移行拒否 PASS |
| DEEP 無効時の4フレーム | 途中移行拒否 PASS |
| 5段のブロック実行数 | Interpreter / reporter / direct / SSA とも **40,000,058** |
| 8段 + 文字列をループ中に割当 + `XYO_GC_THRESHOLD=1` | 直接 / SSA とも **343 GC**、最終状態一致 |
| 旧2/3フレーム OSR をバイナリ v2 で再実行 | 全ケース PASS。2フレームの count 24,000,015、GC368回、3フレームの count 40,000,024、GC341回 |
| GUI Scratch parity（ソース revision 追加後の v2） | off / hotspot / eager 各 **2123/2123** |

新規テストは `tests/CMakeLists.txt` に `xyo-nested-warp-stack-osr` として登録。引き続き旧 2・3 フレーム OSR を別の回帰テストでも確認する。

ソース revision チェックを追加した後の整合性検証済みバイナリ:

```text
board/osr-stack-verified-v2-xyo
SHA-256: 3b24206b0d6c7cd15acd4495ed2c68077c86a905f17150de261aec4567bca706
Source SHA-256: 3c58db381a394289151de514e1a3fa31c5b02d50a3a5b879eddab6e88dabac0a
```

## 未対応

9段以上の warp 呼出し、再帰、IF / control_repeat の中での別手続き呼出し、任意の source PC、途中の SSA／副作用の復元、非 warp yield 復帰は未対応。これらは IR-001 / RT-001 共通 CallFrame / Resume PC の実装領域。今回の経路は **残りループの再コンパイル実行のみ**で、既に実行した手続き prefix を再生しない。

共有ツリーで他エージェントが並行編集しているため、コミット・プッシュはしていない。

## 最終回帰

- CTest #56: xyo-nested-warp-stack-osr **1/1 PASS**、58.08秒。
- 旧2/3フレームの差分テストは独立再実行で全 PASS。
- 並行ビルド中の一度だけ、旧2フレーム fixture が非同期コンパイル完了前に実行を終え、実際の OSR 移行を確認できず assertion が失敗。Scratch の出力不一致ではなく実移行を必須とするテストの時間依存であり、負荷を分離した再実行で全モードの実移行と復帰を確認した。
