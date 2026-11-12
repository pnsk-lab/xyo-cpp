# RT-001：メモリ予算によるwarp移行でのスケジューラ順序維持（2026-11-09）

## 発見した問題

RT-001 の 1 MiB GC ソフト予算は、LLVM Fiber を `AfterLoopBody` で安全に一時停止し、復帰証明を検査した後に Interpreter へ移行する。しかし `Sequencer::stepThread()` は復帰後そのまま `return` していた。つまり、**エンジン変更のための一時中断が、本来の Scratch のスケジューラ yield に化けていた**。

2 本の緑の旗スクリプトを実際に実行して検出した。1 本目は declared-warp 手続き内で 17,000 回の Unicode 文字列結合と `x += 1` を繰り返す。2 本目は共有変数 `x` を `y` に記録し、別ケースではさらに `x=777` と書き込む。

| ケース | Interpreter / budgetなし Eager | 修正前 budget=1MiB | 修正後 budget=1MiB |
| --- | --- | --- | --- |
| 読み取りのみ | x=17000, y=17000 | x=17000, y=14208 | x=17000, y=17000 |
| 読み取り後に書き込み | x=777, y=17000 | x=3569, y=14208 | x=777, y=17000 |

修正前のデオプトは `adopted=1`、GC回収も正常だったため、単一スクリプトの最終状態だけでは検出できなかった。

さらに、SSAワードを改変してフィンガープリントだけ再計算した場合、`adopted=0, collected=0` で安全に復帰拒否しても、**人工的なwarpの中断が残り、他スクリプトが途中状態を観測する**ことも確認した。

## 修正

`src/engine/sequencer.cpp` の `Sequencer::stepThread` に限定。

1. **移行成功時**：`budgetRequest && adopted && restored warpMode` の場合、Fiber の解放とGC後に `stepThreadBound(thread, 0)` を同じ scheduler pass で呼ぶ。リスタートの際に新たな 500 ms を付与せず、同一 steady clock の場合は `thread.warpTimer` の開始時刻を `sequencer.timer.startTime()` に揃える。warp期限を過ぎれば Interpreter 側の通常のループラッチが yield する。
2. **移行失敗時**：`budgetRequest && JitCtx::isWarp` を復帰証明とは独立に中断直後に記録する。証明が失敗したなら、元の *live native Fiber* を1回だけ `jitStepThread(thread)` で同じ scheduler pass 内に続行する。native 内の通常warp期限・明示的 yield はそのまま残る。既存の `nativeGcSoftBudgetAttempted` one-shot フラグにより、同じGC予算を繰り返し要求しない。
3. **非warpへの影響を避ける**：両方ともメモリ予算による割り込みで、かつ現在実行中のwarp状態のときに限る。テスト用のNth-deopt、非warpループの通常yield、正当なwaitやwarp期限でのyieldは変更しない。

SSA/GCの証明は緩和していない。SSA/Source が不正な場合は常にデオプトを拒否し、native Fiber が終了するまでGCを行わない。

## 実測

隔離した LLVM23 Release CLI（以前検証済みのフルソース静的ライブラリを複製し、**修正済みの現在の `sequencer.cpp` だけを再コンパイル・置換・再リンク**。共有ビルドや元のライブラリは変更していない）：

`/home/kicky/.rt001-warp-multihat-isolated/xyo`

SHA256:
`bdf9678f23fdd949e1d1848094d1e6afe1016b0bae4a5baad6715c69a0291580`

メモリ予算で復帰する読み取りのみケース：1,050,350 B → 31,482 B、復元warpフレーム2。書き込みケース：1,054,466 B → 31,638 B、復元warpフレーム2。両方とも最終変数がInterpreterと一致。

追加回帰テスト `tests/differential/native_simple_resume.py`：

- `ssa_budget_warp_two_hats_observer`
- `ssa_budget_warp_two_hats_writer`

それぞれ正常移行、8 MiB quiet、SSA改変拒否＋Fiber終了後GC、SSAルート所有者なしの4条件を確認。計8実行を追加。

## 制約

今回の修正で一般的なLLVM SSA/任意副作用地点からの復帰が可能になったわけではない。ソフト予算も実メモリの強制上限ではない。GCの対象に他の suspended native Fiber が含まれるケースの全般的なルート管理、編集によるsource変更、並列実行の競合については別途追加検証が必要。

共有作業ツリー内の他タスクの変更は保持。コミット・pushなし。RT-001は in_progress、実験フラグは既定で無効。

## 回帰テスト確定結果

```bash
python3 tests/differential/native_simple_resume.py \
  --native /home/kicky/.rt001-warp-multihat-isolated/xyo \
  --out build/rt001-warp-multihat-final
```

**946/946 native resume/fail-closed parity runs PASS**。
`build/rt001-warp-multihat-final.log` に全体ログを記録。

- 既存の非warp、root-only、再帰、ソース改変、SSA/GC所有権、複数hat回帰を含む。
- 今回追加した複数hatの正常・拒否・無圧力・GCルート保証なしの計8実行を含む。
- `python3 -m py_compile tests/differential/native_simple_resume.py` PASS。
- `git diff --check -- src/engine/sequencer.cpp tests/differential/native_simple_resume.py` PASS。

隔離ビルドは差分のある `sequencer.cpp` を再コンパイルして既存のフルソースLLVM23アーカイブにリンクしたものであり、**現在の全未コミット変更を含めた完全な再ビルドではない**。その制約を越えた検証完了とは主張しない。
