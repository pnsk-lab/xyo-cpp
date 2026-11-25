# Scheduler cleanup / retireThread 調査 (2026-11-13)

状態: 共有dirty worktree、未コミット。ほかのエージェントの変更はresetしない。

## 実装

- `src/engine/sequencer.cpp`: `XYO_SCHED_COMPACT_SUFFIX=1`（既定OFF）を追加。最初の完了Threadの前にある生存prefixは位置を変えず、runtimeIndexが古い場合のみ修復する。doneThreadsの順序、完了ThreadのreleaseFinished、threadMap更新、スケジューラ構造世代の発行は従来と同じ。
- `src/engine/sequencer.cpp` と `.hpp`: 診断 `XYO_SCHED_CLEANUP_PROFILE=1` を追加。compact呼出し、走査スロット、完了数、移動数、compact/retire累積ナノ秒を報告。64件に1件の標本からmap削除、releaseFinished、doneThreadsへの追加の内訳も出す。
- `src/engine/sequencer.cpp`: 終了する旧Threadと同じIDを別の生存Threadが使う場合、`threadMap`をIDだけで削除しないよう、エントリの`Thread*`も一致した場合だけ削除する。終了したThreadが別Threadの登録を誤消去する問題を防止。
- `tests/unit/test_scheduler.cpp`: 正順の圧縮、ほぼ全件完了、完了なし、空配列、古いruntimeIndex、同じIDの生存新Thread保持のテストを追加。環境変数 `XYO_TEST_ONLY_COMPACTION=1` と `XYO_TEST_ONLY_STACKLESS_CORO=1`で集中実行可能。

## ScratchBench 既存ワークロードのON/OFF

同一CLIバイナリ、CPU16固定、XYO_JIT_MODE=eager、Mega実験フラグ同一。違いは `XYO_SCHED_COMPACT_SUFFIX` のみ。8組交互、スコアは高いほど良い。

| 項目 | OFF中央値 | ON中央値 | 対応ペアON/OFF速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| sched_clonechurn | 244.281 | 240.500 | 1.0482倍 | 5/8 |
| sched_threads | 7265.032 | 6527.941 | 0.9455倍 | 3/8 |
| sched_pingpong | 255.283 | 240.637 | 1.0338倍 | 3/8 |
| sched_fanwait | 479.542 | 471.974 | 0.9970倍 | 3/8 |

clonechurn単独12組ではOFF中央値229.742、ON中央値230.521、対応ペア速度比 **0.9773倍**、ON勝利6/12。安定した改善がないため既定OFF。

結果: `build-scheduler-rewrite-20261111/compact-suffix/bench-sched-ab/results.json`
単独: `build-scheduler-rewrite-20261111/compact-suffix/bench-clonechurn-isolated/results.json`

## cleanup時間の実測

`XYO_SCHED_CLEANUP_PROFILE=1`を使った同一ワークロードのある1回では:

- compact_calls=799643
- scannedSlots=2801785
- removedThreads=2002143
- relocatedThreads=0
- compactionNs=153888356（153.9ms）
- retirementCalls=2468143
- retirementNs=63896015（63.9ms）
- mapEraseHits=2002143（完了Threadすべてにmap登録あり）
- samples=31283（完了64件に1件）
- 標本平均: `threadMap`削除55.1ns、`releaseFinished()`42.9ns、doneThreads追加14.0ns

これらの時間は計測フラグONによる計時の影響を含み、アプリ全体時間の占有割合は単独では断定できない。移動0件のため、メモリ移動量削減よりIDのmap操作と終了後の後片付けに費用が偏っている。

## 検討して採用しなかった案

- std::unordered_mapをstd::pmr::unsynchronized_pool_resourceのmapに置き換える独立実験は、100万件のinsert/eraseについて通常約42-43msに対してPMR pool約58msで遅かったため採用しなかった。Runtimeのmap型は変更していない。
- `threadMap.erase(key)`と`find(key)`+`erase(iterator)`+ポインタ確認の独立実験はそれぞれ約40ms/100万件でほぼ同速度。ポインタ確認版は誤削除防止のため採用。
- 全生存prefixに対するruntimeIndex書込削減は、実入力でrelocatedThreads=0かつON/OFFが改善せず、既定OFF。

## 回帰テスト

- 独立compactionテスト: `XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_TEST_ONLY_COMPACTION=1`で従来・suffix両経路 **22/22 PASS**。
- build: `cmake --build build-scheduler-rewrite-20261111 --target xyo-tests xyo-cli -j 4`成功。
- JIT=eager + MegaフラグON + suffix実験ONの全体テスト: Scheduler **613/613件PASS**、他のスイートも失敗ゼロ、終了コード0。
  ログ: build-scheduler-rewrite-20261111/compact-suffix/final-all-on.out
- 待機通知レジストリON + suffix実験ONの専用テストも **22/22件PASS**。
- git diff --check: PASS。

検証用ソース: `build-scheduler-rewrite-20261111/compact-suffix/bench-threadmap-pool.cpp` と `bench-threadmap-erase-identity.cpp`（製品コードへ統合しない）。
