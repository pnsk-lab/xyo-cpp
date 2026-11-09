# ASTERIA プレイ中の描画負荷調査

対象: debug/projects/【3D】ASTERIA(Ver.1.1).sb3（約327 MiB、834 SVG、357 PNG、130 音声）。

## 計測用ビルドと実行

XYO_PLAY_PROFILE を指定しなければフレーム統計取得は無効になる。

~~~sh
cmake --build build --target xyo-cli -j4
mkdir -p bench-output

XYO_PLAY_PROFILE=bench-output/asteria-play.csv \
  build/xyo play 'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' \
  --fps 30 2> bench-output/asteria-play.stderr.log
~~~

自分で操作し、重い場面を含めてプレイしてから通常通り終了する。フレームごとの CSV は30フレームごとに書き出され、終了時にも flush される。さらに終了時には bench-output/asteria-play.csv.sprites.csv に、クローンを元のスプライト名で合算した描画時間と描画回数を保存する。ログを集計するコマンド:

~~~sh
python3 tools/analyze_play_profile.py \
  bench-output/asteria-play.csv \
  --json bench-output/asteria-play-summary.json
~~~

## CSV 各列

- frame_interval_ms: フレーム開始間隔。FPS 制限の sleep やイベント処理を含む。
- step_ms: Runtime::step 全体。SDL イベントポーリングや sleep は除外。
- vm_ms: step_ms - draw_ms。JIT・Scratch ロジック・当たり判定など。
- draw_ms: renderer.draw() 全体。スプライト、ペン、画面更新をすべて包含。
- sprite_ms / backdrop_ms: スプライト・背景の描画時間。画像変換・効果生成を包含。
- costume_decode_ms, costume_decodes: 未キャッシュの SVG/PNG コスチュームのラスタライズと SDL テクスチャ生成。
- effect_build_ms, effect_builds, effect_hits: 画像エフェクトのキャッシュ再生成時間・件数。
- texture_uploads: コスチューム・エフェクトの SDL テクスチャアップロード回数（ペンの更新は含めない）。
- sprite_copies / backdrop_copies: SDL_RenderCopyEx 呼び出し回数。
- collision_ms, collision_calls: touching / color touching の処理時間・回数（VM 内）。
- pen_ms: ペン層の描画・転送。
- present_ms: SDL_RenderPresent。vsync / GPU キュー待ちを含み、純粋な GPU 処理時間ではない。
- monitor_ms / ui_ms: 変数・リスト監視、吹き出し、画面 UI。
- rss_kib: 現在のプロセス RSS（Linux の /proc/self/statm）。
- threads: 生存中の Scratch スレッド数。

タイマーは階層的で重複する。draw_ms は sprite/pen/present を含み、sprite_ms は costume_decode/effect_build を含み、vm_ms は collision_ms を含むため加算しない。

## 読み方

draw_ms が高ければ sprite_ms、present_ms、costume_decode_ms、effect_build_ms を優先する。draw_ms が低く vm_ms が高ければ JIT、Scratch ロジック、collision_ms を見る。collision_ms が大きければ見た目がカクついていても原因は画面描画ではなく CPU 色判定かもしれない。

プレイ中の計測は時計・メモリ取得のオーバーヘッドを含む。初期 SB3 読込時間は CSV に含まれない。JIT の個別コンパイル時間や GPU 専用タイマーも現状は記録しない。
