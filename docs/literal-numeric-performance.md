# 定数の数値変換による3D2の追加高速化

## 変更内容

[コード生成](<../src/jit/codegen.cpp>)で、文字列として保持された定数の数値変換をコンパイル時に行うようにした。例えばSB3の数値ブロックに入っている`"1"`は、ローダ上では文字列Valueになる。従来は演算に使うたびにSStrの数値キャッシュを読み、状態を判定していた。

`constValue`が明示的な文字列定数を出力したとき、そのLLVM値と`cast::toNumber`の結果を対応づける。後で同じ値を`emitToNumF64`へ渡した場合だけ、変換済みのdouble定数を使う。生成されたホットループには、この最適化の有効・無効を調べる分岐を追加していない。

元のboxed文字列は変更しない。例えば`"003"`を変数やリストへ入れるときは文字列のままで、数値演算の入力として使うときだけ3になる。変数、リスト要素、実行時の手続き引数、live評価される変数shadowを定数扱いする変更ではない。

## 互換性の条件

変換は独自実装せず、既存の`cast::toNumber`を使う。NaNから+0への変換、負のゼロ、Infinity、桁あふれ・アンダーフロー、Unicode空白、16進・2進・8進文字列も同じ規則に従う。演算の順序やreporterの評価順は変えていない。

対応表の値はdoubleだけで、実行時にGCオブジェクトを参照する表は追加していない。JITの文字列pinとAOTの文字列slotは従来どおり使う。AOTでも、数値として使う箇所にはdouble定数を出力し、文字列として使う箇所には元のslotを残す。

`XYO_JIT_LITERAL_NUM_GENERIC=1`を指定すると、この変換をコンパイル時に省略せず、従来の実行時変換へ戻せる。これはコード生成時の切替なので、既に生成したAOTオブジェクトへ後から指定しても内容は変わらない。

## 最終計測

3D2のSB3は変更していない。SDL offscreen/OpenGL、ステージ480×360、表示scale=2、Res=1、seed=1。開始クリックと60 activeフレームのウォームアップをそろえ、sleepなしで30描画フレームを実行した。

同じ最終バイナリで定数変換を有効・無効交互に5回ずつ測った。他の試行用の最適化は撤去済みで、詳細プロファイル・ストロークログ・トレースは無効である。

| Res=1・30描画フレーム、各5回の中央値 | 従来の変換 | 定数変換 |
| --- | ---: | ---: |
| 描画込みwall time | 786.918 ms | 768.923 ms |
| 全プロセスCPU時間 | 784.803 ms | 766.777 ms |
| 1フレームあたり | 26.231 ms | 25.631 ms |
| 描画込みスループット | 38.123 FPS | 39.016 FPS |
| wall timeの範囲 | 782.820〜805.682 ms | 764.030〜776.040 ms |

wall timeは約2.29%短縮、スループットは約2.34%増加した。最終計測では5組全てで定数変換の方が速かった。先行する別の5組の比較でも中央値786.163→770.960 msだったが、1組では逆転した。常に同じ改善率になるという保証ではない。

全試行で5,184,000ストローク、quads同数、draw=30、GPU flush=60、CPU readback/upload=0を照合した。並列dispatchは両版とも0であり、今回の差は描画を並列化した結果ではない。既定のFPS設定や画質も変えていない。

Res=4は描画一致の確認用に各1回実行し、81.896→79.919 msだった。この単発値は性能結論には使わない。両版とも324,000ストローク、draw=30、flush=30、readback/upload=0だった。

## 描画と回帰テスト

最終画像の差はFPS表示内だけだった。Res=1では854画素（x=631..718、y=54..69）、Res=4では820画素（x=630..730、y=54..69）が異なり、それ以外はbyte一致した。

[数値変換の回帰テスト](<../tests/unit/test_nan_codegen.cpp>)では、296通りの式を32組の入力で実行する。インタプリタ、JIT、AOTを独立したcastベースの参照結果と比較し、符号付きゼロを含む非NaN値をビット単位で検査する。インタプリタのNaNの符号だけは正規化して比較し、コンパイルされた算術のNaNはcanonicalなビット列であることも確認する。

AOTテストは生成元のRuntimeを破棄し、別のRuntimeへオブジェクトをロードする。JITへフォールバックしていないことも検査する。数値に使える文字列`"003"`を変数・別リストへそのまま保存し、型と綴りが残ることも確認した。

[リスト添字の回帰テスト](<../tests/unit/test_list_codegen_bounds.cpp>)には、添字を算術で生成するケースを追加した。文字列の数値化、正の小数添字、Infinity×0で生じるNaNでも、有効範囲の判定とread/replaceの結果が変わらないことを確認する。

検証結果は次のとおり。

- CTest 6/6成功。
- 通常設定と`XYO_JIT_LITERAL_NUM_GENERIC=1`のunitテストで全suite成功。
- 数値変換95,032チェック、リスト添字1,321チェックで失敗0。
- Scratch executeフィクスチャはJIT・インタプリタとも116/116成功（assert JIT 336、インタプリタ339、失敗0）。

## コンパイル時間の注意点

現在は、数値として使われない文字列定数も初回コンパイル時に数値パースする。100万文字の文字列を変数へ保存するだけの合成プロジェクトで、各3回の確認を行った。数字だけの文字列ではコンパイル時間の中央値が2.8→3.6 ms、非数値の文字列では2.8→2.9 msだった。表示は0.1 ms単位で、これを一般的なコンパイル時間の保証にはしない。

この初回コストと、ループ内で繰り返す変換の削減は区別する。今回の描画時間にはロード・JIT・ウォームアップを含めていない。

## 採用しなかった追加試行

| 試行、各5回の中央値 | 対照 | 試行版 | 判断 |
| --- | ---: | ---: | --- |
| 無効なプロファイラの初期化確認を削減 | 786.885 ms | 789.681 ms | 改善なし。撤去 |
| 有限の定数から証明できるNaN検査だけを省略 | 792.614 ms | 795.449 ms | 改善なし。撤去 |
| 既知numberのタグ判定を再利用 | 768.283 ms | 773.749 ms | 改善なし。撤去 |

測定時刻が異なるため、表の異なる行同士や[前段の色キャッシュ計測](<pen-cache-followup.md>)との絶対値を直接比較しない。不採用の切替オプションや実装はソースに残していない。

## 再現

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure

export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=dummy
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export XYO_SEED=1
export XYO_BENCH_WARMUP_FRAMES=60
unset XYO_PROF XYO_PENPROF XYO_PENLOG XYO_PENTRACE
unset XYO_JIT_EQ_GENERIC XYO_JIT_MOTION_BOXED XYO_JIT_SLOT_RELOAD
unset XYO_JIT_LITERAL_NUM_GENERIC
build/tests/xyo-pen-bench --project 3D2.sb3 --frames 30
XYO_JIT_LITERAL_NUM_GENERIC=1 build/tests/xyo-pen-bench --project 3D2.sb3 --frames 30

# セッション内の最終計測を再実行。XYO_*を整理し、交互5組とRes=4を実行する。
python3 build/literal-num-ab/final_bench.py
```

ローカルの最終ログ・JSON集計・画像は`build/literal-num-ab/`にある。
