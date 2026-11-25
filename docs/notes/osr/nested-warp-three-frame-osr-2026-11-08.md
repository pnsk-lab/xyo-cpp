# 3フレームのネスト warp OSR（2026-11-08）

## 対応範囲

`XYO_JIT_OSR_NESTED_DEEP=1` は、従来の2フレームの途中移行に加え、次の **ちょうど3つ** の実行中インタプリタフレームを持つスクリプトに対して、内側のカウント付き `repeat` の残り部分だけを LLVM で実行する。

```text
event_whenflagclicked
  -> outer warp procedure call      (stack[0])
      -> inner warp procedure call  (stack[1])
          -> control_repeat         (stack[2], counter captured)
```

追加の入れ子呼出しは外側手続きの **直列ステートメント** に限る。再帰全般、IF分岐下の子手続き呼出し、呼出しを含む別の制御ループ、4フレーム以上への OSR は非対応である。元の2フレーム実装は変更せず、DEEP フラグの既定値は無効。

## 実行経路と安全性

- `src/jit/nested_warp_osr.hpp`: 外側 warp 手続きから直接呼ばれる内側 warp 手続きを静的に探索し、内側の `repeat` に既存の安全性検査を適用。内側の手続きの `argumentNames` で `argument_reporter` を検証する。返却メタデータに外側呼出し元の ID を格納。
- `src/jit/backend_llvm.cpp`, `src/jit/jit.hpp`: 非同期 JIT 完了後もネストした2つの呼出し ID とループ ID を保持。既存の standalone OSR LLVM エントリを再利用。
- `src/engine/runtime.cpp`: 2/3フレームを選び、正確なスタック ID、両手続きの `warp` 署名・引数数・名前一致、保留 reporter の不在、`InterpreterOsrSnapshot::validFor`、`sourcePath` の実際の祖先関係、実行済み call と loopCounter を検証してから OSR へ移行。任意の PC から移行するわけではない。
- `src/jit/rt.cpp`: 直接／SSA 引数ブリッジは、常に最後のループの **ひとつ手前** の StackFrame の `params` を参照。外側手続きの同名引数を誤読しない。両呼出し元の StackFrame は生きた GC ルートのまま。
- `src/engine/sequencer.cpp`: LLVM 完了時に3フレームと2つの呼出し元 ID とループ ID を再照合し、**内側ループだけ** `goToNextBlock()` する。内側・外側手続きの suffix はそれぞれ既存インタプリタで一度だけ再開する。

`JitCtx`, `Thread`, LLVM の手続きフレーム ABI のレイアウトは変更していない。一般的な NativeSourceContinuation／IR-001 の復元機能とは別の限定経路。

## オプトイン設定

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

## 検証

新しい `tests/differential/jit_nested_warp_deep_osr.py` で、各モードが Interpreter と全変数で一致することに加え、実際のネイティブ移行 `depth=3` と復帰ログを確認。

| ケース | 結果 |
| --- | --- |
| 2つの warp 手続き + 800万回 repeat、reporter 経由 | PASS、深さ3で移行・復帰 |
| 同条件、直接引数ブリッジ | PASS、深さ3で移行・復帰 |
| 同条件、事前 SSA 引数読込 | PASS、深さ3で移行・復帰 |
| 引数に日本語・絵文字・サロゲートペア | PASS |
| 内側の未宣言引数参照 | 3方式すべて移行拒否 |
| 内側が非 warp 手続き | 3方式すべて移行拒否 |
| DEEP フラグ無効 | 移行拒否 |
| 実行ブロック数 | Interpreter / 3方式とも **40,000,024** |
| `XYO_GC_THRESHOLD=1`、GC管理文字列・内外 suffix | 直接/SSAとも **341回 GC**, state完全一致 |
| 既存2フレーム direct args / GC / block-count | PASS、**368回GC** |
| 既存2フレーム args preserved | **6/6 PASS** |
| GUI Scratch parity、全体 | off / hotspot / eager 各 **2,123/2,123** |
| CTest `xyo-nested-warp-deep-osr` | **1/1 PASS**、36.19秒 |

整合ビルド `board/osr-deep-verified-xyo` の SHA-256:

```text
810c2c9c431d70ace30b235902cd8f8350d4f067724fae5da4543cd791c9d294
```

ソースダイジェスト（コンパイル時）は
`c623959ee79c7b647d89895b5454558532c68f01d5dffd33c3dfb5d294d392f7`。
CTest は `tests/CMakeLists.txt` に登録済み。

## 今後

より深い呼出しへ対応するには、対象手続きの静的なネスト経路を複数段保持し、各フレームの署名・戻り先・ソース上の親子関係を一般化した検証器が必要。効果途中の任意 PC、動的呼出し、非 warp の yield 復帰については IR-001 / RT-001 の共通 CallFrame・source PC を優先する。

コミット・プッシュは行っていない。
