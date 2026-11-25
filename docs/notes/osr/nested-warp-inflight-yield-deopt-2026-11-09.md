# OSR-008 — ネイティブ warp yield 後のソース編集からの限定デオプト

2026-11-09

## 概要

OSR-007 までの `XYO_JIT_OSR_NESTED_SOURCE_PROOF=1` は、**LLVM OSR を採用する前**のソース改竄を拒否する。OSR-008 では、すでに LLVM でループの一部を実行した後、**実際の native Fiber が yield してスケジューラに戻った時点**でソースが更新された場合に、既存の Interpreter のスタックへ正しく戻る経路を実装する。

新しい実験フラグ：

```bash
export XYO_JIT_OSR=1
export XYO_JIT_OSR_NESTED=1
export XYO_JIT_OSR_NESTED_DEEP=1
export XYO_JIT_OSR_NESTED_PARENT_REPEAT=1
export XYO_JIT_OSR_NESTED_ARG_READS=1
export XYO_JIT_OSR_NESTED_DIRECT_ARGS=1
export XYO_JIT_OSR_NESTED_HOIST_ARGS=1
export XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
export XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
```

`INFLIGHT_DEOPT` は `SOURCE_PROOF` を必須とし、既定では無効である。共通 CallFrame ABI / RT-001 / IR-001 のコードは変更しない。

## 安全な境界

### 1. コンパイル側

`src/jit/codegen.cpp` は、ネイティブ実行中の**選択された counted repeat** が実際に warp yield する直前に、すでに本体を実行済みの更新を Target に反映（`spillPromoted`）。続いて、LLVM の private loop counter から **`remainingAfterBody = remainingBeforeDecrement - 1`** を記録する（`xyo_rt_nested_osr_yield_checkpoint`）。

この時点では直前の Scratch 本体のすべての処理が終わっている。一方、LLVM のループ末尾にある private counter の減算は `xyo_yield` から戻った後に実行される。そのため、yield 前に `-1` した値を別に保存すると、Fiber を再開することなく次の Scratch `control_repeat` 判定に必要な正確な残り回数を復元できる。

`src/jit/backend_llvm.cpp` はこのモードでネストされた counted repeat 本体を対象外にする。内側のループで yield した場合に、外側の本体がまだ完了していないのに「すべて完了した」と誤認しないための保守的な制限である。空ループの一括スケジューラ、GPU実行、par-for も、この選択ループでは使用しない。

### 2. スケジューラ側

`src/engine/sequencer.cpp` の `jitStepThread` は、すでに一度 native Fiber から戻り、再開を待っている場合だけ、不変な `SourceFrameSymbols` のソース改訂番号と内容ダイジェストをチェックする。

- 同一なら通常どおり LLVM Fiber を再開。
- 相違していて正常な post-body checkpoint があるなら、Fiber を再開せず破棄し、LLVM の未完了 private counter を元の Interpreter の内側 `ExecContext::loopCounter` に復元。`Thread::stack` / `stackFrames` / 全引数の GC ルートをそのまま保持。Interpreter の次の `control_repeat` 判定に戻る。
- 相違していて実行可能な境界を証明できないなら、古い LLVM コードを実行して誤った副作用を起こすより、スクリプトを安全終了する（fail-closed）。これは完全な実行継続ではない。

`src/jit/jit.hpp` に追加した `nestedOsrDeoptReady` と `nestedOsrDeoptRemaining` は `JitCtx` の末尾へ追加。従来の offset は変更せず、既定経路には yield 時の追加ヘルパや per-iteration のチェックは発生しない。

## 実測検証

`tests/platform/nested_warp_inflight_deopt_smoke.cpp` は実際の `Runtime::step` とネイティブ Fiber を使う。テスト用に公開 Runtime の `warpTimeMs` を 3ms に設定し、500ms の通常の warp budget を短縮して**本物のスケジューラ yield** を安定して再現する。製品コードには改竄用フックを作らない。

`tests/differential/jit_nested_warp_inflight_deopt.py` は内側に800万回、外側に動的 `repeat(3)` の Scratch プロジェクトを生成。日本語・絵文字を含む手続き引数と `join` の大量実行／GC を含む。LLVM の実 native Fiber がすでに yield したことを確認してから、実行済みの root `set sum = 0` の `VARIABLE.id` を `v_sum`→`v_done` に直接変更。元のソース改訂番号を変えない場合と、明示的に版番号を変更する場合を比較する。変更する初期化ブロックはすでに実行済みであるため、後続の実行結果は本来変わらない。

| モード | 実 native OSR | 通常 native 完了 | 境界デオプト | Scratch全変数 |
|---|---:|---:|---:|---|
| positive（proof ON、編集なし） | 3 | 3 | 0 | Interpreter と同一 |
| unguarded（proof ON、境界 guard OFF、無改訂編集） | 1 | 1 | 0 | 同一 |
| guarded（proof ON、境界 guard ON、無改訂編集） | 1 | 0 | **1** | 同一 |
| versioned（proof ON、境界 guard ON、改訂あり編集） | 1 | 0 | **1** | 同一 |

**ブロック実行数：Interpreter / positive / unguarded / guarded / versioned のすべてが正確に `144,000,043`**（`XYO_BLOCK_COUNT=1` で全状態を比較）。

従来の OSR-007 の「LLVM 採用前」改竄拒否 CTest と、新 OSR-008 の「LLVM の実 yield 後」デオプト CTest は、SDL2 無効のヘッドレス構成で **2/2 PASS**（それぞれ 28.11s、40.53s、計68.65s）。

新たに `tests/CMakeLists.txt` へ、ヘッドレスで使える `xyo-nested-warp-inflight-deopt` を登録。

整合ビルドは以下の凍結バイナリ：

```text
board/osr-inflight-verified-v2-smoke
Binary SHA256: 5b4e4d5b01110e7c7ccb3ab87554f7eb4a9ffc92e4496cce8eb73e524262dede
Source SHA256: 976bd3f34797addb208c665dbdbe5b9039a2e6fe00c4acfa878f3a08f458f809
```

独立した SDL2 付き GUI バイナリのビルドは、並行作業の `src/render/sdl_renderer.cpp` で `realDlssEnabled_` が未宣言となるコンパイルエラーのため失敗。描画側ファイルには手を加えていない。**今回の変更を取り込んだ GUI parity 2123/2123 は未実施**である。

## 限界

対象は「元 Interpreter の制御フレームがすべて残ったまま、最内側の単一 flat counted repeat の本体を完了し、その native Fiber が yield した境界」だけ。

- LLVM ネイティブ実行中で scheduler に戻る前の別スレッドからの直接同時書換えは対象外（Scratch の通常ソース編集もシリアルスケジューラ上で実施する）。
- LLVM SSA の演算途中位置、任意の多層ループ本体や複数手続きの非 warp yield、未完了副作用、任意の PC からの再開は未対応。
- source proof は **変更を検出する機構**であって、その後の Interpreter が変更されたソースを再コンパイル／再キャッシュする全機構ではない。実際のエディタは通常 `Blocks::resetCache` を行い、今回の特殊な無改訂編集は通常の API 契約外である。
- プロジェクトが内容の同じ別版に更新された場合も保守的にデオプトする。
- 本オプトイン中のソースダイジェストは warp の native yield ごとに再評価するため、巨大ソースでのレイテンシを測定する余地がある。通常の OSR 経路はこの処理を行わない。

コミット・プッシュはしていない。

## 最終検証（フラットループ限定・fail-closed追加後）

- 最新のコンパイル時制限（内部 nested counted repeat を含む native 領域は選択しない）、安全な post-body checkpoint を持たない yield でソースが変更された場合は絶対に LLVM を再開しない fail-closed を追加。
- この状態の整合ビルド `board/osr-inflight-verified-v2-smoke` を用い、`XYO_BLOCK_COUNT=1` で5条件（Interpreter + native4条件）の全変数・全ブロック数比較が再度 **PASS**。`__block_count = 144000043`。
- 最終ソースを取り込んで従来 `xyo-nested-warp-silent-edit` と新規 `xyo-nested-warp-inflight-deopt` の双方を再ビルドし、headless CTest **2/2 PASS**（28.02s + 39.66s、計67.68s）。
- In-flight の source proof は native fiber の安全な停止点ごとに再評価。default-OFF の経路には本処理なし。SDL2込み build は別開発の `realDlssEnabled_` 未宣言エラーのため引き続き未検証。
