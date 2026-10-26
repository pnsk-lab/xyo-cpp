# HSV変換の整数化とCPUサンプリング

## 調査で分かった負荷

[ベンチ専用サンプラ](<../tests/bench/pc_sampler.hpp>)を追加し、3D2のウォームアップ後90描画フレームについて、主スレッドの実行位置を採取した。従来の不完全なgprofngデータとは別の計測である。

Linux x86_64の`CLOCK_THREAD_CPUTIME_ID`と`SIGEV_THREAD_ID`を使う。ハンドラは命令ポインタを事前確保したlock-free atomic配列へ保存するだけで、割り当て・ロック・ファイル出力は行わない。採取は既定で無効。準備と出力はベンチの時間計測区間外に置く。

| 周期 | 主スレッドCPU時間 | 期待イベント数 | 実サンプル数 | overrun / dropped |
| --- | ---: | ---: | ---: | ---: |
| 2000 µs | 2415.585 ms | 1207.792 | 1207 | 0 / 0 |
| 2300 µs | 2432.431 ms | 1057.579 | 1057 | 0 / 0 |

両方でタイマーの採取範囲を確認できた。`hsvToRgb`は全サンプルの約9.5〜11.1%、呼び出し元の`updatePenColor`は約5.1%だった。匿名実行領域は約51〜53%だが、これはJIT候補として分類しただけで、個々の生成関数まで特定した値ではない。

サンプリングは実行を乱すため、採取した実行時間を高速化のA/B値には使わない。通常のA/Bでは`XYO_BENCH_SAMPLE`を外す。

## 変更内容と同値性

[HSV変換](<../src/core/color.cpp>)では、最終的なRGBの整数化が次の形になっていた。

```cpp
static_cast<int>(std::floor(channel * 255))
```

有限の色相について、既存処理の順序を保つと以下が成り立つ。

1. 色相の正規化結果は0〜360。負の最小subnormalなどでは、360を足した丸めでちょうど360になる場合もある。
2. `i = int(h / 60)`は0〜6、`f = h / 60 - i`は0以上1未満。sector 6は従来どおりswitchのdefaultを通る。
3. 既存の順序付きmin/maxで、彩度・明度は0〜1に収まる。NaN・負の無限大は0、正の無限大は1になる。
4. 元の式で求めるp/q/tとRGB各成分は0〜1、最後の255倍も有限の0〜255になる。
5. この範囲では、整数への切り捨てとfloor後の整数化は同じで、intの範囲にも収まる。

そこで、有限の色相では最終のfloorを省いた。途中の掛け算・引き算・除算の結合順、色相正規化、clamp、sector選択は変えていない。近似・量子化・fast-mathの導入ではない。無彩色も同じ非負範囲の証明を使う。

`XYO_HSV_FLOOR_GENERIC=1`で従来のfloor経路へ戻せる。非有限の色相は従来経路へ残したが、元の実装には非有限値からintへの変換があり、そのC++上の未定義動作を今回定義し直したわけではない。

## 同値性テスト

[独立した元の式との比較](<../tests/unit/test_color_fast.cpp>)を拡張した。

- 全sector境界と隣接double、ちょうど360へ正規化される負の微小値。
- 12色相×全256彩度×全256明度と、各彩度・明度の隣接double。
- RGBの整数境界に近い補間値、巨大な有限色相、NaN/Infの彩度・明度。
- `XYO_TEST_COLOR_EXHAUSTIVE=1`では、全16,777,216 RGBのrgbToHsv結果も元のHSV式と比較。

全数検査込みで24,341,040チェック・失敗0。通常の7,563,824チェックも従来floor経路で失敗0だった。RGB入力へ戻ることを保証する検査ではなく、元のRGB→HSV→RGB往復で生じる丸めをそのまま保つ検査である。

[比較・リストの追加回帰テスト](<../tests/unit/test_literal_context.cpp>)も219,850チェック・失敗0だった。インタプリタ・JIT・別RuntimeへロードしたAOTで、文字列型、TABと他の空白の比較差、Infinityの大小文字、特殊リスト添字、置換ITEMの副作用・評価順、RNG消費を確認する。

## 描画時間の最終計測

最初のCPU affinityを指定しない30フレームの測定は、同一描画量でも793〜3622 msまで振れた。ホスト負荷も高かったため、その中央値の改善率は結論に使わない。

追加した`XYO_BENCH_CPU=12`で、ウォームアップ後のベンチ主スレッドだけをCPU 12へ固定し、90描画フレームを交互に各5回測った。CPU 12はSMTの12–13組・最大4.8 GHzのPコアで、Eコアの最大3.6 GHzとは区別できる。通常再生のaffinityやFPS設定を変えたわけではない。

| Res=1・90フレームの中央値 | 従来floor | 整数化 |
| --- | ---: | ---: |
| 描画込みwall time | 2474.249 ms | 2433.375 ms |
| 全プロセスCPU時間 | 2461.888 ms | 2401.124 ms |
| スループット | 36.375 FPS | 36.986 FPS |
| wall time範囲 | 2428.139〜2573.048 ms | 2344.519〜2532.052 ms |

wall timeは約1.65%、CPU時間は約2.47%短かった。ただしwall timeは5組中3組で短縮、2組では小幅に逆転し、範囲も重なる。小さい改善として扱い、全ての実行や解像度での高速化を保証しない。

全試行で15,552,000ストローク、quads同数、draw=90、flush=180、CPU readback/upload=0を照合した。Res=4は描画確認の各1回で242.578→249.162 msと逆転した。両版とも972,000ストローク、draw=90、flush=90で、この単発値から速度の結論は出さない。

画像の差はFPS表示内のみだった。Res=1の873画素はx=630..718・y=54..69、Res=4の908画素はx=631..730・y=54..69で、それ以外はbyte一致した。

計測ログと再実行スクリプトは`build/hsv-trunc-ab/`にある。全体ビルドは別途編集中のエディタAPIの引数不一致で一度停止したが、[共有化の最終検証](<list-string-sharing-performance.md>)時に成功した。CTestは6/6、JIT・インタプリタ・共有無効のunit suiteとexecuteフィクスチャも全て成功した。通常の色変換検査は各7,563,824チェック・失敗0だった。これは先の全数色検査とは別の再確認である。

## 不採用の試行

| 試行 | 対照の中央値 | 試行の中央値 | 比較 |
| --- | ---: | ---: | --- |
| 定数リスト添字の展開 | 749.557 ms | 785.315 ms | 各3回 |
| 定数比較の展開 | 749.557 ms | 760.716 ms | 各3回 |
| 上記2つの併用 | 749.557 ms | 795.996 ms | 各3回 |
| ペン直径1/3の特化 | 784.730 ms | 787.378 ms | 交互各5回 |

定数の試行は対照の範囲も748.399〜844.498 msと広く、改善の根拠にできなかった。直径特化も有利と確認できず、3案とも実装と切替オプションを撤去した。直径特化の画像・GPU記録の回帰テストは両経路で成功していた。

## サンプリングの再現

```sh
# 実行ファイルを保存する。後から再ビルドしたバイナリで古いPCを解決しない。
cp build/tests/xyo-pen-bench build/sampled-pen-bench
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
XYO_SEED=1 XYO_BENCH_WARMUP_FRAMES=60 \
XYO_BENCH_SAMPLE=build/sample XYO_BENCH_SAMPLE_US=2000 \
build/tests/xyo-pen-bench --project 3D2.sb3 --frames 90

python3 tools/analyze_pc_samples.py build/sample \
  --executable build/sampled-pen-bench --json build/sample-summary.json
```

[解析スクリプト](<../tools/analyze_pc_samples.py>)はPC数、CPUタイマーの期待イベント数、overrun/drop、書き出し成否を確認する。大量の欠落やイベントの併合がある場合は警告し、その割合を代表値として扱わない。データは`.pcs`、`.maps`、`.json`へ保存する。シグナルが既に使用中・block中・pendingならサンプリングを開始しない。Linux x86_64以外は、明示的に有効化した場合だけ非対応のメッセージを出す。

今回の生データと集計は`build/list-literal-ab/profile-2000*`と`profile-2300*`にあり、解析には[保存した対照バイナリ](<../build/hsv-trunc-ab/probe-base>)を使う。
