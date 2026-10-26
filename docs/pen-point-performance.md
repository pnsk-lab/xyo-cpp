# 3D2 の点ペン投入オーバーヘッド削減

## 変更

3D2 の主描画手続きは、各画素についてペンを下ろし、直後に上げる。このため主な API は移動線分ではなく `penPoint` である。

GPU のインスタンスレコード経路に限り、点を汎用線分へ変換せず、既存と同じ10 float のレコードを直接追加する。端点の差分、長さの平方根、汎用線分・CPUラスタ・クアッド生成関数の呼出しを省く。レコード追加は線分と共有し、順序、100,000本での途中 flush、シェーダ、色、ブレンドを変えない。

直径の正規化と始点の検証も共通関数を使う。元の double が厳密に1または3の場合だけ半画素補正する。float に丸めた幅で判定しない。ゼロ・NaN直径の扱い、負の微小数がfloatの負のゼロになる場合、非有限座標・float範囲を超える座標の拒否も従来どおり。

CPUラスタ、旧GPUクアッド、プロファイル・ログ・トレース・描画スキップは汎用経路を維持する。`XYO_PEN_POINT_GENERIC=1` で同一バイナリの点専用経路を無効化できる。描画品質の切替ではなく、A/B計測用である。

`PenStats::records` はインスタンス描画に実際に送ったレコード数を集計するよう修正した。点の投入数・quad数は従来どおり、詳細プロファイルが無効でも数える。

## 計測条件と結果

RTX 3070 / NVIDIA OpenGL 4.3、SDL offscreen、stage 480×360、表示scale=2。詳細プロファイル、ペンログ、トレースは無効。別プロセスのCPU/GPU負荷があり、wall timeは大きく変動した。

### 実トレースの連続投入

既存の3D2トレース200,000点を100回繰り返し、計20,000,000点を投入。各実行の前に同じ負荷で2秒GPUをウォームアップ。変更前に保存した実行ファイルと変更後を交互に5回ずつ実行した。

| 区間（5回の中央値） | 変更前 | 点専用経路 |
| --- | ---: | ---: |
| 投入wall全体（途中GPU flushを含む） | 315.8 ms | 322.9 ms |
| 投入wallからflush計時分を差し引いた区間 | 241.9 ms | 148.5 ms |
| 上記区間の1点あたり | 12.10 ns | 7.43 ns |

flushを除く投入区間は約39%短縮した。ただし、これはCPUの実行時間そのものではなく、wall timeの区間差分である。連続投入では変更後のGPU flush待ちが増え、**総wall timeの改善は確認できなかった**。この結果を「ペン全体が1.6倍」「FPSが1.6倍」とは解釈しない。

### 実プロジェクトの描画あり比較

追加した `--project` モードはロード・起動・JITの初期処理を含む開始待ちを除外し、100,000点を超える描画をした10フレームをウォームアップする。その後はCLIのsleepなしで30 active framesを測る。`Runtime::step()` が呼ぶ既存の描画を利用し、重複して `draw()` を呼ばない。時間計測後にのみスクリーンショットを保存する。

同一バイナリの汎用経路と専用経路を交互に5回ずつ実行した。全10回で、計測区間は以下が一致した。

- active frames / steps / draws: 30 / 30 / 30
- strokes / quads: 5,184,000 / 5,184,000
- GPU flushes: 60
- CPU readbacks / uploads: 0 / 0
- startup steps: 97、active warmup: 10

| 30描画フレーム（5回の中央値） | 汎用点経路 | 点専用経路 |
| --- | ---: | ---: |
| wall time | 1221.088 ms | 1182.775 ms |
| 実測FPS | 24.568 | 25.364 |

中央値では約3.2%高いFPSだった。ただし個々の試行は汎用1023〜1906ms、専用1119〜1902msまで揺れたため、**エンドツーエンドの約3%差を安定した改善率とは断定できない**。入力から表示までの負荷は同じであり、ペン投入CPU処理の削減は確認できたが、FPS頭打ちの解消を示す結果ではない。

別の30フレームA/Bで保存した960×720の画像は、実時間依存のFPS数値表示内の848画素だけが異なった（差分bounds x=630..719, y=54..69）。それ以外の画素はbyte一致した。

## 再計測

```sh
cmake --build build -j 4

export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=dummy
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export XYO_SEED=1
unset XYO_PROF XYO_PENPROF XYO_PENLOG XYO_PENTRACE XYO_PENSKIP XYO_PEN_POINT_GENERIC

# 同一バイナリで交互に複数回。起動と10 active framesを除外する。
XYO_PEN_POINT_GENERIC=1 build/tests/xyo-pen-bench --project 3D2.sb3 --frames 30
build/tests/xyo-pen-bench --project 3D2.sb3 --frames 30

# --project は3D2用のクリックと >100000 strokes/step の活動判定を使う。
# 他作品一般のベンチマークではない。
# process_cpu は計測区間の全プロセススレッドのCPU時間で、GPU時間ではない。

# 実トレースがある場合。総投入数とflush数も照合する。
build/tests/xyo-pen-bench --workload real_trace \
  --trace build/bench/3d2_200k.trace --warmup-ms 2000 --reps 100 --frames 1

# 実GPUの点/ゼロ長線分 parity。CPUや旧quadへのフォールバックは失敗扱い。
XYO_TEST_PEN_RECORDS=1 XYO_DEBUG=1 build/tests/xyo-render-tests

ctest --test-dir build --output-on-failure
```

## 回帰検証

点とゼロ長線分の比較は、明示的なGPU readback後に行う。NaN/±Inf/±0、subnormal、double→float overflow、幅1/3の隣接double、半透明色、stage端点を含む。99,999 / 100,000 / 100,001本のバッチに異なる半透明色の点と線を交互に投入し、順序・途中flush・clearによる未描画バッチ破棄も検査する。実GPUでは標準stageと640×480のカスタムstageの両方を検査する。

最終ビルドでCTestは6/6成功。実GPUの上記parityは標準stageとカスタムstageを合わせて392チェック失敗0、CPU/interpreterのrendererテストは `XYO_JIT_DISABLE=1` で1088チェック失敗0だった。保存した変更前バイナリと最終ビルドで同じ200,000点トレースを再生した画像も、PPM全体がbyte一致した。

なお、既存のborderテストはJIT期待判定で `XYO_JIT_DISABLE` だけを参照するため、`XYO_JIT_MODE=off` 単独では「コンパイル済みである」というテスト側のassertが1件失敗した。描画検査の失敗ではなく、今回は従来のテスト用フラグでinterpreter検証を行った。

今後さらに削る場合も、GPU待ちを含む総時間と、ストロークを組み立てるCPU側区間を分けて評価する。過去に中立だった「各セグメントが結局汎用penStrokeを通るポリライン化」は再導入していない。
