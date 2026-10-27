# Tri fill engineのplay経由・高stamps時の分割実行

## 再現

[ヘッドレスVulkanの修正](<tri-fill-throughput-regression.md>)とは別に、`play`のGL経路でも100万stampsが容量制限に達することを確認した。初期stampsを100万にした実行は、GPU実行に失敗してCPUへ戻り、確認した最終表示は904,159tpsだった。[修正前の診断](<../build/tri-fill-play/before.stderr>)を保存した。これは短い診断実行の値で、交互A/Bの性能値とは区別する。

GL描画ではレコードの内容を実際にペン面へ描くため、ヘッドレスのように省略できない。100万lane・1laneあたり32レコードでは、レコードだけで1.536GB、描画順序IDも含めると1.792GBになる。従来は要求全体を1回で処理し、512MiBを超えると全体をCPUへ戻していた。

## 分割実行

[容量プランナ](<../src/gpu/gpu_analysis.cpp>)と[GL executor](<../src/render/sdl_renderer.cpp>)を変更し、単一dispatchに収まらない要求を昇順のチャンクへ分割する。収まる要求は従来どおり単一dispatchで処理する。

GPU一時バッファとトランザクション用ペン面は、分割時に256MiB以内へ収める。SSBO、workgroup、描画頂点数の上限もチャンク単位で検査する。返却する全lane出力は従来のAPIを保つが、別にホスト512MiB上限を設け、巨大なwriteback領域の確保を防ぐ。これらはGLドライバや既存レンダラ全体の消費メモリ上限を意味するものではない。

チャンク内の保存添字はローカルlane、論理反復番号は`globalBase + localLane`に分けた。乱数seed、`OP_ITER`、induction、全体のlane 0だけが引き継ぐ初期ペン状態は、元のグローバル反復番号を使う。各チャンクのレコードをlane順に描き、チャンク自体も昇順に処理するため、描画順序は変えない。writebackは元の全lane位置へ配置する。

## 途中失敗を公開しない描画

前のチャンクを公開ペン面へ描いてから次のチャンクが失敗すると、CPUでループ全体をやり直した際に先頭部分が二重描画になる。この方法は使わない。

分割時はRGBA8の一時FBOへ既存ペン面をblitし、その面に全チャンクを描く。すべての容量・件数・命令ガードとGL処理が成功してから、1回のblitで公開する。途中で失敗した場合は一時面と未公開出力を捨て、元のペン面とRuntimeの状態からCPUへ戻れる。既存のSDL共有テクスチャ自体を交換しないため、interopの参照も保つ。

SSBOを次のチャンクで再利用する前には、先の描画がそのレコードを読み終えるようmemory barrierを置く。以前の大きい容量キャッシュを分割時に縮小し、要求サイズだけを小さく見積もって古い大容量確保を残さない。

この非破壊性は通常のガード・容量・GL処理の失敗を対象とする。GPUコンテキストやデバイス自体を失った場合まで、元のGPU状態を回復できると主張するものではない。

## 検証の方針

[CPU-onlyプランナテスト](<../tests/unit/test_gl_chunk_plan.cpp>)では、100万laneの要求、ホスト出力上限、最大属性数、SSBO・workgroup・整数境界、強制分割と末尾チャンクを検査する。

[実GLテスト](<../tests/gpu/test_parallel.cpp>)では、小さい131laneを単一dispatch、7lane分割、65lane分割で比較する。全lane出力のビット列、描画RGBA、件数、明示的な反復番号、複数回の乱数、lane 0と後続laneの異なる初期状態を照合する。実際に分割された回数も内部診断値で確認する。

さらに、2番目のチャンクだけがレコード上限または命令ガードで失敗する入力を使う。公開ペン面、位置、ペン状態、変数、ホストRNGが不変であることを検査する。失敗時にdirtyフラグが立たないことを利用してCPUキャッシュが誤ったGPU描画を隠さないよう、テストでは実FBOの読み戻しを強制する。成功時は、チャンク数にかかわらずホストRNGを1dispatch分だけ進めることも確認する。

`XYO_GPU_GL_CHUNK_LANES=N`は検証用に最大チャンク長を小さくする。`XYO_GPU_GL_BATCH=0`は分割を無効にし、従来の容量拒否を対照として再現する。どちらも通常は未設定にする。

## playの同一負荷計測

[TFG用スクリプト](<../tests/bench/tri_fill_regression.py>)へ`--mode play`を追加した。100万stampsの完全なバッチを指定数だけ実行し、完了カウンタを照合する。任意の描画フレーム数でwarpバッチを途中停止して比較する方法は使わない。

`XYO_EXIT_ON_STOP=1`は計測用アーカイブのstop-allを受けてplayを終了する明示的な検証設定である。通常のplayの終了動作は変更しない。終了時に元のstopコールバックを戻し、GUIからRuntimeを再利用する場合の参照寿命も保つ。

## 実GLの交互A/B

RTX 3070、SDL offscreen／OpenGL、seed=1、`--fps 0`でsleepを入れず実行した。1バッチ100万stampsを3バッチ、合計300万stampsを全試行で完了した。従来の容量拒否（`XYO_GPU_GL_BATCH=0`）と既定の分割を、同じ固定バイナリで交互に各5回比較した。ロード・JIT・ウィンドウ初期化を含む全プロセス時間と、最後のバッチのプロジェクト表示を区別して記録した。

| 各5回の中央値 | 従来の容量拒否→CPU | 分割GPU |
| --- | ---: | ---: |
| 最終バッチのThroughput表示 | 914,077 tps | 1,733,102 tps |
| 表示tpsの範囲 | 892,857〜925,069 | 1,727,115〜1,739,130 |
| 全プロセスwall time | 3,608.335 ms | 2,007.021 ms |
| 全プロセスwall timeの範囲 | 3,568.299〜3,621.376 ms | 1,991.083〜2,016.454 ms |

Throughput表示は約1.90倍、全プロセス時間は約44.38%短縮し、5組すべてで分割GPUが速かった。分割の診断では100万laneを143,638laneずつ、計7チャンクで処理し、全3バッチのGPU実行が成功した。CPUフォールバックは0だった。一時GPU領域の計画値は268,435,148 bytes、全laneのホスト出力は64,000,000 bytesだった。

[全試行](<../build/tri-fill-play-final-ab/results.json>)、[集計](<../build/tri-fill-play-final-ab/summary.json>)、[固定バイナリと入力のハッシュ](<../build/tri-fill-play-final-ab/manifest.json>)、[分割診断](<../build/tri-fill-play-final-ab/chunked-diagnostic.stderr>)を保存した。実行ファイルSHA-256は`38e4bafc861a7b07a1448d2602df834c33de3b8b7ac040f130957e289dfc7796`。

重要な制約として、**今回のplay実測は1,000万tpsに戻っていない**。ヘッドレスの約1,560万tpsは画面を描かない別条件であり、playの値として扱えない。今回は容量制限によるCPUへの全体フォールバックを解消し、同じ描画負荷での改善を確認した。残る描画・レコード処理の内訳を計測せず、特定の処理がボトルネックだとは断定しない。

## 回帰とAOT

- [実GLテスト](<../build/tri-fill-play/gl-live-final.log>)は908チェック・失敗0。単一／7lane／65lane分割の全出力・画像・RNG一致、後半チャンクの失敗での実FBO不変を含む。初回は新しい件数oracleを`3N+1`として3件失敗したが、GLの既存`PENDOWN`はup→down遷移だけで打刻するため正しい件数は`3N`だった。oracleを直し、製品側の打刻規則は変えていない。
- [CTest](<../build/tri-fill-play/ctest.log>)は6/6成功。
- Vulkanの既定70チェック・従来記録経路67チェックは失敗0。共有VMへ追加したグローバル基点はVulkanでは0に初期化する。
- インタプリタのunit suiteも成功。executeはJIT・インタプリタとも116/116成功（assert 336／339、失敗0）。
- 3D2も同じ345,600打点・2描画フレームで確認した。計測後にFPSモニタの表示だけを固定したPPMは、変更前の固定バイナリと全byte一致した。共通SHA-256は`b960d67d2a01653f4238df2b9d7bc2624ebf1f66f2e71f220f801fa24f03ca09`。これは描画確認であり、2フレームから速度の結論は出さない。

書き出し直したAOTアプリのウィンドウ付き再生でも、100万stamps×3バッチすべてがGPUで成功した。最終表示は1,779,359tpsだったが、診断付きの各1回なので上の中央値には含めない。[AOTのGPUログ](<../build/tri-fill-play/aot-play.stderr>)を保存した。

TFGの大負荷で従来はCPUへ戻っていた場合、既存GPU VMのf32・lane別RNGで動く経路へ変わる。CPUとGPUのランダムな最終画像をビット一致と主張するものではない。分割そのものの互換性は、同一のGPUプログラム・seedで単一dispatchと分割の全lane出力・RGBAを比較して確認した。

## 使用と再現

通常のplayでは追加設定は不要。[build/xyo](<../build/xyo>)は修正済み。既存のAOTアプリには古いランタイムが埋め込まれるため、書き出し直す。

```sh
cmake --build build -j 2
XYO_SET='@stamps=1000000' build/xyo play 'The fastest tri fill engine.sb3'

SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl SDL_AUDIODRIVER=dummy \
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
XYO_TEST_GPU=1 build/tests/xyo-gpu-tests --parallel

__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json \
python3 tests/bench/tri_fill_regression.py --mode play \
  --frames 3 --trials 5 --output build/tri-fill-play-recheck
```

出力先には未作成のディレクトリを指定する。診断は時間計測と分離し、計測中にビルドや別のテストを実行しない。
