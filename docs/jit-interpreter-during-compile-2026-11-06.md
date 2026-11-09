# xyo-cpp: インタプリタ実行中の非同期LLVMコンパイル（2026-11-06）

## 実装と安全性

HotSpotで環境変数 `XYO_JIT_ASYNC=1` を指定すると、Scratchスレッドのコンパイル候補を同期JITせず、インタプリタで直ちに実行しながら別スレッドでLLVM最適化・ネイティブ命令生成を進める。

1. メインスレッドで `decideCompile` を実行する。従来と同じループ/ホットネス判定を使う。
2. `LLVMBackend::prepareAsync` は **メインスレッドで** `jitEmitScript` を行い、ScratchのBlock/Thread/Runtime、GCピン留め等のランタイム情報を参照し終える。
3. 独立した `ThreadSafeContext` に閉じ込めたLLVM Moduleと実行メタデータのみをワーカーキューに渡す。
4. ワーカーはO3の `PassBuilder`、ORC `addIRModule`、`lookup` を行う。ワーカーはScratchのライブ状態を参照・更新しない。
5. ワーカーが完了結果をキューへ追加する。メインスレッドの `Runtime::step` / `JitEngine::compile` が `pollAsync` で結果を取り込む。
6. 同じスクリプトの次回起動時、またはまだ1ブロックも解釈していないスレッドではネイティブへ切り替える。**実行中のループ・手続き途中でのOSRは行わない**（途中から元のJITエントリを実行すると処理済みの副作用を二重実行するため）。

JITバックエンドに `PendingJitCompilation` という2段階インターフェイスを追加した。現在のLLVMバックエンドで実装し、バックエンドが非対応なら従来の同期コンパイルへフォールバックする。

- `JitEngine::Impl` が専用ワーカー1本、待機/完了キューとmutex/condition_variableを所有する。
- 同一トップブロックのコンパイル要求は `compiling_` で重複排除する。最大32トップブロック分のコンパイル要求まで準備し、それ以上はインタプリタで継続する（後続呼出しで再試行可）。
- `invalidate()` は世代番号を進め、前世代のコンパイル結果を破棄。古いブロックグラフの機械語を公開しない。
- 破棄時はワーカーを停止してjoinし、**バックエンドの寿命より前に確実に終了**させる。
- 外部の `jitMode=Off` に切り替えた後は、完了したJITを未実行スレッドに自動適用しない。

## 起動時間

同一バイナリ SHA256 `684f9e1727f6bc0339d2aaf75dbfcf1e574edfc5184308a5e39fc1b226675672` （追加の32件上限・OFFガード適用前）、CPU12に固定し各10ペアの順序を乱択して測定。比較のため `XYO_JIT_FAST_START=0` に統一。

| SB3 | 同期JIT | 非同期JIT | 速度比 |
| --- | ---: | ---: | ---: |
| app_ast | 123.29 ms | 11.87 ms | 10.36倍 |
| app_brainfuck | 110.37 ms | 10.57 ms | 10.44倍 |
| app_life | 209.37 ms | 10.90 ms | 19.02倍 |
| algo_sort | 78.75 ms | 10.22 ms | 7.72倍 |

**起動準備時間の定義**: `cmdRun` から `read-sb3`、`Runtime` 初期化、プロジェクト構築、`greenFlag()` まで。プロセスの動的リンク時間、`rt.step()`、ワーカーのコンパイル完了待ち、作品実行終了までの時間は含まない。

計測: `build/async-jit/bench_startup.py`、`build/async-jit/startup-ab.json`。

## 正しさ

`tests/differential/jit_async_compile.py` はScratchの実SB3を生成。ブロードキャスト受信器が2000反復の変数更新を8回行い、送信側は間隔を空けて待つ。インタプリタ、同期HotSpot、非同期HotSpotで最終的な `sum=16000`、`done=1` を比較し、バックグラウンドの結果公開も確認。CTestに登録。

## 制限と次の段階

- **OSR未実装**: 長時間実行される単発のScratchスクリプトはコンパイル完了後もインタプリタに留まる。したがって現在は `XYO_JIT_ASYNC=1` で明示的に有効化する実験機能であり、既定は従来の同期JIT。Eager JITとAOTは従来通り。
- スクリプト終了前にプロセスが終了する場合、バックグラウンドワーカーの安全なjoinがあるためプロセスwall timeにはコンパイル待機が含まれ得る。
- `XYO_JIT_ASYNC_TIMING=1` でワーカーの作業時間と公開を記録できる。
- 今後はインタプリタのループ状態とLLVM側SSAの状態を対応付けるOSRエントリ、フレーム/変数/リスト/warp時計の再構築が必要。これができるまでは安全なスクリプト開始点のみ切り替える。
