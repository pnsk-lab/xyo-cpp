# jitStepThread のFiber再開コスト分解（2026-11-14）

対象：xyo-cpp の通常Fiber経路（native Fiber resume）。共有dirty worktreeの変更はコミットしない。

## 実装

- src/engine/sequencer.cpp に計測専用 XYO_JIT_NATIVE_RESUME_PROFILE=1 を追加。
- プロファイル有効ビルド（XYO_DISABLE_RT_PROF=OFF）に限定し、OSスレッドごとに64回に1回、3区間を steady_clock で計時。
- 1. 時計ポインタ4個の設定、2. Fiber::resume、3. 再開後の状態確認・終了時のコンテキスト解放。
- 集計は atomic で行い、診断結果は [jit-resume] として終了時出力。
- 通常Release（XYO_DISABLE_RT_PROF=ON）では計測処理を完全に除外。
- XYO_JIT_CLOCK_PTR_BUNDLE=1（既定OFF）を追加。従来4回個別に行っていたWarpClockWatchdog::armedの読み取りを1回へ統合。
- 監視時計の atomic 値そのもの、デッドライン、Unix時計、JitCtxの可視ポインタのNULL条件、Fiber切替およびGCの管理方法は不変。

## プロファイル結果

別Releaseビルド build-scheduler-prof-20261113、XYO_JIT_MODE=eager、時計ポインタ統合OFF。
最初のプロファイル実行における標本平均（単位ns、計測オーバーヘッド含む）：

| ケース | 標本数 | 時計設定 | Fiber.resume(LLVM含む) | 再開後 |
|---|---:|---:|---:|---:|
| sched_pingpong | 109474 | 14.18 | 183.31 | 14.93 |
| sched_clonechurn | 109943 | 14.26 | 195.86 | 14.43 |
| 循環依存合成SB3 | 1875 | 14.09 | 28.30 | 13.13 |

追加の同一計測専用バイナリ・同一pingpong SB3の6組交互比較（時計設定区間の1回平均の中央値）：
- OFF 時計設定：33.23 ns
- ON 時計設定：28.49 ns（約4.74ns短縮）
- Fiber.resumeなど他区間は変動があり、end-to-endの大幅改善は見られなかった。

これらの数値はネイティブ命令一つの絶対所要時間ではなく、標本内の区間差である。
測定環境の競合やウォームアップの影響があり、最初のプロファイルと6組測定を直接比較しない。

## ScratchBench ON/OFF A/B

同じ通常Releaseバイナリ、CPU16固定、XYO_JIT_MODE=eager。
対象カーネルのみ選択、12組交互。スコアが大きい方が良い。

| ケース | OFF中央値 | ON中央値 | 対応ペアON/OFF速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| sched_pingpong | 414.060 | 418.130 | 1.0123倍 | 9/12 |
| sched_clonechurn | 403.482 | 408.707 | 1.0104倍 | 8/12 |
| sched_threads | 12103.919 | 12216.476 | 1.0035倍 | 7/12 |

記録：build-scheduler-rewrite-20261111/jit-step-20261114/bench-clock-bundle/results.json

pingpong単独・20組再測定：
- OFF中央値419.032、ON中央値420.385、対応ペア速度比1.0073倍、ON勝利11/20。
- ON/OFFの優劣が安定せず、既定ONにするには根拠不十分。
- 記録：build-scheduler-rewrite-20261111/jit-step-20261114/bench-clock-bundle-pingpong-isolated/results.json

## 結論と今後

時計ポインタの取得費用は実測で約10〜15ns規模。まとめると設定区間に数nsの削減は見えるが、
ScratchBenchへの波及は約0〜1%程度でノイズとの区別が難しいため既定OFFを維持する。
性能改善の大きい経路は、C++の小さなgetter統合ではなく、Fiberへ戻る回数自体を減らすこと。

## 回帰

ビルド：build-scheduler-rewrite-20261111 の xyo-tests/xyo-cli、build-scheduler-prof-20261113 の xyo-cli 成功。
全体回帰：XYO_JIT_MODE=eager / XYO_JIT_CLOCK_PTR_BUNDLE=1、Scheduler 453/453 PASS、その他全テスト群失敗ゼロ、終了コード0。
ログ：build-scheduler-rewrite-20261111/jit-step-20261114/full-eager-clock-on.out
ctestのFiber smoke、native GC遅延smoke、複数Fiber GC barrier smokeは3/3 PASS。
git diff --check: PASS。
変更は未コミット、他エージェントの変更を保持。
