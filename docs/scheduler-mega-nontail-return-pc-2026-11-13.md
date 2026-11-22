# Mega Scheduler β — 非末尾自作ブロックの明示的な return PC

日付: 2026-11-13
状態: opt-in / 既定OFF / 未コミット

## 対応した制御フロー

既存の「緑旗→引数なし・非warp自作ブロック（末尾）」を拡張し、
「緑旗→引数なし・非warp自作ブロック呼出し→定数変数加算（後続1命令）」
をソース証明済みの条件として受け入れる。

手続き本体は旧Mega-Yieldの以下3形式のみ。
- 非warp repeat(非負の整数リテラル) { 定数変数加算 }
- 固定ASCIIメッセージの broadcast and wait → 定数変数加算
- 非warp repeat(非負の整数リテラル) {
    固定ASCIIメッセージの broadcast and wait
  } → 定数変数加算

ソース解析 `src/jit/mega_yield_source.hpp` で後続の
`id`・`name`・`delta` を別に保存。
`src/jit/jit.cpp` と `backend_llvm.cpp` で両方の変数参照を確認。

共有LLVM `switch(pc)` で、従来のスロットの開始PC `2*i`、
再開PC `2*i+1` に加えて、戻り先PC `2*N+i` を発行
（Nは同一Targetで融合したScript数）。
calleeの反復/待機中はThread-owned `megaPc` と
`megaRemaining` を保持する。calleeが完了した瞬間に
戻り先PCの基本ブロックへ分岐し、caller側の変数加算を一度だけ行う。
LLVMの同じ呼び出し内で分岐するため、帰還のためだけに
余分なScratchスケジューラの選択回数を追加しない。

一つのLLVM関数の各種状態へのジャンプであり、
一般の動的な再帰用CallFrame、引数スタック、複数ネスト戻り先はない。
return状態自体にはGC管理オブジェクトを保存しない。
通常のSource revision・変数binding generationチェックは維持する。

## フォールバック・未対応

- 引数がある、warp付き、reporter形式、再帰、入れ子自作ブロック
- callerに後続2命令以上、任意の副作用、動的な演算式
- 複数段の呼び出し、動的repeat-until、イベント受信側との融合
- AOT/WASM、非eager、描画あり、途中実行Thread

これらは既存のJIT/インタプリタへ戻し、既定ONにはしない。
ScratchBench sched_* は単純な証明条件を満たさないため、
今回の修正で高速化したとは主張しない。

## 実行

```sh
XYO_JIT_MODE=eager XYO_JIT_MEGA_BATCH_YIELD=1 \
  XYO_JIT_MEGA_BATCH_TRACE=1 \
  ./build-scheduler-rewrite-20261111/tests/xyo-tests
```

## 回帰テスト

`testMegaNonTailProcedureReturnPc` を `tests/unit/test_scheduler.cpp` に追加。
- 2本の独立した非末尾repeat→caller後続加算
- broadcast-and-wait→callee末尾加算→caller後続加算
- repeat(0)が本体を書き込まず即座にcallerを実行
- callerに後続2命令があるケースは共有switchから拒否して通常経路へ
- 反復途中・戻り前の値、完了後の合計、Fiber未使用、緑旗2回目のコード再利用

## 実測テスト結果

- CMake Ninja: `xyo-tests` ターゲットの再ビルド PASS
- `XYO_JIT_MEGA_BATCH_YIELD=1`: `xyo-tests` PASS。Scheduler **473 checks / 0 failures**。
- `XYO_JIT_MEGA_BATCH_YIELD=0`: `xyo-tests` PASS。Scheduler **419 checks / 0 failures**。
- 新ケースの融合ログ: `[mega-yield] slots=4 definitions=1 states=12`
- 変数 `s` の合計: 初回413、同一コンパイルキャッシュで2回目826。
- `git diff --check`: PASS（最終確認時）。

既存のMega-relatedフラグは個別の独立したβ機能。
ユーザーや別エージェントの共有未コミット変更は保持し、commit/pushはしない。

