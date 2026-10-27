# 数学演算結果のboxing削減

## 調査と変更

[初期リスト文字列の共有](<list-string-sharing-performance.md>)後の3D2を、GPU描画・90フレーム・主スレッドCPU 12固定でサンプリングした。CPUタイマーの期待イベント数は約1,064.3、実サンプルは1,064、overrun/dropは0だった。匿名実行領域は46.33%で、個別関数を特定した割合ではない。ネイティブの`xyo_num`は11サンプル、全体の約1.03%だった。この採取中の時間は性能A/Bには使っていない。[採取結果](<../build/list-intern-final-gl-ab/profile-summary.json>)を保存した。

3D2の`Scanline`内側ループでは、テクスチャ添字を計算する分岐に`floor`が2か所ある。従来のJITは数学演算の結果を外部`xyo_num`へ渡し、返されたValueを後続の算術で再び型判定していた。2か所という値は静的な配置であり、毎描画点で必ず2回呼ばれるという意味ではない。

[コード生成](<../src/jit/codegen.cpp>)を変更し、`operator_mathop`の結果を既存の`emitCanon`で直接Value化した。`xyo_num`は`Value::numCanonical`を呼ぶだけで、`emitCanon`も同じNaN正規化を行う。数学演算そのもの、入力の評価順、浮動小数の演算順、描画量は変えていない。

- NaNはcanonicalなValueへ正規化する。
- 有限値、±0、±Infinityのビット列を保持する。
- 結果を直後の算術へ渡す場合も、Scratchの規則に従ってNaNを0へ変換する。
- 既知のnumberとして扱えるため、直後のbool／文字列タグ検査を省ける。
- 変数やリストから読み出す未知の値を、numberだと仮定する変更ではない。

切替はコード生成時の`XYO_JIT_MATH_BOX_GENERIC=1`で、従来の外部boxingへ戻せる。生成後のホットループに設定確認の分岐は追加していない。AOTも生成時に同じ規則を使うが、生成済みオブジェクトを実行時の環境変数で変更することはできない。

## IRと互換性の検証

`floor(x)+1`をリスト添字に使う[最小プロジェクト生成器](<../build/math-box-ab/make_ir_probe.py>)でO2後のIRを比較した。従来経路には`xyo_num`と、その返り値に対する`xyo_to_num`が残る。変更後は両呼び出しが消え、`floor`と後続加算の間に型判定を挟まない。入力変数xの数値変換は従来どおり残る。[従来IR](<../build/math-box-ab/ir-off.ll>)と[変更後IR](<../build/math-box-ab/ir-on.ll>)を保存した。

[数値回帰テスト](<../tests/unit/test_nan_codegen.cpp>)に`abs`／`floor`／`ceiling`／`sqrt`の20式を追加した。動的入力、直接保存、直後の加算・乗算、除算で作る非有限値、domain-error NaN、負の小数からの符号付きゼロを確認する。期待値は既存のcastと標準数学関数から求め、インタプリタ・JIT・別RuntimeへロードしたAOTと比較する。

[リスト添字テスト](<../tests/unit/test_list_codegen_bounds.cpp>)にも`floor`／`ceiling`／`sqrt`／`sqrt+1`の4経路を追加した。NaN・Infinity・小数・文字列・特殊添字を入力し、readとreplaceの範囲判定、NaNを次の算術で0にする規則、同じリストへ解決される異なるIDを検証する。

### GPU利用時のCPU予算テスト

GPUアクセスが可能になったことで、既存のCPU時間予算テストがGPU計算へ移り、CPUのyieldチェックポイントを通らず全20,000反復を完了することが分かった。数学boxingを従来へ戻しても同じ1件が失敗し、GPU計算を無効にするとunitテストは成功した。GPUテスト自体はGPU計算を必要とするため、全CTestを一律にGPU無効で回す方法は適切ではなかった。

[CPU予算テスト](<../tests/unit/test_danger.cpp>)の実行中だけ`XYO_GPU=0`を設定し、終了時に元の設定へ戻すよう独立化した。通常再生やGPUテストの設定は変更していない。これはCPUチェックポイントのテスト範囲を明確にした変更であり、既存GPU計算経路の時間予算・preemptionを修正したわけではない。

最終ビルド後の[CTest](<../build/math-box-ab/ctest-final.log>)は6/6成功した。さらに固定した実行ファイルで、JIT・インタプリタ・従来boxingの各unit suiteが成功した。数値回帰は各100,892チェック、リスト添字は各3,077チェック、失敗0だった。数値回帰の件数は既存の算術ケースも含むsuite全体の値である。

Scratch executeフィクスチャもJIT・インタプリタ・従来boxingの各設定で116/116成功した（assertは336／339／336、失敗0）。[再実行スクリプト](<../build/math-box-ab/run_regressions.py>)と同じディレクトリに全ログ・固定実行ファイルのハッシュを保存した。unit内の条件付きスケジューラdispatch検査2件は、実行条件が揃わずskipされた。

## 画像比較の方法

先行A/Bの未加工画像では、FPSの数字だけでなく、小数桁数によってFPSモニタの横幅も変わった。従来の数字部分だけのマスクはこの差を拒否した。これを避けるために比較範囲を広げるのではなく、[ベンチ](<../tests/bench/pen_bench.cpp>)に計測後の表示固定を追加した。

`XYO_BENCH_SHOT_MONITOR=@fps`は、時間と描画統計を確定した後にFPSモニタの表示レコードだけを0へ変更し、描画を1回行ってから画像を保存する。Scratch変数、ペンバッファ、実行済みの計算結果は変更せず、Runtimeのstepも追加しない。追加描画や画像保存の時間・統計はA/Bの計測区間外である。プロセス全体のプロファイラには追加の1描画が含まれるため、計測区間の`PROJECT`統計とは区別する。

[交互A/Bスクリプト](<../tests/bench/list_intern_ab.py>)の`--normalize-fps`を指定すると、この表示固定を使い、画像の比較マスクを無効にする。表示固定後の画像は1画素でも異なれば失敗する。画像比較は各解像度の最初の組の最終フレームに対して行い、全フレームの一致を確認したという意味ではない。

## 最終GPU A/B

NVIDIA RTX 3070、SDL offscreen／OpenGL、ステージ480×360・表示scale=2、Res=1、seed=1。主スレッドをウォームアップ後にCPU 12へ固定し、ロード・JIT・60 activeフレームのウォームアップを除いた90描画フレームを測った。sleep、詳細プロファイル、サンプリング、ストロークログは無効。同一の固定実行ファイルで従来boxingと変更版を交互に各5回比較した。初期リスト文字列の共有など、他の最適化は両群で有効のままである。

| Res=1・90描画フレーム、各5回の中央値 | 従来boxing | inline boxing |
| --- | ---: | ---: |
| 描画込みwall time | 2,158.023 ms | 2,074.387 ms |
| 全プロセスCPU時間 | 2,114.040 ms | 2,068.769 ms |
| 描画込みスループット | 41.705 FPS | 43.386 FPS |
| wall timeの範囲 | 2,118.817〜2,214.688 ms | 2,068.785〜2,120.560 ms |

wall timeは約**3.88%短縮**、スループットは約4.03%増加した。CPU時間は約2.14%短く、5組全てで変更版のwall timeが短かった。範囲はわずかに重なっており、全ての環境・解像度で同じ改善を保証するものではない。前回の共有化の改善率と単純に足し合わせない。

全試行で15,552,000ストローク、quads同数、draw=90、flush=180、CPU readback/upload=0を照合した。並列スケジューラのdispatchも両群で0だった。Res=4は画像確認の各1回で、972,000ストローク、draw/flush=90、readback/upload=0。時間は324.869→329.102 msと逆転しており、この単発値を性能改善の根拠にしない。

計測後にFPS表示だけを固定した最終画像は、Res=1／Res=4とも全画素一致した。PPMファイル全体のSHA-256も両群で一致した。

| 確認画像 | 両群共通のSHA-256 |
| --- | --- |
| Res=1 | `b960d67d2a01653f4238df2b9d7bc2624ebf1f66f2e71f220f801fa24f03ca09` |
| Res=4 | `e251440046c2ad7e195ebd6a2271508124f0c721bd1b801f326f274ed80f99ff` |

実行ファイルのSHA-256は`30960bc574f7f1db096246b421e4a281975a5b45391711687ce595486aca6003`。各試行の前後に照合した。[manifest](<../build/math-box-normalized-gl-ab/manifest.json>)、[全試行](<../build/math-box-normalized-gl-ab/results.json>)、[集計と画像比較結果](<../build/math-box-normalized-gl-ab/summary.json>)を保存した。

先行計測も交互5組だったが、FPSモニタの幅の違いで画像検査が停止した。そのログ・画像は`build/math-box-final-gl-ab/`に保存し、最終表には表示固定を追加した後の別の5組だけを使った。異なるバイナリや測定時刻の結果を混ぜて中央値を求めていない。

## 再現

```sh
cmake --build build -j 2
ctest --test-dir build --output-on-failure
SDL_RENDER_DRIVER=opengl \
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
python3 tests/bench/list_intern_ab.py --comparison math-box \
  --normalize-fps --backend gl --cpu 12 --output build/math-box-recheck

# 従来boxingを個別に確認。既に生成したAOTには反映されない。
XYO_JIT_MATH_BOX_GENERIC=1 build/tests/xyo-tests
```

出力先には未作成のディレクトリを指定する。スクリプトが実行ファイルを固定するが、測定中に別のビルドやテストを走らせない。
