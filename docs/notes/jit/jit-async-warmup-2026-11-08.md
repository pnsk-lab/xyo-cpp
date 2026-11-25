# 非同期 JIT の協調的な起動待機（2026-11-08）

## 背景

Real Project の Linux RISC-V （`1201938491.sb3`）は `RISCV.powerOn`
から warp 手続き `0.Reset RAM` の内部に長時間入る。
既存の `XYO_JIT_OSR=1` はトップレベル `repeat` を対象にし、
このような深い手続きスタック／warp からは安全に途中移行できない。
ログでは `I?` の非同期コンパイルが 100–300ms 程度で完了しても、
手続き内部の既に開始済みスレッドは Interpreter に残った。

## 実装

**実験用・既定 OFF**。
`XYO_JIT_ASYNC_WARMUP_MS=4000` のように指定すると、
GUI付き HotSpot のコンパイル処理が worker で保留中で、かつ
Scratch 命令をまだ一つも実行していない Thread の開始を最大指定msだけ
`STATUS_YIELD_TICK` で協調的に延期する。期間上限は 1–5000ms。
同期 JIT と違い、コンパイル待ちのために UI スレッドはブロックしない。

コンパイル済みコードが `Runtime::step` で公開された場合は、
従来の `!interpreterStarted` 経路が未開始スレッドへコードをアタッチする。
期間切れ・コンパイル失敗の場合は通常通り Interpreter で実行する。
実行済みコードの巻き戻し、深い warp 手続きの OSR、Fiber 復元は**行わない**。
`XYO_JIT_ASYNC_WARMUP_LOG=1` でスクリプト単位の初回延期を記録できる。

変更点：
- `Thread::asyncJitWarmupDeadlineMs` は再利用／再構築時にゼロ化。
- `JitEngine::isCompiling` はメインスレッド専用の読み取りAPI。
- `Sequencer::stepThread` 冒頭で未実行・未コンパイル・worker保留中だけを判定。
- workerの LLVM 生成方式と `XYO_JIT_OSR` の安全条件は無変更。

## 性能結果と安全条件

Linux RISC-V で通常の `XYO_JIT_ASYNC=1` は28フレーム・約12.6sで
約2.06億ブロック実行（初期化を主としてInterpreterで実行）。
`XYO_JIT_ASYNC_WARMUP_MS=4000` では105フレーム・約5.22sで
約26.4億ブロック実行した。ただし **フレーム数、起動タイミング、
実行済みScratch命令の範囲が違う**ので、速度比として割り算しない。
`XYO_JIT_ASYNC_WARMUP_MS=1000` ではworker待ち行列の先行する
重いコンパイル処理に間に合わず、途中でInterpreterに戻って効果が無かった。

最重要：この延期はスレッド間の副作用の可視化順序を変える。
`XYO_JIT_ASYNC_WARMUP_MS=400` を一般のGUI Scratch Parityへ
適用すると **2069/2121** まで低下した。
したがって **既定 ON にしない**。
性能優先の専用実験に限定し、厳密な互換性を要求する測定から除外する。

単一の長い warp 手続きだけを持つスモークテスト
`tests/differential/gui_async_jit_warmup_smoke.py` は、
Interpreter・通常 HotSpot・warmup付き HotSpot・Eager で
最終変数とリスト内容が等しいことを検証する。全般的なスケジューラ
互換性を証明するものではない。

今後の本命は深いスタックの `CallFrame`／戻り先PC／GCルートを
`RT-001` と `IR-001` で統合し、warp 手続き途中でも公開済み
ネイティブ実行へ**既存副作用を再実行せず**安全に移行すること。

## 最終回帰

- ネイティブ・Emscripten/WASMの両方を新しいThreadレイアウトで再ビルド成功。
- GUI CTest `xyo-gui-scratch-parity` と `xyo-gui-async-jit-warmup`: 2/2成功。
- `tests/differential/jit_osr.py`: 9/9成功。
- 4経路共通CFG出力一致: 33/33。未知副作用fallback: 8/8。
- 上記の出力一致は Scratch期待値への合否とは別の指標。

テスト出力: `board/realproject-warmup-final-{build,ctest,osr}.log`, `board/realproject-warmup-wasm-{build,cfg,fallback}.log`。
