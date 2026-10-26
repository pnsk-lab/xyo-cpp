// xyo-cpp: SDL2 ベースの表示レンダラ（xyo play 用）。
// ステージ描画・スプライト・ペン・モニタ・バブルを SDL_Renderer で合成する。
// SDL_ttf/SDL_image 非依存 — PNG:stb_image / SVG:nanosvg / テキスト:stb_truetype。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../engine/runtime.hpp"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
union SDL_Event;

namespace xyo {

/// ピクセル矩形（SDL_Rect 相当。ヘッダで SDL 型を露出させないため独立型）
struct IRect { int x = 0, y = 0, w = 0, h = 0; };

/// ペン層の累積統計（XYO_PENPROF と同値。ベンチ・適応制御用）。
struct PenStats {
    uint64_t strokes = 0;    // penStroke 呼び出し
    uint64_t rasterPx = 0;   // CPU ラスタが触れた画素
    uint64_t quads = 0;      // GPU バッチへ積んだストローク
    uint64_t flushes = 0;    // FBO への描画回数
    uint64_t readbacks = 0;  // FBO → CPU 転送回数
    uint64_t convPx = 0;     // premult→straight 変換画素
    uint64_t uploads = 0;    // SDL_UpdateTexture 回数
    uint64_t frames = 0;     // draw() 回数
    uint64_t records = 0;    // GPU レコード描画数
    uint64_t rcFlushes = 0;  // 逆順合成でフラッシュした回数
    uint64_t fwdFlushes = 0; // 前向き合成でフラッシュした回数
    double strokeMs = 0, rasterMs = 0, quadMs = 0, flushMs = 0,
           readbackMs = 0, convertMs = 0, uploadMs = 0, copyMs = 0,
           drawMs = 0, presentMs = 0;
};

/// プロセス全体のペン層統計を返す（カウンタはリセットしない）。
PenStats penStats();
/// 統計をリセットする（ベンチの区間計測用）。
void penStatsReset();

class SdlRenderer : public RendererAPI {
public:
    SdlRenderer() = default;
    ~SdlRenderer() override;

    SdlRenderer(const SdlRenderer&) = delete;
    SdlRenderer& operator=(const SdlRenderer&) = delete;

    /// ウィンドウ+レンダラ生成 + runtime.targets の drawable 化。
    /// scale=1 で 480x360。成功時 true。
    bool init(Runtime& rt, int scale = 2);
    /// 埋め込みモード（エディタ）: 既存の SDL_Renderer を共有して
    /// ステージをその中の 1 領域へ描く。ウィンドウは作らない。
    /// renderer の寿命は呼び出し側が管理する（このオブジェクトは破棄しない）。
    bool initEmbedded(Runtime& rt, SDL_Renderer* renderer);
    /// 埋め込み時のステージ表示矩形（renderer のクライアント座標）を設定。
    /// 描画・pick・マウス座標変換の基準になる。
    void setStageRect(const IRect& r);
    /// 現在のステージ矩形（renderer クライアント座標）。
    IRect stageRect() const { return stageRect_; }
    /// 埋め込みモードか（自前ウィンドウなし）。
    bool embedded() const { return embedded_; }
    bool ok() const { return renderer_ != nullptr; }
    SDL_Window* window() const { return window_; }
    SDL_Renderer* sdlRenderer() const { return renderer_; }
    /// ask and wait の入力 UI がアクティブか（イベント転送判定用）。
    bool askActive() const { return askActive_; }
    /// スプライトの現在コスチュームの描画テクスチャ（UI サムネイル用）。
    /// 見つからなければ nullptr。論理サイズ・回転中心も返す。
    SDL_Texture* spriteTexture(Target* t, double* lw, double* lh,
                               double* rcx, double* rcy);
    /// コスチュームの描画テクスチャ（Costumes タブ用）。装飾なしの素の見た目。
    SDL_Texture* costumeTexture(const Costume* c, double* lw, double* lh);

    /// SDL イベントを ioDevices/UI に流す。終了要求で false。
    bool pollEvents();
    /// 1 イベント分の処理（pollEvents の本体。テスト注入にも使用）
    bool handleEvent(const SDL_Event& ev);
    /// 現在のレンダーターゲットを PPM として保存（検証用）。
    bool savePPM(const char* path);
    void setTitle(const char* title);
    void setFrameCapture(std::function<void()> capture) {
        frameCapture_ = std::move(capture);
    }

    // --- RendererAPI ---
    int pick(double x, double y) override;
    void draw() override;
    bool drawableTouching(int drawableID, double x, double y) override;
    bool isTouchingDrawables(int drawableID, const int* others,
                             size_t n) override;
    bool isTouchingColor(int drawableID, uint32_t rgb) override;
    bool colorTouchingColor(uint32_t color, uint32_t target,
                            int drawableID) override;
    void getDrawableBounds(int drawableID, double* b) const override;
    bool hasDrawable(int drawableID) const override;
    void getFencedPosition(int drawableID, double& x, double& y,
                           bool fencing) const override;
    void getCurrentSkinSize(int drawableID, double& w,
                            double& h) const override;
    double setDrawableOrder(int drawableID, double order, const char* group,
                            bool isRelative) override;
    void destroyDrawable(int drawableID) override;
    void emitVisualChange(Target* t) override;
    std::vector<int> drawableOrder() const override;
    int drawableOfTarget(Target* t) const override;
    int createPenSkin() override;
    void penLine(int skin, const float rgba[4], double diameter,
                 double x0, double y0, double x1, double y1) override;
    void penPoint(int skin, const float rgba[4], double diameter, double x,
                  double y) override;
    void penClear(int skin) override;
    void penStamp(int skin, int drawableID) override;
    bool gpuExecParallel(gpu::ExecRequest& req) override;
    bool gpuExecNative(gpu::NativeRequest& req) override;
    void getFenceRect(int drawableID, double out[4]) const override;

private:
    friend struct SdlRendererTestAccess;
    struct Skin {
        SDL_Texture* tex = nullptr;
        std::vector<uint8_t> px;   // RGBA8（タッチ判定・スタンプ用に保持）
        int w = 0, h = 0;
        double scale = 1;          // テクスチャ px / 論理 px
        double rcx = 0, rcy = 0;   // 回転中心（論理 px）
        double logicalW = 0, logicalH = 0;
        /// エフェクト適用バリアントのキャッシュ（effectKey → 変換済みバッファ）
        struct FxVariant {
            std::vector<uint8_t> px;
            void* tex = nullptr;   // SDL_Texture*
        };
        std::unordered_map<std::string, FxVariant> fx;
    };
    struct Drawable {
        int id = -1;
        Target* target = nullptr;
        std::string group = "sprite";
        double order = 0;
        /// getFencedPosition 用キャッシュ。4 隅の回転後オフセット
        /// (rx,ry) は skin・size・direction・rotation style にのみ依存
        /// するため、移動のたびに cos/sin と 4 隅変換をやり直さない
        /// （setXY は毎ストローク走る）。加算順序は元実装と同一に保ち
        /// 浮動小数の丸めまで一致させる。
        mutable const Costume* fenceCostume = nullptr;
        mutable const Skin* fenceSkin = nullptr;
        mutable double fenceSize = 0, fenceDir = 0;
        mutable double fenceW = 0, fenceH = 0;   // skin 寸法もキーに含める
        mutable std::string fenceRot;
        mutable double fenceRx[4] = {0, 0, 0, 0};
        mutable double fenceRy[4] = {0, 0, 0, 0};
        /// 4 隅の stage px オフセット min/max と inset（=min(15,幅/2)）。
        /// 前計算しておき、毎移動の min/max ループと floor を省く。
        /// ステージ寸法は実行時に変わり得るので保持しない（毎回
        /// stageW_/2 - inset で求める）。
        mutable double fenceMinRx = 0, fenceMaxRx = 0;
        mutable double fenceMinRy = 0, fenceMaxRy = 0;
        mutable double fenceInset = 0;
        mutable double fenceMaxAbsRx = 0, fenceMaxAbsRy = 0;
        mutable bool fenceValid = false;
    };
    /// 実際に描画・サンプリする画素列（エフェクト適用後）
    struct Rendered {
        SDL_Texture* tex = nullptr;
        const uint8_t* px = nullptr;
        int w = 0, h = 0;
    };

    /// getFencedPosition の 1 エントリメモ（同一 drawable が連続で動く）
    mutable int lastFenceId_ = -1;
    mutable const Drawable* lastFenceDrawable_ = nullptr;

    // --- drawable/skin ---
    /// init/initEmbedded 共通の後半（drawable・コールバック・カーソル・ask）
    void initTargetsAndUi();
    Drawable* drawableFor(int id);
    const Drawable* drawableFor(int id) const;
    void createDrawableForTarget(Target* t, const char* group);
    Skin* skinForTarget(Target* t);
    const Skin* skinForTarget(Target* t) const;
    Skin* decodeCostume(const Costume* c);
    static void freeSkin(Skin& s);
    /// ベーススキン + エフェクトの最終ビットマップを返す（base なら s を返却）
    const Rendered* renderedFor(Target* t, const Skin* s,
                                Rendered* out) const;
    // スプライトの変換パラメータ（stage 論理 px 座標系）
    struct DrawXform {
        double k;      // テクスチャ px → stage px
        double cx, cy; // アンカー（スプライト位置）の stage 座標 [0..w]
        double cos, sin;
        double dirScaleX; // left-right 反転
        double skinHalfW, skinHalfH;
    };
    bool xformFor(const Drawable& d, const Skin& s, DrawXform& out) const;
    // scratch 座標 → stage 座標 [0..stageW/H]（中心原点・y 上向き → 左上原点）
    double stageX(double sx) const { return sx + stageW_ / 2.0; }
    double stageY(double sy) const { return stageH_ / 2.0 - sy; }
    double scratchX(double px) const { return px - stageW_ / 2.0; }
    double scratchY(double py) const { return stageH_ / 2.0 - py; }
    /// ステージ論理 px の色を合成サンプル（タッチ判定用）。skipId 自身は除外。
    bool stageColorAt(double px, double py, int skipId, uint8_t out[4]) const;
    bool skinPxAt(const Drawable& d, double stagePx, double stagePy,
                  uint8_t out[4]) const;

    // --- 描画 ---
    void drawStageArea();
    void drawDrawable(Drawable& d);
    void drawPenLayer();
    void drawBubble(Target* t);
    void drawMonitors();
    void drawAskBar();
    void drawUiChrome();
    void recomputeViewport();
    SDL_Texture* textTexture(const std::u16string& s, int px, uint8_t r,
                             uint8_t g, uint8_t b, int* w, int* h,
                             bool bold = false);

    // --- ペン（CPU ビットマップ + テクスチャ反映）---
    void penEnsure();
    void penStroke(const float rgba[4], double diameter, double x0,
                   double y0, double x1, double y1);
    void penAppendRecord(const float rgba[4], float width, float x, float y,
                         float dx, float dy, float length);
    void penBlendPremult(uint8_t* p, const float rgba[4], double coverage);
    void penBlendPx(int x, int y, const uint8_t c[4]);
    double penScale() const { return penScale_; } // ペン px / 論理 px

    // --- 遅延バッチ + 逆順合成（CPU ラスタのオーバードロー対策）---
    // CPU ラスタでは即時合成の代わりにストロークを溜め、フラッシュ時に
    // 推定オーバードローから「前向き逐次合成」と「逆順合成 + 早期終了」を
    // 選ぶ。逆順合成は (T,P) 半群
    //     (T2,P2)∘(T1,P1) = (T2·T1, P2 + T2·P1)
    // を新しい順に適用し、透過率 T が閾値を下回った画素を以降スキップする。
    struct PenRec {
        float rgba[4];
        float width;
        float fx0, fy0, fdx, fdy;
        float L;   // float 精度の線分長（厳密経路と同一値）
    };
    void penStrokeRaster(const PenRec& r);   // 前向き逐次合成（厳密経路）
    void penFlushCpuBatch();                 // バッチを penPx_ へ反映
    void penRasterReverse();                 // 逆順合成 + 早期終了
    void penBatchPush(const PenRec& r);
    void penRcEnv();
    /// 逆順合成の透過率閾値（XYO_PEN_EPS、既定 1/512）
    float penRcEps() const { return penRcEps_; }
    std::vector<PenRec> penBatch_;
    double penBatchArea_ = 0;    // バッチの推定被覆面積（オーバードロー判定）
    uint32_t penImmediate_ = 0;  // 直近フラッシュ以降に即時ラスタした本数
    double penBatchBx0_ = 1e30, penBatchBy0_ = 1e30;
    double penBatchBx1_ = -1e30, penBatchBy1_ = -1e30;
    float penRcEps_ = 1.0f / 512.0f;
    int penRcMode_ = -1;         // -1=auto, 0=forward 固定, 1=RC 固定
    bool penRcInit_ = false;
    uint64_t penRcFlushes_ = 0, penFwdFlushes_ = 0;
    std::vector<float> penAcc_;      // 逆順合成の蓄積（premult rgb + T）
    std::vector<int32_t> penRowOpen_;  // 行ごとの T>=eps 画素数

    // --- UI ---
    bool hitChrome(int mx, int my, int& which) const; // 0=flag 1=stop
    void postMouse(int mx, int my, bool hasDown, bool down, int button,
                   bool wasDragged = false);
    /// スライダモニタの掴み判定。掴めたら true + 対象変数を解決。
    bool sliderHitTest(int mx, int my);
    /// スライダの現在位置から値を書き込む
    void sliderDragTo(int mx);
    /// ask 入力ボックス領域（ステージ下端バー）。未アクティブ時は false。
    bool askHitTest(int mx, int my) const;
    void askSubmit();
    /// ask 入力中のキー編集（TEXTINPUT 外の特殊キー）
    void askEditKey(int sym);
    /// マウス位置からシステムカーソルを更新（hand/ibeam/arrow）
    void updateCursor(int mx, int my);

    // --- ask and wait ---
    bool askActive_ = false;
    std::u16string askQuestion_;      // 空=スプライトバブル表示
    std::u16string askInput_;
    size_t askCaret_ = 0;             // UTF-16 単位のカーソル位置
    IRect askFieldRect_{};            // 入力フィールド（window px、描画時更新）
    IRect askSubmitRect_{};           // ✓送信ボタン（同上）

    // --- スプライトドラッグ ---
    Target* dragTarget_ = nullptr;
    double dragOffX_ = 0, dragOffY_ = 0;   // 掴み点→スプライト原点（scratch）
    bool dragMoved_ = false;

    // --- スライダドラッグ ---
    Variable* sliderVar_ = nullptr;
    double sliderMin_ = 0, sliderMax_ = 100;
    bool sliderDiscrete_ = true;
    int sliderX0_ = 0, sliderW_ = 0;   // window px 座標（トラック範囲）
    /// モニタ id → トラック矩形（毎フレーム drawMonitors が更新）
    std::unordered_map<std::string, IRect> sliderRects_;
    /// モニタ id → ノブ可動スパン [x,w]（値マッピング基準）
    std::unordered_map<std::string, std::pair<int,int>> sliderTravel_;

    // --- システムカーソル ---
    SDL_Cursor* curArrow_ = nullptr;
    SDL_Cursor* curHand_ = nullptr;
    SDL_Cursor* curIbeam_ = nullptr;
    SDL_Cursor* curActive_ = nullptr;   // 現在適用中（重複 SetCursor 回避）

    Runtime* rt_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool embedded_ = false;   // initEmbedded: window_/renderer_ を所有しない
    std::function<void()> frameCapture_;
    int stageW_ = 480, stageH_ = 360;
    int scale_ = 2;                  // 初期スケール
    IRect stageRect_;                // ウィンドウ内のステージ表示領域

    std::unordered_map<int, Drawable> drawables_;
    int nextDrawableId_ = 1;
    std::unordered_map<const Costume*, std::unique_ptr<Skin>> skins_;
    // テキストテクスチャキャッシュ: (文字列,px,rgb) → texture
    struct TexEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, TexEntry> textCache_;

    // ペン CPU バッファ（stageW_*penScale_ ピクセル）
    std::vector<uint8_t> penPx_;
    std::vector<uint8_t> penUploadPx_;
    int penW_ = 0, penH_ = 0;
    double penScale_ = 1.0;
    SDL_Texture* penTex_ = nullptr;
    bool penDirty_ = false;
    int penSkinId_ = -1;
    struct PenKernel {
        double width = -1;
        double fx = 0, fy = 0;
        int n = 0;
        int8_t ox[81], oy[81];
        float cov[81];
    };
    PenKernel penKernels_[8];
    int penKernelNext_ = 0;

    // --- GPU ペン層（OpenGL レンダラ時のみ有効）---
    // ストロークを頂点としてバッチ蓄積し、フラッシュ時に
    // FBO へ一括描画する。カバレッジ clamp(radius+0.5-dist) は
    // フラグメントシェーダで解析評価（SDL_Renderer では表現不可、
    // TurboWarp の pen シェーダと同じ構造）。penPx_ は読み出し経路
    // （touching 色判定・savePPM・テクスチャアップロード）のため
    // フラッシュ時に readback で同期する。
    struct GlPen;
    GlPen* glPen_ = nullptr;     // 実体は cpp のみ（不完全型のため raw 所有）
    void* sdlGlCtx_ = nullptr;   // SDL_GLContext
    bool initGlPen();
    bool penGpu() const;
    void penFlushGpu();          // バッチを FBO へ描画（readback 無し）
    void penSyncCpu();           // バッチ描画 + penPx_ へ readback
    void penStrokeGpu(const float rgba[4], float width,
                      float fx0, float fy0, float fdx, float fdy,
                      float L);

    // --- GPU 並列スクリプト実行（GL4.3 compute。src/gpu 参照）---
    // コンパイル済みミニ VM バイトコードを compute shader で
    // 1 反復=1 レーンとして解釈し、ペンストロークレコードを SSBO へ
    // 出力 → レコードを VS でクアッド展開して既存ペン FBO へ描画。
    struct GlCompute;
    GlCompute* glComp_ = nullptr;
    bool initGlCompute();
    std::vector<float> gpuLaneOut_;   // laneOut readback 用
    bool penUploadGpu();

    // UI 状態
    int flagRect_[4] = {0, 0, 0, 0};
    int stopRect_[4] = {0, 0, 0, 0};
};

} // namespace xyo
