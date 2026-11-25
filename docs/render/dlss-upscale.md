# DLSS 相当のアップスケーリング（レンダラ）

ステージの中身（背景・ペン・スプライト）を**低い内部解像度**でオフスクリーンに描き、
表示解像度へ高次フィルタで再構成する経路。NVIDIA DLSS と同じ「低解像度レンダリング +
高品質再構成」という構造を、外部 SDK なしの GL 3.3 シェーダで実装している。

## 既定は無効（1.0 = 従来と byte 一致）

`renderScale_` の既定は 1.0 で、その場合は `draw()` が従来どおり
`drawStageContent()` を直接呼ぶ。オフスクリーン target も確保しない。
1.0 → 0.5 → 1.0 と切り替えて戻したフレームが、一度も切り替えていない
フレームと**バイト一致**することを `tests/render/test_renderer.cpp` の
`test_render_scale()` が検証する。

## 構成

- `src/render/gl_upscale.{hpp,cpp}` の `GlUpscale`
  - 4×4 Catmull-Rom（bicubic）で再構成。バイリニアより線のエッジが締まる。
  - 3×3 近傍のバイリニア結果との差分を unsharp mask として加算（`uSharpness`）。
  - 時間方向: 前フレームの再構成結果を再利用し、現在フレームの
    4×4 近傍 min/max に clamp してから混ぜる（variance clipping）。
    動くスプライトは近傍から外れた履歴を捨てるので尾を引かない。
  - ping-pong の 2 枚の target で履歴を受け渡す。GL の state
    （program / VAO / FBO / viewport / draw buffer / blend / scissor /
    texture unit と bind）は呼出し前後で復元する（`gl_effects.cpp` と同じ方針）。
- `SdlRenderer`（`src/render/sdl_renderer.cpp`）
  - `draw()` は stage 矩形と `renderScale_` を見て `drawStageContent()` か
    `drawScaledStage()` を選ぶ。
  - `drawScaledStage()`: ステージサイズ × 出力ピクセル比（HiDPI）× scale で
    target を作り、`stageRect_` を target 座標へ差し替えて `drawStageContent()`
    を実行、戻してから `GlUpscale::apply()` の結果を表示矩形へ転送する。
  - 吹き出し・ask バー・モニタ・green flag/stop・ラベルは**表示解像度のまま**
    （UI とテキストをぼかさない）。
  - ペン層の解像度（`desiredPenSize()`）も scale に追従する
    （TurboWarp の High Quality Pen と組み合わさる場合は実表示密度 × scale）。

## 有効化

| 指定 | 意味 |
| - | - |
| `--render-scale N` | `xyo play` の CLI。env より優先 |
| `XYO_RENDER_SCALE=N` | 0.25〜1.0（範囲外は clamp）。既定 1.0 |
| `XYO_DLSS_SHARP=N` | unsharp mask の強さ（既定 0.35） |
| `XYO_DLSS_TEMPORAL=0` | 時間方向の履歴を切る（毎フレーム空間再構成のみ） |
| `XYO_DLSS_CPU=1` | 強制的に従来経路（A/B 用） |
| `XYO_DLSS_DEBUG=1` | シェーダのコンパイル/リンク失敗、FBO 状態、数フレームごとの解像度を stderr へ |

## フォールバック

- ソフトウェア SDL レンダラ（`SDL_VIDEODRIVER=dummy` 等）と Emscripten は
  この経路を使わない。フルウィンドウの追加 blit が直接描画より高くつくことを
  実測で確認したため、`renderScale_ < 1` でも従来の直接描画へ戻す。
- GL 3.3 未満、シェーダのリンク失敗、target 確保失敗、`SDL_SetRenderTarget`
  失敗はすべて従来経路へフォールバックする（落ちない）。

## 画質の実測

`examples/play_demo.sb3` を x11 / RTX 3070（OpenGL 4.6）で 45 フレーム
実行し、1.0 のフレームと比較（`xyo-pen-bench --compare`）:

| scale | temporal | SSIM | meanErr | maxErr |
| - | - | - | - | - |
| 0.75 | on | 0.9975 | 0.179 | 149 |
| 0.75 | off | 0.9973 | 0.192 | 151 |
| 0.60 | on | 0.9924 | 0.307 | 229 |
| 0.50 | off | 0.9947 | 0.257 | 200 |

maxErr が大きいのは 1px 幅のペン線とテキスト境界（フィルタで再配分される画素）。
UI・モニタ・レイアウトに崩れは無い。

## 性能の実測（この環境での限界）

- `debug/projects/3D2.sb3` 90 フレーム: draw 1.61ms / step 216.8ms（scale 1.0）と
  draw 1.81ms / step 219.4ms（scale 0.5）。**描画は元々律速ではない**ため差が出ない。
- 80 スプライトの全画面オーバードロー（`--scale 4`、1920×1440）でも、ウォームアップ後は
  draw 0.41ms（1.0）/ 0.46ms（0.5）で GPU は埋まっていない。
- ソフトウェアレンダラでは scale 0.5 が 249ms → 336ms と**悪化**する（追加 blit のため）。
  これが上記のフォールバック条件。

つまりこのマシン（i7-12700 + RTX 3070）では scratch-bench のスコアにも
ステージ描画時間にも影響しない。効くのは「フィルバウンド / HiDPI /
弱い GPU / 大きな `_twconfig_` ステージ」のケース。

## 注意

- scratch-bench は CPU ブロック実行のベンチなので、この機能はスコアに影響しない。
- 時間方向の履歴はリサイズ・プロジェクト再読込・scale 変更で `reset()` する。
- 履歴を強くすると静止画のちらつきは減るが、ペンで毎フレーム全画面を描き替える
  作品では尾を引くことがある。その場合は `XYO_DLSS_TEMPORAL=0`。
