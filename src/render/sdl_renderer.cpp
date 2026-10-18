// xyo-cpp: SDL2 ベースの表示レンダラ。
// 座標系:
//   scratch 座標: 中心原点・y 上向き（-240..240, -180..180）
//   stage 座標:   左上原点の論理 px（0..stageW_, 0..stageH_）
//   window 座標:  SDL クライアント px（stageRect_ 内にレターボックス表示）
//   pen 座標:     stage 座標を penScale_ 倍した CPU ビットマップ

#include "sdl_renderer.hpp"

#if defined(XYO_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

#ifdef XYO_HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

#if defined(XYO_PEN_GL)
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "../../third_party/stb_image.h"
#include "../core/cast.hpp"
#include "../core/fileio.hpp"
#include "../core/jsnum.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../gpu/gpu_isa.hpp"
#include "../gpu/gpu_analysis.hpp"
#include "../gpu/gpu_native.hpp"
#include "../model/target.hpp"
#include "effects.hpp"
#include "text.hpp"

namespace xyo {

#if defined(XYO_PEN_GL)
// GPU ペン層の状態（実体はこの TU 内のみ）。定義はメソッド実装と
// 同じ場所ではなく不完全型のままデストラクタ等で使えないため
// ここで定義する。
struct SdlRenderer::GlPen {
    SDL_GLContext ctx = nullptr;
    GLuint fbo = 0, tex = 0, prog = 0, vao = 0, vbo = 0, ebo = 0;
    GLint uSize = -1;
    // 手動バッファ（vector::resize の二重初期化を避ける）
    float* v = nullptr;
    size_t vLen = 0, vCap = 0;
    uint32_t* ix = nullptr;
    size_t ixLen = 0, ixCap = 0;
    std::vector<uint8_t> readback;   // 反転読み戻し用（確保は初回のみ）
    bool ok = false;
    bool fboDirty = false;   // FBO が penPx_ より新しい（readback 必要）
    bool shared = false;
    bool sdlTexDirty = false;
    GLuint upFbo = 0, upVao = 0;
    GLuint upProg2d = 0, upProgRect = 0;
    ~GlPen() { std::free(v); std::free(ix); }
};

/// GPU 並列スクリプト実行の状態（GL4.3 compute）。
/// VM compute shader が反復=レーンでバイトコードを解釈して
/// ストロークレコードを SSBO へ出力 → record-draw プログラムが
/// VS でレコードをクアッド展開し既存ペン FBO へ描画する。
struct SdlRenderer::GlCompute {
    GLuint cs = 0;        // VM compute プログラム
    GLuint rp = 0;        // レコード描画プログラム（VS 430 + FS）
    GLuint ordCs = 0;     // レーン major 記録 → 逐次順 draw-id stream
    GLuint vao = 0;       // 描画用（core profile で必須）
    GLuint paramsB = 0, globalsB = 0, codeB = 0;
    GLuint recsB = 0, cntsB = 0, loutB = 0;
    GLuint idsB = 0, offsB = 0;   // draw-id stream / レーン別オフセット
    GLint uSize = -1, uHalf = -1, uScale = -1, uMax = -1, uOrd = -1;
    GLint ordULanes = -1;
    size_t recCap = 0, cntCap = 0, loutCap = 0, idsCap = 0,
           offsCap = 0;
    GLint64 ssboMax = 0;          // GL_MAX_SHADER_STORAGE_BLOCK_SIZE
    GLint maxGroupsX = 0;         // GL_MAX_COMPUTE_WORK_GROUP_COUNT[0]
    std::vector<uint32_t> cntReadback;
    std::vector<uint32_t> ordOffsets;
    struct NativeProgs {
        GLuint fast = 0, capture = 0;
        GLint uBaseFast = -1, uDispFast = -1;
        GLint uBaseCap = -1, uDispCap = -1;
        GLint uColsFast = -1, uTotalFast = -1, uSeedFast = -1;
        GLint uColsCap = -1, uTotalCap = -1, uSeedCap = -1;
    };
    std::unordered_map<std::string, NativeProgs> nprogs;
    std::deque<std::string> nprogOrder;
    GLuint detCs = 0, rowCs = 0;
    GLint detUTotal = -1, detUColOff = -1, detULast = -1;
    GLint rowUCols = -1, rowURows = -1, rowUCap = -1, rowUColOff = -1,
          rowUStepX = -1, rowUStepY = -1,
          rowUBrush = -1, rowUFence = -1, rowULast = -1;
    GLuint nParamsB = 0, nMetaB = 0, nObB = 0, nLeB = 0, nAiB = 0,
           nAoB = 0;
    size_t nParamCap = 0, nMetaCap = 0, nObCap = 0, nLeCap = 0,
           nAiCap = 0, nAoCap = 0;
    uint64_t nScene = ~0ull;
    const void* nSceneProg = nullptr;
    std::vector<float> nScalar;
    std::vector<float> nAccum;
    bool nAccumValid = false;
    GLuint timQ[4] = {0, 0, 0, 0};
    uint32_t timCount = 0;
    bool ok = false;
};
#else
struct SdlRenderer::GlPen {};
struct SdlRenderer::GlCompute {};
#endif

namespace {

constexpr double kPenScale = 1.0;   // ペン解像度: 論理 px あたりの texel 数

uint8_t clampByte(double v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

// ---- UI 描画ヘルパ（走査線方式の図形塗り）-------------------------------

void fillDisc(SDL_Renderer* r, int cx, int cy, int rad) {
    for (int dy = -rad + 1; dy < rad; ++dy) {
        int dx = static_cast<int>(
            std::sqrt(static_cast<double>(rad) * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - dx + 1, cy + dy, cx + dx - 1, cy + dy);
    }
}

void fillDisc(SDL_Renderer* r, int cx, int cy, int rad,
              uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    fillDisc(r, cx, cy, rad);
}

void fillRoundedRect(SDL_Renderer* r, const SDL_Rect& rc, int rad) {
    if (rad <= 0 || rc.w < 2 * rad || rc.h < 2 * rad || rc.w <= 0 ||
        rc.h <= 0) {
        SDL_RenderFillRect(r, &rc);
        return;
    }
    SDL_Rect mid{rc.x, rc.y + rad, rc.w, rc.h - 2 * rad};
    SDL_RenderFillRect(r, &mid);
    SDL_Rect top{rc.x + rad, rc.y, rc.w - 2 * rad, rad};
    SDL_RenderFillRect(r, &top);
    SDL_Rect bot{rc.x + rad, rc.y + rc.h - rad, rc.w - 2 * rad, rad};
    SDL_RenderFillRect(r, &bot);
    fillDisc(r, rc.x + rad, rc.y + rad, rad);
    fillDisc(r, rc.x + rc.w - rad, rc.y + rad, rad);
    fillDisc(r, rc.x + rad, rc.y + rc.h - rad, rad);
    fillDisc(r, rc.x + rc.w - rad, rc.y + rc.h - rad, rad);
}

void fillRoundedRect(SDL_Renderer* r, const SDL_Rect& rc, int rad,
                     uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    fillRoundedRect(r, rc, rad);
}

/// 1px 枠付き角丸ボックス（外側を枠色で一回り大きく塗る方式）
void roundedBox(SDL_Renderer* r, SDL_Rect rc, int rad,
                uint8_t fr, uint8_t fg, uint8_t fb, uint8_t fa,
                uint8_t br, uint8_t bg, uint8_t bb, uint8_t ba) {
    SDL_Rect outer{rc.x - 1, rc.y - 1, rc.w + 2, rc.h + 2};
    fillRoundedRect(r, outer, rad + 1, br, bg, bb, ba);
    fillRoundedRect(r, rc, rad, fr, fg, fb, fa);
}

/// 正八角形（ストップサイン用）: 半幅 r、隅を 45° でカット
void fillOctagon(SDL_Renderer* r, int cx, int cy, int rad) {
    for (int dy = -rad + 1; dy < rad; ++dy) {
        int hw = static_cast<int>(rad * 1.4142) - std::abs(dy);
        hw = std::min(hw, rad);
        SDL_RenderDrawLine(r, cx - hw + 1, cy + dy, cx + hw - 1, cy + dy);
    }
}

} // namespace

// ============================================================ 初期化 ====

bool SdlRenderer::init(Runtime& rt, int scale) {
    rt_ = &rt;
    stageW_ = rt.stageWidth;
    stageH_ = rt.stageHeight;
    scale_ = std::max(1, scale);

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    window_ = SDL_CreateWindow("xyo", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, stageW_ * scale_,
                               stageH_ * scale_, SDL_WINDOW_RESIZABLE);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_)
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    recomputeViewport();

    for (Target* t : rt_->targets) {
        if (t->isStage) createDrawableForTarget(t, "backdrop");
        else if (t->isRenderable()) createDrawableForTarget(t, "sprite");
    }
    rt_->onTargetCreated = [this](Target* t) {
        if (t->isRenderable()) createDrawableForTarget(t, "sprite");
    };
    rt_->onTargetRemoved = [this](Target* t) {
        int id = drawableOfTarget(t);
        if (id >= 0) destroyDrawable(id);
    };
    // システムカーソル
    curArrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    curHand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    curIbeam_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    curActive_ = curArrow_;
    // ask and wait — 質問表示/非表示で入力欄を開閉
    rt_->onQuestion = [this](const std::u16string* q) {
        if (std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "[onQuestion] q=%p\n", (const void*)q);
        if (q) {
            askActive_ = true;
            askQuestion_ = *q;
            askInput_.clear();
            askCaret_ = 0;
            SDL_StartTextInput();
        } else {
            askActive_ = false;
            askInput_.clear();
            askCaret_ = 0;
            SDL_StopTextInput();
        }
    };
    return true;
}

SdlRenderer::~SdlRenderer() {
    SDL_StopTextInput();
    if (rt_) {
        rt_->onQuestion = nullptr;
        rt_->onTargetCreated = nullptr;
        rt_->onTargetRemoved = nullptr;
    }
    if (curArrow_) SDL_FreeCursor(curArrow_);
    if (curHand_) SDL_FreeCursor(curHand_);
    if (curIbeam_) SDL_FreeCursor(curIbeam_);
    for (auto& [id, e] : textCache_) SDL_DestroyTexture(e.tex);
    for (auto& [c, s] : skins_) if (s) freeSkin(*s);
    if (penTex_) SDL_DestroyTexture(penTex_);
#if defined(XYO_PEN_GL)
    if (glPen_ && glPen_->ctx) {
        if (SDL_GL_MakeCurrent(window_, glPen_->ctx) == 0) {
            GlPen& g = *glPen_;
            if (g.vao) glDeleteVertexArrays(1, &g.vao);
            if (g.vbo) glDeleteBuffers(1, &g.vbo);
            if (g.ebo) glDeleteBuffers(1, &g.ebo);
            if (g.prog) glDeleteProgram(g.prog);
            if (g.fbo) glDeleteFramebuffers(1, &g.fbo);
            if (g.tex) glDeleteTextures(1, &g.tex);
            if (glComp_) {
                GlCompute& c = *glComp_;
                if (c.cs) glDeleteProgram(c.cs);
                if (c.rp) glDeleteProgram(c.rp);
                if (c.vao) glDeleteVertexArrays(1, &c.vao);
                for (auto& [k, p] : c.nprogs) {
                    if (p.fast) glDeleteProgram(p.fast);
                    if (p.capture) glDeleteProgram(p.capture);
                }
                c.nprogs.clear();
                if (c.detCs) glDeleteProgram(c.detCs);
                if (c.rowCs) glDeleteProgram(c.rowCs);
                if (c.ordCs) glDeleteProgram(c.ordCs);
                GLuint bs[] = {c.paramsB, c.globalsB, c.codeB,
                               c.recsB, c.cntsB, c.loutB,
                               c.idsB, c.offsB,
                               c.nParamsB, c.nMetaB, c.nObB,
                               c.nLeB, c.nAiB, c.nAoB};
                glDeleteBuffers(14, bs);
                if (c.timQ[0]) glDeleteQueries(4, c.timQ);
            }
            if (g.upFbo) glDeleteFramebuffers(1, &g.upFbo);
            if (g.upVao) glDeleteVertexArrays(1, &g.upVao);
            if (g.upProg2d) glDeleteProgram(g.upProg2d);
            if (g.upProgRect) glDeleteProgram(g.upProgRect);
            if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
        }
        SDL_GL_DeleteContext(glPen_->ctx);
    }
    delete glPen_;
    glPen_ = nullptr;
    delete glComp_;
    glComp_ = nullptr;
#endif
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void SdlRenderer::recomputeViewport() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    double s = std::min(w / double(stageW_), h / double(stageH_));
    stageRect_.w = static_cast<int>(stageW_ * s);
    stageRect_.h = static_cast<int>(stageH_ * s);
    stageRect_.x = (w - stageRect_.w) / 2;
    stageRect_.y = (h - stageRect_.h) / 2;
    // UI ボタン（ステージ右上内側）
    int bs = static_cast<int>(22 * s);
    flagRect_[0] = stageRect_.x + stageRect_.w - 2 * bs - 12;
    flagRect_[1] = stageRect_.y + 6;
    flagRect_[2] = flagRect_[3] = bs;
    stopRect_[0] = stageRect_.x + stageRect_.w - bs - 6;
    stopRect_[1] = stageRect_.y + 6;
    stopRect_[2] = stopRect_[3] = bs;
}

// ========================================================= drawable ====

void SdlRenderer::createDrawableForTarget(Target* t, const char* group) {
    if (t->drawableID >= 0) return;
    Drawable d;
    d.id = nextDrawableId_++;
    d.target = t;
    d.group = group;
    d.order = t->layerOrder;
    drawables_[d.id] = d;
    t->drawableID = d.id;
}

SdlRenderer::Drawable* SdlRenderer::drawableFor(int id) {
    auto it = drawables_.find(id);
    return it == drawables_.end() ? nullptr : &it->second;
}
const SdlRenderer::Drawable* SdlRenderer::drawableFor(int id) const {
    auto it = const_cast<SdlRenderer*>(this)->drawables_.find(id);
    return it == drawables_.end() ? nullptr : &it->second;
}

void SdlRenderer::freeSkin(Skin& s) {
    if (s.tex) SDL_DestroyTexture(s.tex);
    s.tex = nullptr;
    for (auto& [k, v] : s.fx)
        if (v.tex) SDL_DestroyTexture(static_cast<SDL_Texture*>(v.tex));
    s.fx.clear();
}

SdlRenderer::Skin* SdlRenderer::decodeCostume(const Costume* c) {
    if (!c) return nullptr;
    auto fail = [this, c]() -> Skin* {
        skins_.try_emplace(c);
        return nullptr;
    };
    auto skin = std::make_unique<Skin>();
    if (c->data.empty()) return fail();

    if (c->dataFormat == "svg") {
#ifndef XYO_HAVE_RSVG
        // librsvg の無いビルド（Windows 等）は SVG を失敗として
        // キャッシュする — ビットマップコスチュームは描画できる。
        return fail();
#else
        // nanosvg は NUL 終端文字列を要求
        std::string xml(reinterpret_cast<const char*>(c->data.data()),
                        c->data.size());
        GError* err = nullptr;
        RsvgHandle* h = rsvg_handle_new_from_data(
            reinterpret_cast<const guint8*>(xml.data()),
            static_cast<gsize>(xml.size()), &err);
        if (err) g_error_free(err);
        if (!h) return fail();
        rsvg_handle_set_dpi(h, 96.0);
        double sw = 0, sh = 0;
        bool sized = rsvg_handle_get_intrinsic_size_in_pixels(h, &sw, &sh);
        if (!sized) {
            gboolean hasW, hasH, hasVb;
            RsvgLength lw, lh;
            RsvgRectangle vb;
            rsvg_handle_get_intrinsic_dimensions(h, &hasW, &lw, &hasH,
                                                 &lh, &hasVb, &vb);
            bool badDim =
                (hasW && !(std::isfinite(lw.length) && lw.length > 0)) ||
                (hasH && !(std::isfinite(lh.length) && lh.length > 0));
            if (!badDim && hasVb && std::isfinite(vb.width) &&
                vb.width > 0 && std::isfinite(vb.height) &&
                vb.height > 0) {
                sw = vb.width;
                sh = vb.height;
                sized = true;
            }
        }
        if (!sized || !(std::isfinite(sw) && sw > 0) ||
            !(std::isfinite(sh) && sh > 0)) {
            g_object_unref(h);
            return fail();
        }
        // scratch-render 相当の 2x ラスタ化（bitmapResolution も掛ける）
        double rs = 2.0 * std::max(1.0, c->bitmapResolution);
        double rw = sw * rs, rh = sh * rs;
        if (!(std::isfinite(rs) && rs > 0 && std::isfinite(rw) &&
              std::isfinite(rh)) || rw > 8192.0 || rh > 8192.0 ||
            rw * rh > 64.0 * 1024.0 * 1024.0) {
            g_object_unref(h);
            return fail();
        }
        skin->w = static_cast<int>(std::ceil(rw));
        skin->h = static_cast<int>(std::ceil(rh));
        skin->scale = rs;
        skin->px.assign(static_cast<size_t>(skin->w) * skin->h * 4, 0);
        cairo_surface_t* surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, skin->w, skin->h);
        cairo_status_t sst = cairo_surface_status(surf);
        cairo_t* cr =
            sst == CAIRO_STATUS_SUCCESS ? cairo_create(surf) : nullptr;
        if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            if (cr) cairo_destroy(cr);
            cairo_surface_destroy(surf);
            g_object_unref(h);
            return nullptr;
        }
        cairo_scale(cr, rs, rs);
        RsvgRectangle vp{0.0, 0.0, sw, sh};
        err = nullptr;
        bool rok = rsvg_handle_render_document(h, cr, &vp, &err);
        if (err) g_error_free(err);
        cairo_destroy(cr);
        g_object_unref(h);
        if (!rok) {
            cairo_surface_destroy(surf);
            return fail();
        }
        cairo_surface_flush(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        const uint8_t* spx = cairo_image_surface_get_data(surf);
        for (int y = 0; y < skin->h; ++y) {
            const uint8_t* srow =
                spx + static_cast<size_t>(y) * stride;
            uint8_t* drow =
                skin->px.data() + static_cast<size_t>(y) * skin->w * 4;
            for (int x = 0; x < skin->w; ++x) {
                uint32_t v;
                std::memcpy(&v, srow + static_cast<size_t>(x) * 4, 4);
                unsigned a = v >> 24;
                uint8_t* o = drow + static_cast<size_t>(x) * 4;
                if (!a) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                o[0] = static_cast<uint8_t>(std::min(
                    255u, (((v >> 16) & 0xFF) * 255 + a / 2) / a));
                o[1] = static_cast<uint8_t>(std::min(
                    255u, (((v >> 8) & 0xFF) * 255 + a / 2) / a));
                o[2] = static_cast<uint8_t>(
                    std::min(255u, ((v & 0xFF) * 255 + a / 2) / a));
                o[3] = static_cast<uint8_t>(a);
            }
        }
        cairo_surface_destroy(surf);
        // SVG の rotationCenter は論理 px（=svg px）
        skin->rcx = c->rotationCenterX;
        skin->rcy = c->rotationCenterY;
#endif // XYO_HAVE_RSVG
    } else {
        int w = 0, h = 0;
        unsigned char* px =
            stbi_load_from_memory(c->data.data(),
                                  static_cast<int>(c->data.size()), &w, &h,
                                  nullptr, 4);
        if (!px) return fail();
        skin->w = w;
        skin->h = h;
        skin->scale = std::max(0.01, c->bitmapResolution);
        skin->px.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
        // ビットマップの rotationCenter は生画像 px — 論理単位
        // （=px/bitmapResolution）へ直して保持。drawable 側は
        // rcx*scale でテクセル座標を復元する（SVG 経路と同じ約束）。
        skin->rcx = c->rotationCenterX / skin->scale;
        skin->rcy = c->rotationCenterY / skin->scale;
    }
    skin->logicalW = skin->w / skin->scale;
    skin->logicalH = skin->h / skin->scale;

    skin->tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STATIC, skin->w, skin->h);
    if (!skin->tex) return nullptr;
    SDL_UpdateTexture(skin->tex, nullptr, skin->px.data(), skin->w * 4);
    SDL_SetTextureBlendMode(skin->tex, SDL_BLENDMODE_BLEND);
    auto [it, _] = skins_.emplace(c, std::move(skin));
    return it->second.get();
}

SdlRenderer::Skin* SdlRenderer::skinForTarget(Target* t) {
    const Costume* c = t ? t->getCurrentCostume() : nullptr;
    if (!c) return nullptr;
    auto it = skins_.find(c);
    return it != skins_.end() ? it->second.get() : decodeCostume(c);
}
const SdlRenderer::Skin* SdlRenderer::skinForTarget(Target* t) const {
    return const_cast<SdlRenderer*>(this)->skinForTarget(t);
}

// ベーススキンにターゲットのエフェクトを適用した最終ビットマップ。
// エフェクト無しなら元のテクスチャ/画素をそのまま返す。
const SdlRenderer::Rendered* SdlRenderer::renderedFor(
        Target* t, const Skin* s, Rendered* out) const {
    out->tex = s->tex;
    out->px = s->px.data();
    out->w = s->w;
    out->h = s->h;
    if (!t || !hasActiveEffects(t->effects)) return out;
    std::string key = effectKey(t->effects);
    Skin* mut = const_cast<Skin*>(s);
    auto it = mut->fx.find(key);
    if (it == mut->fx.end()) {
        if (mut->fx.size() >= 24) {   // 値の連続変化での肥大化を全破棄で抑止
            for (auto& [k, v] : mut->fx)
                if (v.tex)
                    SDL_DestroyTexture(static_cast<SDL_Texture*>(v.tex));
            mut->fx.clear();
        }
        Skin::FxVariant v;
        applyEffects(s->px.data(), s->w, s->h, t->effects, v.px);
        v.tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STATIC, s->w, s->h);
        if (v.tex) {
            SDL_UpdateTexture(static_cast<SDL_Texture*>(v.tex), nullptr,
                              v.px.data(), s->w * 4);
            SDL_SetTextureBlendMode(static_cast<SDL_Texture*>(v.tex),
                                    SDL_BLENDMODE_BLEND);
        }
        it = mut->fx.emplace(std::move(key), std::move(v)).first;
    }
    out->tex = static_cast<SDL_Texture*>(it->second.tex);
    out->px = it->second.px.data();
    return out;
}

// ====================================================== 変換 / サンプル ==

bool SdlRenderer::xformFor(const Drawable& d, const Skin& s,
                           DrawXform& out) const {
    Target* t = d.target;
    if (!t) return false;
    out.k = (t->size / 100.0) / s.scale;             // tex px → stage 論理 px
    out.cx = stageX(t->x);
    out.cy = stageY(t->y);
    double angleDeg = 0;
    out.dirScaleX = 1;
    if (!t->isStage) {
        if (t->rotationStyle == "all around") {
            angleDeg = t->direction - 90.0;
        } else if (t->rotationStyle == "left-right") {
            out.dirScaleX = t->direction < 0 ? -1.0 : 1.0;
        }
    }
    double rad = angleDeg * M_PI / 180.0;
    out.cos = std::cos(rad);
    out.sin = std::sin(rad);
    out.skinHalfW = s.logicalW / 2.0;
    out.skinHalfH = s.logicalH / 2.0;
    return true;
}

// stage 論理 px 座標 → skin texel RGBA。範囲外/透明は false。
bool SdlRenderer::skinPxAt(const Drawable& d, double px, double py,
                           uint8_t out[4]) const {
    Target* t = d.target;
    if (!t || (!t->visible && !t->isStage)) return false;
    const Skin* s = skinForTarget(t);
    if (!s || !s->w) return false;
    DrawXform f;
    if (!xformFor(d, *s, f)) return false;
    Rendered r;
    const Rendered* rd = renderedFor(t, s, &r);
    if (!rd->px) return false;

    // アンカー（回転中心）からのオフセットを逆変換
    double dx = px - f.cx;
    double dy = py - f.cy;
    double lx = dx * f.cos + dy * f.sin;   // 逆回転
    double ly = -dx * f.sin + dy * f.cos;
    lx *= f.dirScaleX;
    // 論理 → テクスチャ px（回転中心が origin）
    double k = t->size / 100.0;
    double tx = (lx / k + s->rcx) * s->scale;
    double ty = (ly / k + s->rcy) * s->scale;
    int ix = static_cast<int>(std::floor(tx));
    int iy = static_cast<int>(std::floor(ty));
    if (ix < 0 || iy < 0 || ix >= rd->w || iy >= rd->h) return false;
    const uint8_t* p = &rd->px[(static_cast<size_t>(iy) * rd->w + ix) * 4];
    if (p[3] < 10) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
    return true;
}

// 背面→前面の全レイヤ（backdrop → pen → sprite）を合成サンプル
bool SdlRenderer::stageColorAt(double px, double py, int skipId,
                               uint8_t out[4]) const {
    // GPU ペン層の未描画バッチを反映してから penPx_ を読む
    const_cast<SdlRenderer*>(this)->penSyncCpu();
    // 前面から順に最初の不透明ピクセルを採る
    std::vector<const Drawable*> order;
    order.reserve(drawables_.size());
    for (auto& [id, d] : drawables_) order.push_back(&d);
    std::sort(order.begin(), order.end(), [](const Drawable* a,
                                             const Drawable* b) {
        return a->order > b->order;   // 前面 → 背面
    });
    uint8_t px4[4];
    for (const Drawable* d : order) {
        if (d->id == skipId || d->group != "sprite") continue;
        if (skinPxAt(*d, px, py, px4)) { std::memcpy(out, px4, 4); return true; }
    }
    // ペンレイヤ
    if (penTex_ && penW_ > 0) {
        int ix = static_cast<int>(px * penScale_);
        int iy = static_cast<int>(py * penScale_);
        if (ix >= 0 && iy >= 0 && ix < penW_ && iy < penH_) {
            const uint8_t* p =
                &penPx_[(static_cast<size_t>(iy) * penW_ + ix) * 4];
            if (p[3] > 0) {
                int a = p[3];
                for (int i = 0; i < 3; ++i)
                    out[i] = static_cast<uint8_t>(
                        std::min(255, (p[i] * 255 + a / 2) / a));
                out[3] = static_cast<uint8_t>(a);
                return true;
            }
        }
    }
    // 背景
    for (const Drawable* d : order) {
        if (d->id == skipId || d->group != "backdrop") continue;
        if (skinPxAt(*d, px, py, px4)) { std::memcpy(out, px4, 4); return true; }
    }
    out[0] = out[1] = out[2] = 255; out[3] = 255;   // 白背景
    return true;
}

// ========================================================= RendererAPI ==

int SdlRenderer::pick(double clientX, double clientY) {
    // client 座標（canvas 左上原点）→ stage 論理 px
    double s = stageRect_.w / double(stageW_);
    double px = clientX / s;
    double py = clientY / s;
    std::vector<const Drawable*> order;
    for (auto& [id, d] : drawables_)
        if (d.group == "sprite") order.push_back(&d);
    std::sort(order.begin(), order.end(),
              [](const Drawable* a, const Drawable* b) {
                  return a->order > b->order;
              });
    uint8_t px4[4];
    for (const Drawable* d : order)
        if (skinPxAt(*d, px, py, px4)) return d->id;
    return -1;
}

void SdlRenderer::getDrawableBounds(int drawableID, double* b) const {
    b[0] = b[1] = b[2] = b[3] = 0;
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target) return;
    const Skin* s = skinForTarget(d->target);
    if (!s) return;
    DrawXform f;
    if (!xformFor(*d, *s, f)) return;
    // 4 隅を回転して AABB
    Target* t = d->target;
    double k = (t->size / 100.0);
    double corners[4][2] = {
        {-s->rcx, -s->rcy},
        {s->logicalW - s->rcx, -s->rcy},
        {-s->rcx, s->logicalH - s->rcy},
        {s->logicalW - s->rcx, s->logicalH - s->rcy},
    };
    double l = 1e9, r = -1e9, tp = 1e9, bt = -1e9;
    for (auto& c : corners) {
        double lx = c[0] * f.dirScaleX * k;
        double ly = c[1] * k;
        double rx = lx * f.cos - ly * f.sin;
        double ry = lx * f.sin + ly * f.cos;
        double sx = f.cx + rx, sy = f.cy + ry;
        l = std::min(l, sx); r = std::max(r, sx);
        tp = std::min(tp, sy); bt = std::max(bt, sy);
    }
    b[0] = scratchX(l); b[1] = scratchX(r);
    b[2] = scratchY(tp); b[3] = scratchY(bt);
}

bool SdlRenderer::hasDrawable(int drawableID) const {
    return drawableFor(drawableID) != nullptr;
}

void SdlRenderer::getFencedPosition(int drawableID, double& x, double& y,
                                    bool fencing) const {
    if (!fencing) return;
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target) return;
    Target* t = d->target;
    double bounds[4] = {t->x, t->x, t->y, t->y};
    if (skinForTarget(t)) getDrawableBounds(drawableID, bounds);
    double inset = std::floor(
        std::min(bounds[1] - bounds[0], bounds[2] - bounds[3]) / 2);
    double sx = stageW_ / 2.0 - std::min(15.0, inset);
    double sy = stageH_ / 2.0 - std::min(15.0, inset);
    double dx = x - t->x, dy = y - t->y;
    if (bounds[1] + dx < -sx) x = std::ceil(t->x - (sx + bounds[1]));
    else if (bounds[0] + dx > sx) x = std::floor(t->x + (sx - bounds[0]));
    if (bounds[2] + dy < -sy) y = std::ceil(t->y - (sy + bounds[2]));
    else if (bounds[3] + dy > sy) y = std::floor(t->y + (sy - bounds[3]));
}

void SdlRenderer::getCurrentSkinSize(int drawableID, double& w,
                                     double& h) const {
    w = h = 1;
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target) return;
    const Skin* s = skinForTarget(d->target);
    if (s) { w = s->logicalW; h = s->logicalH; }
}

double SdlRenderer::setDrawableOrder(int drawableID, double order,
                                     const char* group, bool isRelative) {
    Drawable* d = drawableFor(drawableID);
    if (!d) return 0;
    if (group && *group) d->group = group;
    if (std::isinf(order)) {
        // グループ内の最前/最後へ
        double best = order > 0 ? -1e18 : 1e18;
        for (auto& [id, o] : drawables_) {
            if (o.group != d->group || o.id == d->id) continue;
            best = order > 0 ? std::max(best, o.order)
                             : std::min(best, o.order);
        }
        d->order = std::abs(best) > 1e17 ? 0
                   : best + (order > 0 ? 1 : -1);
    } else if (isRelative) {
        d->order += order;
    } else {
        d->order = order;
    }
    return d->order;
}

void SdlRenderer::destroyDrawable(int drawableID) {
    drawables_.erase(drawableID);
}

void SdlRenderer::emitVisualChange(Target*) {
    // draw() は毎フレーム target 状態を読み直すためキャッシュ無効化は不要
}

std::vector<int> SdlRenderer::drawableOrder() const {
    std::vector<const Drawable*> order;
    order.reserve(drawables_.size());
    for (auto& [id, d] : drawables_) order.push_back(&d);
    std::sort(order.begin(), order.end(),
              [](const Drawable* a, const Drawable* b) {
                  if (a->group != b->group) return a->group < b->group;
                  return a->order < b->order;
              });
    std::vector<int> out;
    for (const Drawable* d : order) out.push_back(d->id);
    return out;
}

int SdlRenderer::drawableOfTarget(Target* t) const {
    for (auto& [id, d] : drawables_)
        if (d.target == t) return id;
    return -1;
}

// ------------------------------------------------------------ タッチ判定

bool SdlRenderer::drawableTouching(int drawableID, double x, double y) {
    const Drawable* d = drawableFor(drawableID);
    if (!d) return false;
    uint8_t px4[4];
    return skinPxAt(*d, stageX(x), stageY(y), px4);
}

bool SdlRenderer::isTouchingDrawables(int drawableID, const int* others,
                                      size_t n) {
    const Drawable* a = drawableFor(drawableID);
    if (!a || !a->target || !a->target->visible) return false;
    const Skin* sa = skinForTarget(a->target);
    if (!sa) return false;
    // 自身の AABB（stage px）
    double ab[4];
    getDrawableBounds(drawableID, ab);
    double ax0 = stageX(std::min(ab[0], ab[1]));
    double ax1 = stageX(std::max(ab[0], ab[1]));
    double ay0 = stageY(std::max(ab[2], ab[3]));
    double ay1 = stageY(std::min(ab[2], ab[3]));
    for (size_t i = 0; i < n; ++i) {
        const Drawable* b = drawableFor(others[i]);
        if (!b || !b->target || !b->target->visible || b->id == a->id)
            continue;
        if (!skinForTarget(b->target)) continue;
        double bb[4];
        getDrawableBounds(others[i], bb);
        double bx0 = stageX(std::min(bb[0], bb[1]));
        double bx1 = stageX(std::max(bb[0], bb[1]));
        double by0 = stageY(std::max(bb[2], bb[3]));
        double by1 = stageY(std::min(bb[2], bb[3]));
        int x0 = static_cast<int>(std::max(ax0, bx0));
        int x1 = static_cast<int>(std::ceil(std::min(ax1, bx1)));
        int y0 = static_cast<int>(std::max(ay0, by0));
        int y1 = static_cast<int>(std::ceil(std::min(ay1, by1)));
        if (x0 >= x1 || y0 >= y1) continue;
        uint8_t pa[4], pb[4];
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (skinPxAt(*a, x + 0.5, y + 0.5, pa) &&
                    skinPxAt(*b, x + 0.5, y + 0.5, pb))
                    return true;
    }
    return false;
}

bool SdlRenderer::isTouchingColor(int drawableID, uint32_t rgb) {
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target || !d->target->visible) return false;
    double b[4];
    getDrawableBounds(drawableID, b);
    int x0 = static_cast<int>(stageX(std::min(b[0], b[1])));
    int x1 = static_cast<int>(std::ceil(stageX(std::max(b[0], b[1]))));
    int y0 = static_cast<int>(stageY(std::max(b[2], b[3])));
    int y1 = static_cast<int>(std::ceil(stageY(std::min(b[2], b[3]))));
    uint8_t self[4], behind[4];
    uint8_t tr = (rgb >> 16) & 0xFF, tg = (rgb >> 8) & 0xFF, tb = rgb & 0xFF;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (!skinPxAt(*d, x + 0.5, y + 0.5, self)) continue;
            if (!stageColorAt(x + 0.5, y + 0.5, drawableID, behind)) continue;
            if (std::abs(int(behind[0]) - tr) <= 5 &&
                std::abs(int(behind[1]) - tg) <= 5 &&
                std::abs(int(behind[2]) - tb) <= 5)
                return true;
        }
    return false;
}

bool SdlRenderer::colorTouchingColor(uint32_t color, uint32_t target,
                                     int drawableID) {
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target || !d->target->visible) return false;
    double b[4];
    getDrawableBounds(drawableID, b);
    int x0 = static_cast<int>(stageX(std::min(b[0], b[1])));
    int x1 = static_cast<int>(std::ceil(stageX(std::max(b[0], b[1]))));
    int y0 = static_cast<int>(stageY(std::max(b[2], b[3])));
    int y1 = static_cast<int>(std::ceil(stageY(std::min(b[2], b[3]))));
    uint8_t c1r = (color >> 16) & 0xFF, c1g = (color >> 8) & 0xFF,
            c1b = color & 0xFF;
    uint8_t c2r = (target >> 16) & 0xFF, c2g = (target >> 8) & 0xFF,
            c2b = target & 0xFF;
    uint8_t self[4], behind[4];
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (!skinPxAt(*d, x + 0.5, y + 0.5, self)) continue;
            if (std::abs(int(self[0]) - c1r) > 5 ||
                std::abs(int(self[1]) - c1g) > 5 ||
                std::abs(int(self[2]) - c1b) > 5)
                continue;
            if (!stageColorAt(x + 0.5, y + 0.5, drawableID, behind)) continue;
            if (std::abs(int(behind[0]) - c2r) <= 5 &&
                std::abs(int(behind[1]) - c2g) <= 5 &&
                std::abs(int(behind[2]) - c2b) <= 5)
                return true;
        }
    return false;
}

// ================================================================ ペン ===

int SdlRenderer::createPenSkin() {
    penEnsure();
    return penSkinId_;
}

void SdlRenderer::penEnsure() {
    if (penTex_) return;
    penScale_ = kPenScale;
    penW_ = static_cast<int>(stageW_ * penScale_);
    penH_ = static_cast<int>(stageH_ * penScale_);
    penPx_.assign(static_cast<size_t>(penW_) * penH_ * 4, 0);
    penUploadPx_.assign(penPx_.size(), 0);
    penTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, penW_, penH_);
    if (penTex_) SDL_SetTextureBlendMode(penTex_, SDL_BLENDMODE_BLEND);
    penSkinId_ = 1;
    penDirty_ = true;
#if defined(XYO_PEN_GL)
    if (!glPen_ && !initGlPen() && std::getenv("XYO_DEBUG"))
        std::fprintf(stderr, "GL pen unavailable; CPU raster\n");
#endif
}

void SdlRenderer::penBlendPremult(uint8_t* p, const float rgba[4],
                                  double coverage) {
    double sa = std::clamp(double(rgba[3]) * coverage, 0.0, 1.0);
    if (sa <= 0.0) return;
    if (sa >= 1.0) {
        for (int i = 0; i < 3; ++i)
            p[i] = static_cast<uint8_t>(
                std::lround(std::clamp(double(rgba[i]), 0.0, 1.0) * 255.0));
        p[3] = 255;
        return;
    }
    double om = 1.0 - sa;
    for (int i = 0; i < 3; ++i)
        p[i] = static_cast<uint8_t>(std::clamp(
            std::lround(std::clamp(double(rgba[i]), 0.0, 1.0) * sa * 255.0 +
                        p[i] * om), 0L, 255L));
    p[3] = static_cast<uint8_t>(
        std::clamp(std::lround(sa * 255.0 + p[3] * om), 0L, 255L));
}

void SdlRenderer::penBlendPx(int x, int y, const uint8_t c[4]) {
    if (x < 0 || y < 0 || x >= penW_ || y >= penH_) return;
    float rgba[4] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f,
                     c[3] / 255.0f};
    penBlendPremult(&penPx_[(static_cast<size_t>(y) * penW_ + x) * 4], rgba,
                    1.0);
}

// ========================================================== GPU ペン層 ====
// OpenGL レンダラ時のみ: ストロークを quad として頂点バッファに蓄積し、
// フラッシュ時に FBO へ一括描画する。カバレッジはシェーダで
// clamp(radius+0.5-distToSegment) を解析評価（penBlendPremult と同じ式、
// CPU ラスタと同一仕様）。描画はペン層解像度の FBO に蓄積され、
// CPU 側バッファが必要な読者（stageColorAt/penStamp/upload/savePPM）の
// 前で readback する。
#if defined(XYO_PEN_GL)

namespace {

const char* kPenVsCore = R"GLSL(#version 330 core
layout(location=0) in vec2 aPos;    // ペン層 px（y 下向き）
layout(location=1) in vec2 aUV;     // ストローク空間 (u=線分方向, v=垂直) px
layout(location=2) in vec2 aMeta;   // (L, radius)
layout(location=3) in vec4 aColor;  // straight rgba
uniform vec2 uSize;
out vec2 vUV;
out vec2 vMeta;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x / uSize.x * 2.0 - 1.0,
                       1.0 - aPos.y / uSize.y * 2.0, 0.0, 1.0);
    vUV = aUV; vMeta = aMeta; vColor = aColor;
}
)GLSL";

const char* kPenFsCore = R"GLSL(#version 330 core
in vec2 vUV;
in vec2 vMeta;
in vec4 vColor;
out vec4 frag;
void main() {
    float u = clamp(vUV.x, 0.0, vMeta.x);
    float dist = length(vec2(vUV.x - u, vUV.y));
    float cov = clamp(vMeta.y + 0.5 - dist, 0.0, 1.0);
    float sa = clamp(vColor.a, 0.0, 1.0) * cov;
    vec3 c = clamp(vColor.rgb, vec3(0.0), vec3(1.0));
    frag = vec4(c * sa, sa);   // premult
}
)GLSL";

// 互換プロファイル用（GLSL 120）
const char* kPenVsCompat = R"GLSL(#version 120
attribute vec2 aPos;
attribute vec2 aUV;
attribute vec2 aMeta;
attribute vec4 aColor;
uniform vec2 uSize;
varying vec2 vUV;
varying vec2 vMeta;
varying vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x / uSize.x * 2.0 - 1.0,
                       1.0 - aPos.y / uSize.y * 2.0, 0.0, 1.0);
    vUV = aUV; vMeta = aMeta; vColor = aColor;
}
)GLSL";

const char* kPenFsCompat = R"GLSL(#version 120
varying vec2 vUV;
varying vec2 vMeta;
varying vec4 vColor;
void main() {
    float u = clamp(vUV.x, 0.0, vMeta.x);
    float dist = length(vec2(vUV.x - u, vUV.y));
    float cov = clamp(vMeta.y + 0.5 - dist, 0.0, 1.0);
    float sa = clamp(vColor.a, 0.0, 1.0) * cov;
    vec3 c = clamp(vColor.rgb, vec3(0.0), vec3(1.0));
    gl_FragColor = vec4(c * sa, sa);
}
)GLSL";

GLuint penCompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "pen shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

bool SdlRenderer::penGpu() const {
    return glPen_ && glPen_->ok;
}

// コンテキスト要求: まず 3.3 core（VAO 必須・GLSL330）、駄目なら
// デフォルトプロファイル（互換・GLSL120・VAO 不要）へフォールバック。
bool SdlRenderer::initGlPen() {
    if (std::getenv("XYO_PEN_CPU") || !window_ || !renderer_) return false;
    SDL_RendererInfo ri{};
    if (SDL_GetRendererInfo(renderer_, &ri) != 0 || !ri.name) return false;
    if (!std::strstr(ri.name, "opengl") &&
        !std::strstr(ri.name, "opengles"))
        return false;
    sdlGlCtx_ = SDL_GL_GetCurrentContext();

    glPen_ = new GlPen();
    GlPen& g = *glPen_;
    const char* interopEnv = std::getenv("XYO_GL_INTEROP");
    const bool wantShared =
        sdlGlCtx_ && !(interopEnv && interopEnv[0] == '0');
    static const struct { int major, minor, profile; } kCtxTries[] = {
        {4, 3, SDL_GL_CONTEXT_PROFILE_CORE},   // compute shader 用
        {3, 3, SDL_GL_CONTEXT_PROFILE_CORE},
        {0, 0, 0},   // デフォルト（互換 / ES）
    };
    auto tryCtx = [&](bool share) -> SDL_GLContext {
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,
                            share ? 1 : 0);
        for (const auto& t : kCtxTries) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, t.major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, t.minor);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, t.profile);
            if (share && sdlGlCtx_)
                SDL_GL_MakeCurrent(window_, sdlGlCtx_);
            SDL_GLContext c = SDL_GL_CreateContext(window_);
            if (c) return c;
        }
        return nullptr;
    };
    if (wantShared) g.ctx = tryCtx(true);
    if (!g.ctx) g.ctx = tryCtx(false);
    SDL_GL_ResetAttributes();
    if (!g.ctx) { delete glPen_; glPen_ = nullptr; return false; }
    if (SDL_GL_MakeCurrent(window_, g.ctx) != 0) {
        SDL_GL_DeleteContext(g.ctx);
        delete glPen_;
        glPen_ = nullptr;
        return false;
    }
    if (wantShared && sdlGlCtx_) {
        GLuint probe = 0;
        glGenTextures(1, &probe);
        if (probe) glBindTexture(GL_TEXTURE_2D, probe);
        if (probe && SDL_GL_MakeCurrent(window_, sdlGlCtx_) == 0) {
            g.shared = glIsTexture(probe) == GL_TRUE;
            SDL_GL_MakeCurrent(window_, g.ctx);
        }
        if (probe) glDeleteTextures(1, &probe);
    }

    // シェーダ（core 3.3 → 330、その他は 120 を順に試す）
    const char* vsrc[2] = {kPenVsCore, kPenVsCompat};
    const char* fsrc[2] = {kPenFsCore, kPenFsCompat};
    for (int i = 0; i < 2 && !g.prog; ++i) {
        GLuint vs = penCompileShader(GL_VERTEX_SHADER, vsrc[i]);
        GLuint fs = penCompileShader(GL_FRAGMENT_SHADER, fsrc[i]);
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            continue;
        }
        g.prog = glCreateProgram();
        glAttachShader(g.prog, vs);
        glAttachShader(g.prog, fs);
        glBindAttribLocation(g.prog, 0, "aPos");
        glBindAttribLocation(g.prog, 1, "aUV");
        glBindAttribLocation(g.prog, 2, "aMeta");
        glBindAttribLocation(g.prog, 3, "aColor");
        glLinkProgram(g.prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = GL_FALSE;
        glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(g.prog, sizeof(log), nullptr, log);
            std::fprintf(stderr, "pen prog: %s\n", log);
            glDeleteProgram(g.prog);
            g.prog = 0;
        }
    }
    auto fail = [&]() -> bool {
        if (g.fbo) glDeleteFramebuffers(1, &g.fbo);
        if (g.tex) glDeleteTextures(1, &g.tex);
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (g.ebo) glDeleteBuffers(1, &g.ebo);
        if (g.vao) glDeleteVertexArrays(1, &g.vao);
        if (g.prog) glDeleteProgram(g.prog);
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
        SDL_GL_DeleteContext(glPen_->ctx);
        delete glPen_;
        glPen_ = nullptr;
        return false;
    };
    if (!g.prog) return fail();
    g.uSize = glGetUniformLocation(g.prog, "uSize");

    // FBO + ペン層テクスチャ（CPU バッファと同じ premult RGBA8）
    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, penW_, penH_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &g.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, g.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) return fail();

    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ebo);
    if (glGenVertexArrays) {
        glGenVertexArrays(1, &g.vao);
        glBindVertexArray(g.vao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    const GLsizei stride = 10 * sizeof(float);
    const uint8_t* base = nullptr;
    for (int a = 0; a < 4; ++a) {
        glEnableVertexAttribArray(a);
        glVertexAttribPointer(a, a == 3 ? 4 : 2, GL_FLOAT, GL_FALSE,
                              stride, base + (a == 3 ? 6 : a * 2) * 4);
    }
    glViewport(0, 0, penW_, penH_);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g.ok = true;
    if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    return true;
}

// ストロークを quad（2 三角形）としてバッチへ積む。
// CPU パスと同じ座標系: A=(ax,ay)、方向 T=(nx,ny)、法線 N=(-ny,nx)。
void SdlRenderer::penStrokeGpu(const float rgba[4], float width,
                               float fx0, float fy0, float fdx, float fdy) {
    GlPen& g = *glPen_;
    float dx = fdx, dy = -fdy;
    float L = std::hypot(fdx, fdy);
    if (!std::isfinite(L)) return;
    const float eps = 1e-3f;
    float basisX = (std::fabs(dx) < eps && std::fabs(dy) < eps)
                       ? eps : dx;
    float denom = std::max(L, eps);
    float nx = basisX / denom, ny = dy / denom;
    float nn = nx * nx + ny * ny;
    if (!(nn > 0.0f) || !std::isfinite(nn)) return;
    float ax = static_cast<float>(stageX(fx0));
    float ay = static_cast<float>(stageY(fy0));
    float radius = width * 0.5f;
    float m = radius + 1.0f;   // cov>0 の半径 r+0.5 + ラスタ余白
    // quad 角: (u,v) ∈ {-m, L+m} × {-m, m} → 位置 A + u*T + v*N
    const float us[2] = {-m, L + m};
    const float vs[2] = {-m, m};
    uint32_t base = static_cast<uint32_t>(g.vLen / 10);
    if (g.vLen + 40 > g.vCap) {
        size_t nc = g.vCap ? g.vCap * 2 : 1 << 20;
        g.v = static_cast<float*>(
            std::realloc(g.v, nc * sizeof(float)));
        g.vCap = nc;
    }
    if (g.ixLen + 6 > g.ixCap) {
        size_t nc = g.ixCap ? g.ixCap * 2 : 1 << 18;
        g.ix = static_cast<uint32_t*>(
            std::realloc(g.ix, nc * sizeof(uint32_t)));
        g.ixCap = nc;
    }
    float* p = g.v + g.vLen;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            float u = us[i], v = vs[j];
            p[0] = ax + u * nx - v * ny;
            p[1] = ay + u * ny + v * nx;
            p[2] = u; p[3] = v; p[4] = L; p[5] = radius;
            p[6] = rgba[0]; p[7] = rgba[1]; p[8] = rgba[2]; p[9] = rgba[3];
            p += 10;
        }
    g.vLen += 40;
    uint32_t* q = g.ix + g.ixLen;
    q[0] = base;     q[1] = base + 1; q[2] = base + 2;
    q[3] = base + 2; q[4] = base + 1; q[5] = base + 3;
    g.ixLen += 6;
    // 過剰なバッチ膨張を抑制（~2M 頂点で途中フラッシュ）
    if (g.vLen > 20u * 1000 * 10) penFlushGpu();
}

// バッチを FBO へ描画する（readback なし — GPU 上に保持）。
void SdlRenderer::penFlushGpu() {
    if (!penGpu()) return;
    GlPen& g = *glPen_;
    if (g.vLen == 0) return;
    if (SDL_GL_MakeCurrent(window_, g.ctx) != 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, penW_, penH_);
    glUseProgram(g.prog);
    glUniform2f(g.uSize, float(penW_), float(penH_));
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.vLen * sizeof(float)),
                 g.v, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 GLsizeiptr(g.ixLen * sizeof(uint32_t)),
                 g.ix, GL_STREAM_DRAW);
    if (g.vao) glBindVertexArray(g.vao);
    else {
        const GLsizei stride = 10 * sizeof(float);
        const uint8_t* base = nullptr;
        for (int a = 0; a < 4; ++a) {
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, a == 3 ? 4 : 2, GL_FLOAT, GL_FALSE,
                                  stride,
                                  base + (a == 3 ? 6 : a * 2) * 4);
        }
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_TRIANGLES, GLsizei(g.ixLen), GL_UNSIGNED_INT,
                   nullptr);
    g.vLen = 0;
    g.ixLen = 0;
    g.fboDirty = true;
    g.sdlTexDirty = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
}

// FBO を penPx_ へ readback する（fboDirty 時のみ）。
// GL の readback は下→上なので行を反転して CPU バッファへ。
void SdlRenderer::penSyncCpu() {
    if (!penGpu()) return;
    GlPen& g = *glPen_;
    penFlushGpu();
    if (!g.fboDirty) return;
    if (SDL_GL_MakeCurrent(window_, g.ctx) != 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    const size_t row = static_cast<size_t>(penW_) * 4;
    if (g.readback.size() != row * penH_) g.readback.resize(row * penH_);
    glReadPixels(0, 0, penW_, penH_, GL_RGBA, GL_UNSIGNED_BYTE,
                 g.readback.data());
    for (int y = 0; y < penH_; ++y)
        std::memcpy(&penPx_[static_cast<size_t>(penH_ - 1 - y) * row],
                    &g.readback[static_cast<size_t>(y) * row], row);
    g.fboDirty = false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    penDirty_ = true;
}

// ===================================================== GPU 並列 VM ======
// src/gpu/gpu_isa.hpp の ISA を解釈する compute shader。
// 1 反復 = 1 レーン（gl_GlobalInvocationID.x）として本体を実行し、
// ストロークを (vec4×3) レコードとして SSBO に吐く。
// その後 record-draw プログラムがレコードをクアッド展開してペン FBO へ
// 描画する（レーン順 = 直列順序と一致）。
namespace {

const char* kGpuVmSrc = R"GLSL(#version 430 core
layout(local_size_x=64) in;

// params: [0]sx [1]sy [2]size [3]h [4]s [5]v [6]t [7]penDown
//  [8]fenceL [9]fenceR [10]fenceB [11]fenceT
//  [12]nSlots [13]nWb [14]maxPerLane [15]lanes [16]seed
//  [17]codeLen [18]stride [19]miscLimits  [20..20+nWb) wbSlot[]
//  [20+nWb]=penDownAll [21+nWb]=endAttrMask
//  [22+nWb..27+nWb)=endAttr{size,h,s,v,t}（lanes>0 の初期状態）
layout(std430, binding=0) readonly buffer P { float pr[]; };
layout(std430, binding=1) readonly buffer C { uint code[]; };
layout(std430, binding=2) writeonly buffer R { vec4 recs[]; };
layout(std430, binding=3) writeonly buffer N { uint cnts[]; };
layout(std430, binding=4) writeonly buffer O { float lout[]; };
layout(std430, binding=5) readonly buffer G { float globs[]; };

uint lane, iter, maxPerLane, nWb, stride, seed;
float posX, posY, penSize;
float hC, sC, vC, tC, shC;
vec4 rgba;
bool penDown, guarded;
float slots[96];
float st[128];
int sp = 0;
uint recN = 0;
uint flags = 0;
uint wbMask[4];

vec3 hsv2rgb(float h100, float s100, float v100) {
    float h = mod(h100 * 3.6, 360.0); if (h < 0.0) h += 360.0;
    float s = clamp(s100 / 100.0, 0.0, 1.0);
    float v = clamp(v100 / 100.0, 0.0, 1.0);
    int i = int(floor(h / 60.0));
    float f = h / 60.0 - float(i);
    float p = v * (1.0 - s), q = v * (1.0 - s * f),
          t = v * (1.0 - s * (1.0 - f));
    vec3 r;
    if (i == 1) r = vec3(q, v, p);
    else if (i == 2) r = vec3(p, v, t);
    else if (i == 3) r = vec3(p, q, v);
    else if (i == 4) r = vec3(t, p, v);
    else if (i == 5) r = vec3(v, p, q);
    else r = vec3(v, t, p);
    return floor(r * 255.0) / 255.0;
}
vec3 rgb2hsv(vec3 c255) {   // 入力 0-255 → (h,s,v) = (deg,0-1,0-1)
    vec3 c = c255 / 255.0;
    float x = min(c.r, min(c.g, c.b));
    float v = max(c.r, max(c.g, c.b));
    float h = 0.0, s = 0.0;
    if (x != v) {
        float f = (c.r == x) ? c.g - c.b
                : (c.g == x) ? c.b - c.r : c.r - c.g;
        float i = (c.r == x) ? 3.0 : (c.g == x) ? 5.0 : 1.0;
        h = mod((i - f / (v - x)) * 60.0, 360.0);
        s = (v - x) / v;
    }
    return vec3(h, s, v);
}
void updRgba() {
    rgba = vec4(hsv2rgb(hC, sC, vC), 1.0 - tC / 100.0);
}
float rnd() {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return float(seed) * (1.0 / 4294967296.0);
}
float wrapC(float v) { v = mod(v, 100.0); return v < 0.0 ? v + 100.0 : v; }

void push(float v) { st[sp++] = v; }
float pop() { return st[--sp]; }

void stroke(float x0, float y0, float x1, float y1) {
    if (isnan(x0) || isinf(x0) || isnan(y0) || isinf(y0) ||
        isnan(x1) || isinf(x1) || isnan(y1) || isinf(y1)) return;
    if (recN < maxPerLane) {
        uint i = (lane * maxPerLane + recN) * 3u;
        recs[i] = vec4(x0, y0, x1, y1);
        recs[i + 1u] = vec4(penSize, rgba.r, rgba.g, rgba.b);
        recs[i + 2u] = vec4(rgba.a, 0.0, 0.0, 0.0);
    }
    recN++;
}
void gotoXY(float nx, float ny) {
    float fl = pr[8], fr = pr[9], fb = pr[10], ft = pr[11];
    if (nx > fr) nx = floor(fr);
    else if (nx < fl) nx = ceil(fl);
    if (ny > ft) ny = floor(ft);
    else if (ny < fb) ny = ceil(fb);
    if (penDown) stroke(posX, posY, nx, ny);
    posX = nx; posY = ny;
    flags |= 1u;
}
void writeBack() {
    uint base = lane * stride;
    lout[base] = uintBitsToFloat(flags | (penDown ? 8u : 0u));
    lout[base + 1u] = posX;
    lout[base + 2u] = posY;
    lout[base + 3u] = penSize;
    lout[base + 4u] = hC; lout[base + 5u] = sC;
    lout[base + 6u] = vC; lout[base + 7u] = tC;
    uint mw = (nWb + 31u) / 32u;
    for (uint i = 0u; i < mw && i < 4u; ++i)
        lout[base + 8u + i] = uintBitsToFloat(wbMask[i]);
    for (uint i = 0u; i < nWb; ++i)
        lout[base + 8u + mw + i] = slots[uint(pr[20u + i])];
    lout[base + 8u + mw + nWb] = shC;   // PENCOLN 時点の brightness/2
    cnts[lane] = recN | (guarded ? 0x80000000u : 0u);
}

void main() {
    lane = gl_GlobalInvocationID.x;
    // ディスパッチは 64 単位で切り上げ — 範囲外レーンは即 return
    // （cnts/lout/recs への範囲外書き込み防止）
    if (lane >= uint(pr[15])) return;
    iter = lane;
    guarded = false;
    maxPerLane = uint(pr[14]);
    nWb = uint(pr[13]);
    stride = uint(pr[18]);
    seed = iter * 2654435761u ^ floatBitsToUint(pr[16]);
    if (seed == 0u) seed = 2654435769u;   // xorshift は 0 から復帰不能
    posX = pr[0]; posY = pr[1];
    penSize = pr[2];
    hC = pr[3]; sC = pr[4]; vC = pr[5]; tC = pr[6];
    // lanes>0 は前レーン終端状態から始まる — penDown は
    // 「全パス penUp 終了」なら up、「全パス down 終了」なら down
    // （コンパイラ証明済み — 混在はコンパイル不可）。属性も
    // 終端一様値があればそれで初期化する。
    // lane0 のみ dispatch 時点の実状態を引き継ぐ。
    uint eBase = 20u + nWb;
    bool lane0 = (iter == 0u);
    penDown = lane0 ? (pr[7] != 0.0) : (pr[eBase] != 0.0);
    if (!lane0) {
        uint am = uint(pr[eBase + 1u]);
        if ((am & 1u) != 0u) penSize = pr[eBase + 2u];
        if ((am & 2u) != 0u) hC = pr[eBase + 3u];
        if ((am & 4u) != 0u) sC = pr[eBase + 4u];
        if ((am & 8u) != 0u) vC = pr[eBase + 5u];
        if ((am & 16u) != 0u) tC = pr[eBase + 6u];
    }
    for (int i = 0; i < 4; ++i) wbMask[i] = 0u;
    for (int i = 0; i < 96; ++i) slots[i] = 0.0;
    updRgba();

    uint codeLen = uint(pr[17]);
    uint pc = 0u;
    for (uint guard = 0u; guard < 200000u; ++guard) {
        if (pc >= codeLen) break;
        uint op = code[pc * 2u], ar = code[pc * 2u + 1u];
        ++pc;
        switch (op) {
        case 0u: writeBack(); return;                    // HALT
        case 1u: push(uintBitsToFloat(ar)); break;       // PUSH
        case 2u: push(float(iter)); break;               // ITER
        case 3u: push(slots[ar]); break;                 // LOADV
        case 4u: slots[ar] = pop(); break;               // STOREV
        case 5u: {                                       // STOREVM
            uint s = ar & 0xFFFFu, w = ar >> 16;
            slots[s] = pop();
            if (w < 128u) wbMask[w / 32u] |= 1u << (w % 32u);
        } break;
        case 6u: push(globs[ar]); break;                 // LOADG
        case 7u: push(posX); break;
        case 8u: push(posY); break;
        case 9u:  { float b = pop(), a = pop(); push(a + b); } break;
        case 10u: { float b = pop(), a = pop(); push(a - b); } break;
        case 11u: { float b = pop(), a = pop(); push(a * b); } break;
        case 12u: { float b = pop(), a = pop(); push(a / b); } break;
        case 13u: { float b = pop(), a = pop();
            push(a - b * floor(a / b)); } break;         // MOD (floored)
        case 14u: { float b = pop(), a = pop(); push(a < b ? 1.0 : 0.0); } break;
        case 15u: { float b = pop(), a = pop(); push(a > b ? 1.0 : 0.0); } break;
        case 16u: { float b = pop(), a = pop(); push(a == b ? 1.0 : 0.0); } break;
        case 17u: { float b = pop(), a = pop(); push(a <= b ? 1.0 : 0.0); } break;
        case 18u: { float b = pop(), a = pop(); push(a >= b ? 1.0 : 0.0); } break;
        case 19u: { float b = pop(), a = pop(); push(a != b ? 1.0 : 0.0); } break;
        case 20u: { float b = pop(), a = pop();
            push((a != 0.0 && b != 0.0) ? 1.0 : 0.0); } break;
        case 21u: { float b = pop(), a = pop();
            push((a != 0.0 || b != 0.0) ? 1.0 : 0.0); } break;
        case 22u: push(pop() == 0.0 ? 1.0 : 0.0); break; // NOT
        case 23u: push(-pop()); break;                   // NEG
        case 24u: {                                      // MATH
            float a = pop();
            float r;
            switch (ar) {
            case 0u: r = abs(a); break;
            case 1u: r = floor(a); break;
            case 2u: r = ceil(a); break;
            case 3u: r = sqrt(a); break;
            case 4u: r = sin(radians(a)); break;
            case 5u: r = cos(radians(a)); break;
            case 6u: r = tan(radians(a)); break;
            case 7u: r = degrees(asin(clamp(a,-1.0,1.0))); break;
            case 8u: r = degrees(acos(clamp(a,-1.0,1.0))); break;
            case 9u: r = degrees(atan(a)); break;
            case 10u: r = log(a); break;
            case 11u: r = log(a) / log(10.0); break;
            case 12u: r = exp(a); break;
            case 13u: r = pow(10.0, a); break;
            case 14u: r = floor(a + 0.5); break;          // ROUND
            default: r = 0.0; break;
            }
            push(r);
        } break;
        case 25u: {                                      // RAND
            float to = pop(), from = pop();
            float lo = min(from, to), hi = max(from, to);
            float r;
            if (floor(lo) == lo && floor(hi) == hi &&
                lo > -2147483648.0 && hi < 2147483648.0)
                r = lo + floor(rnd() * (hi - lo + 1.0));
            else r = lo + rnd() * (hi - lo);
            push(r);
        } break;
        case 26u: pc = ar; break;                        // JMP
        case 27u: if (pop() == 0.0) pc = ar; break;      // JMPIF0
        case 28u: { float y = pop(), x = pop(); gotoXY(x, y);
            flags |= 64u | 128u; } break;                // GOTOXY: X+Y
        case 29u: gotoXY(pop(), posY); flags |= 64u; break;   // SETX
        case 30u: gotoXY(posX, pop()); flags |= 128u; break;  // SETY
        case 31u: gotoXY(posX + pop(), posY); flags |= 64u; break;
        case 32u: gotoXY(posX, posY + pop()); flags |= 128u; break;
        case 33u:                                        // PENDOWN
            if (!penDown) {          // CPU: up→down 遷移のみ打刻
                penDown = true;
                stroke(posX, posY, posX, posY);
            }
            flags |= 4u;
            break;
        case 34u: penDown = false; flags |= 4u; break;   // PENUP
        case 35u: {                                      // PENSIZE
            float sv = pop();
            penSize = pr[19] != 0.0 ? clamp(sv, 1.0, 1200.0)
                                    : max(sv, 0.0);
            flags |= 2u | 256u;
        } break;
        case 36u: {                                      // PENSIZED
            float sv = pop();
            penSize = pr[19] != 0.0 ? clamp(penSize + sv, 1.0, 1200.0)
                                    : max(penSize + sv, 0.0);
            flags |= 2u | 256u;
        } break;
        case 37u: {                                      // PENCOLN
            float n = pop();
            // cast::toRgbColorObject → decimalToRgb と同じ
            // （alpha ビットは a>0 なら透明度、0 なら不透明）
            float nn = n < 0.0 ? floor(4294967296.0 + n) : floor(n);
            uint ci = uint(nn);
            uint a = (ci >> 24) & 0xFFu;
            vec3 hsv = rgb2hsv(vec3(float((ci >> 16) & 0xFFu),
                                    float((ci >> 8) & 0xFFu),
                                    float(ci & 0xFFu)));
            hC = hsv.x / 3.6; sC = hsv.y * 100.0; vC = hsv.z * 100.0;
            tC = (a > 0u && a < 255u)
                 ? 100.0 * (1.0 - float(a) / 255.0) : 0.0;
            shC = vC / 2.0;   // shade は PENCOLN 時点の brightness/2
            updRgba();
            flags |= 2u | 16u | 512u | 1024u | 2048u | 4096u;
        } break;
        case 38u:                                        // PENCOLP (set)
        case 39u: {                                      // PENCOLPB (add)
            float v = pop();
            bool chg = op == 39u;
            if (ar == 0u)      hC = wrapC(v + (chg ? hC : 0.0));
            else if (ar == 1u) sC = clamp(v + (chg ? sC : 0.0), 0.0, 100.0);
            else if (ar == 2u) vC = clamp(v + (chg ? vC : 0.0), 0.0, 100.0);
            else               tC = clamp(v + (chg ? tC : 0.0), 0.0, 100.0);
            updRgba();
            flags |= 2u | (512u << ar);
        } break;
        default: guarded = true; writeBack(); return;   // 未知 op
        }
    }
    // ガード上限到達 = 実行未完（無限ループ級の TIMES 等）
    guarded = true;
    writeBack();
}
)GLSL";

// レコード → クアッド展開 VS（SSBO から直接読み、頂点属性なし）。
// gl_VertexID: rec = vi/6（レーン×maxPerLane のスライス）、
// 角は {0,1,2,2,1,3} の三角形順（CPU バッチと同一レイアウト）。
const char* kGpuRecVs = R"GLSL(#version 430 core
layout(std430, binding=2) readonly buffer R { vec4 recs[]; };
layout(std430, binding=3) readonly buffer N { uint cnts[]; };
layout(std430, binding=6) readonly buffer I { uvec2 ids[]; };
uniform vec2 uSize;
uniform vec2 uHalf;
uniform float uScale;
uniform uint uMaxPerLane;
uniform bool uOrdered;
out vec2 vUV;
out vec2 vMeta;
out vec4 vColor;
void main() {
    uint vi = uint(gl_VertexID);
    uint rec = vi / 6u;
    const int cm[6] = int[6](0, 1, 2, 2, 1, 3);
    int vert = cm[vi % 6u];
    uint lane, slot;
    if (uOrdered) {
        // 逐次順に compact 化された draw-id (stroke, lane) → lane-major
        // レコード位置へ写像
        uvec2 oid = ids[rec];
        lane = oid.y;
        slot = oid.x;
    } else {
        lane = rec / uMaxPerLane;
        slot = rec % uMaxPerLane;
    }
    if (slot >= cnts[lane]) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        vUV = vec2(0.0); vMeta = vec2(0.0); vColor = vec4(0.0);
        return;
    }
    uint rec3 = (lane * uMaxPerLane + slot) * 3u;
    vec4 g = recs[rec3];
    vec4 m = recs[rec3 + 1u];
    float a = recs[rec3 + 2u].x;
    float d = m.x;
    float off = (d == 1.0 || d == 3.0) ? 0.5 : 0.0;
    float ax = (g.x + off + uHalf.x) * uScale;
    float ay = (uHalf.y - (g.y + off)) * uScale;
    float fdx = (g.z - g.x) * uScale;
    float fdy = (g.w - g.y) * uScale;
    float dx = fdx, dy = -fdy;
    float L = length(vec2(fdx, fdy));
    float eps = 1e-3;
    float basisX = (abs(dx) < eps && abs(dy) < eps) ? eps : dx;
    float denom = max(L, eps);
    vec2 n = vec2(basisX / denom, dy / denom);
    float radius = d * 0.5;
    float mm = radius + 1.0;
    int i = vert & 1, j = vert >> 1;
    float u = i == 0 ? -mm : L + mm;
    float v = j == 0 ? -mm : mm;
    vec2 pos = vec2(ax, ay) + u * n + v * vec2(-n.y, n.x);
    gl_Position = vec4(pos.x / uSize.x * 2.0 - 1.0,
                       1.0 - pos.y / uSize.y * 2.0, 0.0, 1.0);
    vUV = vec2(u, v);
    vMeta = vec2(L, radius);
    vColor = vec4(m.y, m.z, m.w, a);
}
)GLSL";

const char* kGpuRecFs = R"GLSL(#version 430 core
in vec2 vUV;
in vec2 vMeta;
in vec4 vColor;
out vec4 frag;
void main() {
    float u = clamp(vUV.x, 0.0, vMeta.x);
    float dist = length(vec2(vUV.x - u, vUV.y));
    float cov = clamp(vMeta.y + 0.5 - dist, 0.0, 1.0);
    float sa = clamp(vColor.a, 0.0, 1.0) * cov;
    vec3 c = clamp(vColor.rgb, vec3(0.0), vec3(1.0));
    frag = vec4(c * sa, sa);
}
)GLSL";

// レーン major のレコードを逐次実行順（lane, stroke の 64bit 論理順序
// id 昇順 — 自然に lane-major なので一般ソート不要）に compact な
// draw-id stream へ並べる。1 レーン = 1 呼出しで ids[off+stroke] に
// uvec2(stroke, lane) を書く（GPU→CPU readback なし）。
const char* kGpuOrdSrc = R"GLSL(#version 430 core
layout(local_size_x=64) in;
layout(std430, binding=3) readonly buffer N { uint cnts[]; };
layout(std430, binding=7) readonly buffer F { uint offs[]; };
layout(std430, binding=6) writeonly buffer I { uvec2 ids[]; };
uniform uint uLanes;
void main() {
    uint lane = gl_GlobalInvocationID.x;
    if (lane >= uLanes) return;
    uint n = cnts[lane] & 0x7FFFFFFFu;
    uint o = offs[lane];
    for (uint k = 0u; k < n; ++k) ids[o + k] = uvec2(k, lane);
}
)GLSL";

GLuint gpuCompile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "gpu shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint gpuLink(GLuint s) {
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "gpu link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

} // namespace

bool SdlRenderer::initGlCompute() {
    glComp_ = new GlCompute();
    GlCompute& c = *glComp_;
    if (SDL_GL_MakeCurrent(window_, glPen_->ctx) != 0) return false;
    // GL 4.3+ 必須（compute shader + SSBO）
    GLint maj = 0, mnr = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &mnr);
    if (maj < 4 || (maj == 4 && mnr < 3) || !glDispatchCompute) {
        if (std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "gpu exec: GL %d.%d < 4.3\n", maj, mnr);
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
        return false;
    }
    GLuint s = gpuCompile(GL_COMPUTE_SHADER, kGpuVmSrc);
    if (s) {
        c.cs = gpuLink(s);
        glDeleteShader(s);
    }
    GLuint vs = gpuCompile(GL_VERTEX_SHADER, kGpuRecVs);
    GLuint fs = gpuCompile(GL_FRAGMENT_SHADER, kGpuRecFs);
    if (vs && fs) {
        c.rp = glCreateProgram();
        glAttachShader(c.rp, vs);
        glAttachShader(c.rp, fs);
        glLinkProgram(c.rp);
        GLint ok = GL_FALSE;
        glGetProgramiv(c.rp, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(c.rp); c.rp = 0; }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    GLuint os = gpuCompile(GL_COMPUTE_SHADER, kGpuOrdSrc);
    if (os) {
        c.ordCs = gpuLink(os);
        glDeleteShader(os);
    }
    if (!c.cs || !c.rp || !c.ordCs) {
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
        return false;
    }
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &c.ssboMax);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &c.maxGroupsX);
    c.uSize = glGetUniformLocation(c.rp, "uSize");
    c.uHalf = glGetUniformLocation(c.rp, "uHalf");
    c.uScale = glGetUniformLocation(c.rp, "uScale");
    c.uMax = glGetUniformLocation(c.rp, "uMaxPerLane");
    c.uOrd = glGetUniformLocation(c.rp, "uOrdered");
    c.ordULanes = glGetUniformLocation(c.ordCs, "uLanes");
    glGenBuffers(1, &c.paramsB);
    glGenBuffers(1, &c.globalsB);
    glGenBuffers(1, &c.codeB);
    glGenBuffers(1, &c.recsB);
    glGenBuffers(1, &c.cntsB);
    glGenBuffers(1, &c.loutB);
    glGenBuffers(1, &c.idsB);
    glGenBuffers(1, &c.offsB);
    if (glGenVertexArrays) {
        glGenVertexArrays(1, &c.vao);
    }
    c.ok = true;
    if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    return true;
}

bool SdlRenderer::gpuExecParallel(gpu::ExecRequest& req) {
    req.executed = false;
    req.laneOut = nullptr;
    req.overflow = false;
    req.nRecords = 0;
    if (std::getenv("XYO_PEN_CPU")) return false;

    // ---- 要求検証（GPU 確保・dispatch の前に全て拒否）----
    if (!req.code || req.codeLen == 0 || (req.codeLen & 1) ||
        req.codeLen > 2u * 65536u) return false;
    if (req.nSlots > 96 || req.nWb > 128) return false;
    if (req.lanes > (1u << 24) || req.maxPerLane > 2048) return false;
    {
        std::string vw;
        if (!gpu::validateBytecode(req.code, req.codeLen / 2, req.nSlots,
                                   req.nGlobals, req.nWb, &vw))
            return false;
    }
    if (req.nWb && !req.wbSlots) return false;
    for (uint32_t i = 0; i < req.nWb; ++i)
        if (!req.wbSlots || req.wbSlots[i] >= req.nSlots) return false;
    if (!req.lanes || !req.maxPerLane) return false;
    const uint32_t stride = gpu::laneOutStride(req.nWb);
    // uint64 演算でオーバーフロー安全にサイズを検証
    const uint64_t codeBytes = uint64_t(req.codeLen) * 4;
    const uint64_t paramBytes = uint64_t(20 + req.nWb + 7) * 4;
    const uint64_t globBytes =
        uint64_t(std::max<uint32_t>(req.nGlobals, 1)) * 4;
    const uint64_t cntBytes = uint64_t(req.lanes) * 4;
    const uint64_t loutBytes = uint64_t(req.lanes) * stride * 4;
    const uint64_t maxRecs = uint64_t(req.lanes) * req.maxPerLane;
    const uint64_t recBytes = maxRecs * 48;          // vec4*3
    const uint64_t idsBytes = maxRecs * 8;           // uvec2
    const uint64_t offsBytes = cntBytes;
    const uint64_t totalBytes = codeBytes + paramBytes + globBytes +
                                cntBytes + loutBytes + recBytes +
                                idsBytes + offsBytes;
    if (totalBytes > uint64_t(512) * 1024 * 1024) return false;
    if (maxRecs * 6 > uint64_t(std::numeric_limits<GLsizei>::max()) / 2)
        return false;   // 頂点数上限（余裕を持って拒否）
    if (req.nGlobals && !req.globals) return false;

    penEnsure();
    if (!penGpu()) return false;
    if (!glComp_ && !initGlCompute()) return false;
    if (!glComp_->ok) return false;
    GlCompute& c = *glComp_;
    GlPen& g = *glPen_;

    const uint32_t groups = (req.lanes + 63) / 64;
    if (c.ssboMax > 0 &&
        int64_t(std::max(std::max(recBytes, loutBytes),
                         std::max(std::max(idsBytes, codeBytes),
                                  std::max(std::max(cntBytes, globBytes),
                                           std::max(paramBytes,
                                                    offsBytes))))) >
            c.ssboMax)
        return false;
    if (c.maxGroupsX > 0 && groups > uint32_t(c.maxGroupsX))
        return false;

    penFlushGpu();   // 先行ストロークをレーン順で先に描画

    if (SDL_GL_MakeCurrent(window_, g.ctx) != 0) return false;
    while (glGetError() != GL_NO_ERROR) {}   // 既存 GL エラー掃除
    auto restore = [&] {
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    };
    gpuLaneOut_.resize(size_t(req.lanes) * stride);

    // params（固定20 + wbSlot[] + 終端状態7）
    std::vector<float> pr(20 + req.nWb + 7, 0.0f);
    pr[0] = req.startX; pr[1] = req.startY;
    pr[2] = req.penSize;
    pr[3] = req.penHSV[0]; pr[4] = req.penHSV[1];
    pr[5] = req.penHSV[2]; pr[6] = req.penHSV[3];
    pr[7] = req.penDown ? 1.0f : 0.0f;
    pr[8] = req.fenceL; pr[9] = req.fenceR;
    pr[10] = req.fenceB; pr[11] = req.fenceT;
    pr[12] = float(req.nSlots);
    pr[13] = float(req.nWb);
    pr[14] = float(req.maxPerLane);
    pr[15] = float(req.lanes);         // 範囲外レーンのガード用
    {   // baseSeed は u32 ビットパターンで渡す（float 化の精度損失防止）
        float fb;
        std::memcpy(&fb, &req.baseSeed, 4);
        pr[16] = fb;
    }
    pr[17] = float(req.codeLen / 2);   // 命令数（ペア）
    pr[18] = float(stride);
    pr[19] = req.miscLimits ? 1.0f : 0.0f;
    for (uint32_t i = 0; i < req.nWb; ++i)
        pr[20 + i] = float(req.wbSlots[i]);
    // lanes>0 の初期状態（全終了経路が一様と証明されたもののみ）
    {
        uint32_t eb = 20 + req.nWb;
        pr[eb] = req.penDownAll ? 1.0f : 0.0f;
        pr[eb + 1] = float(req.endAttrMask);
        for (int k = 0; k < 5; ++k) pr[eb + 2 + k] = req.endAttr[k];
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.paramsB);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(paramBytes),
                 pr.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.globalsB);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(globBytes),
                 req.globals, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.codeB);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(codeBytes),
                 req.code, GL_STREAM_DRAW);

    // 出力バッファ確保（容量が足りなければ再確保）。
    // 総量は 512MB 上限済み — チャンク分割は未実装（単一 dispatch のみ）。
    auto ensure = [&](GLuint buf, uint64_t need, size_t& cap) {
        if (cap < need) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
            glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(need),
                         nullptr, GL_STREAM_DRAW);
            cap = size_t(need);
        }
    };
    ensure(c.recsB, recBytes, c.recCap);
    ensure(c.cntsB, cntBytes, c.cntCap);
    ensure(c.loutB, loutBytes, c.loutCap);
    ensure(c.idsB, idsBytes, c.idsCap);
    ensure(c.offsB, offsBytes, c.offsCap);
    c.cntReadback.resize(req.lanes);
    if (glGetError() != GL_NO_ERROR) {
        // 確保失敗を容量キャッシュが隠さないよう全リセット
        c.recCap = c.cntCap = c.loutCap = c.idsCap = c.offsCap = 0;
        restore();
        return false;
    }

    glUseProgram(c.cs);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, c.paramsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, c.codeB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c.recsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, c.cntsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, c.loutB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, c.globalsB);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);

    // 溢れ・ガード検査 — 描画前に失敗判定する。
    // cnts[lane] = recN | (guarded ? 0x80000000)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.cntsB);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       GLsizeiptr(cntBytes), c.cntReadback.data());
    // 排他的レーンオフセット（排順 id = (lane<<32)|stroke の natural
    // 順序 — 一般ソート不要）。guarded/超過レーンはここで拒否。
    c.ordOffsets.resize(req.lanes);
    uint64_t totalRecords = 0;
    bool countsOk = gpu::penOrderOffsets(c.cntReadback.data(), req.lanes,
                                         req.maxPerLane,
                                         c.ordOffsets.data(),
                                         &totalRecords);
    req.overflow = !countsOk;
    if (!countsOk && std::getenv("XYO_DEBUG"))
        std::fprintf(stderr, "gpu exec: stroke overflow/guard\n");
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.loutB);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       GLsizeiptr(loutBytes), gpuLaneOut_.data());
    if (!countsOk || glGetError() != GL_NO_ERROR) {
        restore();
        return false;
    }

    // draw-id stream の実体化: 1 レーン = 1 呼出しで
    // ids[offs[lane]+stroke] = uvec2(stroke, lane)。readback なし。
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.offsB);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(offsBytes),
                    c.ordOffsets.data());
    glUseProgram(c.ordCs);
    glUniform1ui(c.ordULanes, req.lanes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, c.cntsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, c.idsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, c.offsB);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    if (glGetError() != GL_NO_ERROR) { restore(); return false; }

    // レコード描画: VS が compact な draw-id stream 経由で
    // 逐次順にレコードを読みクアッド展開（実レコード数のみ — パッド分
    // は描画しない）。
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, penW_, penH_);
    glUseProgram(c.rp);
    glUniform2f(c.uSize, float(penW_), float(penH_));
    glUniform2f(c.uHalf, float(stageW_ / 2), float(stageH_ / 2));
    glUniform1f(c.uScale, float(penScale_));
    glUniform1ui(c.uMax, req.maxPerLane);
    glUniform1i(c.uOrd, 1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c.recsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, c.cntsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, c.idsB);
    if (c.vao) glBindVertexArray(c.vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        restore();
        return false;
    }
    if (totalRecords > 0)
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(totalRecords * 6));
    if (glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        restore();
        return false;
    }
    g.fboDirty = true;
    g.sdlTexDirty = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    req.nRecords = uint32_t(std::min<uint64_t>(totalRecords, UINT32_MAX));
    req.laneOut = gpuLaneOut_.data();
    restore();
    req.executed = true;
    return true;
}

namespace {

const char* kNativeDetSrc = R"GLSL(#version 430 core
layout(local_size_x=64) in;
layout(std430, binding=4) buffer O { uint ob[]; };
uniform uint uTotal;
uniform uint uColOff;
uniform uint uLast;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uTotal) return;
    uint cur = ob[uColOff + i];
    uint prev = (i == 0u) ? uLast : ob[uColOff + i - 1u];
    if (cur != prev) atomicMin(ob[1], i);
}
)GLSL";

const char* kNativeRowSrc = R"GLSL(#version 430 core
layout(local_size_x=64) in;
layout(std430, binding=2) writeonly buffer R { vec4 recs[]; };
layout(std430, binding=3) writeonly buffer N { uint cnts[]; };
layout(std430, binding=4) buffer O { uint ob[]; };
uniform uint uCols;
uniform uint uRows;
uniform uint uCap;
uniform uint uColOff;
uniform float uStepX;
uniform float uStepY;
uniform vec4 uBrush;
uniform vec4 uFence;
uniform uint uLast;

vec4 coldec_(uint bits) {
    float f = uintBitsToFloat(bits);
    uint ci = !isnan(f) && !isinf(f) && f >= 0.0 && f < 4294967296.0
                  ? uint(f) : 0u;
    uint a = (ci >> 24u) & 0xFFu;
    vec3 c = vec3(float((ci >> 16u) & 0xFFu),
                  float((ci >> 8u) & 0xFFu), float(ci & 0xFFu)) / 255.0;
    float al = (a > 0u && a < 255u) ? float(a) / 255.0 : 1.0;
    return vec4(c, al);
}
uint colbits_(uint i) { return ob[uColOff + i]; }
float fx_(float x) { return clamp(x, uFence.x, uFence.y); }
float fy_(float y) { return clamp(y, uFence.z, uFence.w); }

void stroke_(uint row, uint n, float x0, float y0, float x1, float y1,
             vec4 rgba, float size) {
    if (n >= uCap) { atomicOr(ob[0], 16u); return; }
    uint i = (row * uCap + n) * 3u;
    recs[i] = vec4(x0, y0, x1, y1);
    recs[i + 1u] = vec4(size, rgba.r, rgba.g, rgba.b);
    recs[i + 2u] = vec4(rgba.a, 0.0, 0.0, 0.0);
}

void main() {
    uint row = gl_GlobalInvocationID.x;
    if (row >= uRows) return;
    float x0 = uintBitsToFloat(ob[uColOff - 2u]);
    float y0 = uintBitsToFloat(ob[uColOff - 1u]);
    float size = uintBitsToFloat(ob[uColOff - 10u]);
    uint base = row * uCols;
    uint firstCh = ob[1];
    vec4 brush =
        (row == 0u || base < firstCh) ? uBrush : coldec_(colbits_(base - 1u));
    float y = fy_(y0 + float(row) * uStepY);
    uint n = 0u;
    float sx = fx_(x0);
    stroke_(row, n, sx, y, sx, y, brush, size);
    ++n;
    uint prev = (base == 0u) ? uLast : colbits_(base - 1u);
    float px = x0;
    for (uint j = 0u; j < uCols; ++j) {
        uint c = colbits_(base + j);
        if (c != prev) {
            float x = x0 + float(j) * uStepX;
            stroke_(row, n, fx_(px), y, fx_(x), y, brush, size);
            ++n;
            brush = coldec_(c);
            px = x;
            prev = c;
        }
    }
    stroke_(row, n, fx_(px), y, fx_(x0 + float(uCols) * uStepX), y, brush,
            size);
    ++n;
    cnts[row] = n;
}
)GLSL";

std::string nativeVariant(const std::string& src, int capture) {
    size_t nl = src.find('\n');
    if (nl == std::string::npos) return src;
    std::string out = src.substr(0, nl + 1);
    out += "#define XYO_NATIVE_CAPTURE ";
    out += capture ? '1' : '0';
    out += '\n';
    out += src.substr(nl + 1);
    return out;
}

} // namespace

bool SdlRenderer::penUploadGpu() {
    return false;
}

bool SdlRenderer::gpuExecNative(gpu::NativeRequest& req) {
    if (std::getenv("XYO_PEN_CPU")) return false;
    if (const char* g0 = std::getenv("XYO_GPU"))
        if (g0[0] == '0' && g0[1] == '\0') return false;
    if (!req.program || !req.vars || !req.cols || !req.rows) return false;
    const gpu::NativeProgram& np = *req.program;
    penEnsure();
    if (!penGpu()) return false;
    if (!glComp_ && !initGlCompute()) return false;
    if (!glComp_->ok) return false;
    GlCompute& c = *glComp_;
    GlPen& g = *glPen_;

    penFlushGpu();

    if (SDL_GL_MakeCurrent(window_, g.ctx) != 0) return false;
    while (glGetError() != GL_NO_ERROR) {}
    auto restore = [&]() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    };

    const bool timing =
        c.timCount < 8 && std::getenv("XYO_GPU_TIMING");
    auto tnow = []() { return std::chrono::steady_clock::now(); };
    auto tms = [](std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto tTotal = tnow();
    if (timing && !c.timQ[0]) glGenQueries(4, c.timQ);
    const bool useQ = timing && c.timQ[0] != 0;
    double cpuPrograms = 0, cpuUpload = 0, cpuFastStatus = 0,
           cpuRowStatus = 0, cpuOutputs = 0;
    auto tPh = tnow();

    const uint64_t total64 = uint64_t(req.cols) * req.rows;
    if (total64 == 0 || total64 > 0xFFFFFFFFull ||
        !std::isfinite(req.res) || req.res <= 0.0f) {
        restore();
        return false;
    }
    const uint32_t total = uint32_t(total64);
    const uint32_t nWb = uint32_t(np.writtenVars.size());
    const bool path = np.path;
    if (path && req.penDown) {
        restore();
        return false;
    }
    const uint32_t recPerLane =
        path ? gpu::kNativePathMaxPerLane : req.cols + 2;
    const uint32_t drawLanes = path ? total : req.rows;
    const uint32_t colOff = 2 + nWb + 10;
    const uint64_t obInts64 = uint64_t(colOff) + total;
    if (obInts64 > 0xFFFFFFFFull) {
        restore();
        return false;
    }
    const uint32_t obInts = uint32_t(obInts64);
    const bool hasAccum =
        !np.accumVars[0].id.empty() || !np.accumVars[1].id.empty() ||
        !np.accumVars[2].id.empty();

    GLint64 maxSsbo = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsbo);
    GLint maxGroups = 0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxGroups);
    if (np.sharedStack) {
        GLint smem = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &smem);
        if (smem < 32768) {
            restore();
            return false;
        }
    }
    auto fits = [&](uint64_t bytes) {
        return maxSsbo > 0 && bytes <= uint64_t(maxSsbo);
    };
    const uint64_t recBytes = uint64_t(drawLanes) * recPerLane * 3 * 16;
    const uint64_t obBytes = uint64_t(obInts) * 4;
    const uint64_t accumBytes = uint64_t(total) * 16;
    const uint64_t paramBytes = uint64_t(gpu::kNativeParamBase + req.nVars) * 4;
    const uint64_t metaBytes = uint64_t(req.nLists) * 8;
    const uint64_t leBytes = uint64_t(req.listElems) * 8;
    const uint64_t totalAlloc = recBytes + obBytes + paramBytes + metaBytes +
                                leBytes + uint64_t(std::max(drawLanes, total)) * 4 +
                                (hasAccum ? accumBytes * 2 : 0);
    if (!fits(recBytes) || !fits(obBytes) ||
        !fits(paramBytes) || !fits(metaBytes) || !fits(leBytes) ||
        (hasAccum && !fits(accumBytes)) ||
        totalAlloc > uint64_t(512) * 1024 * 1024 ||
        uint64_t(drawLanes) * recPerLane * 6 >
            uint64_t(std::numeric_limits<GLsizei>::max()) ||
        (total + 63) / 64 > uint32_t(maxGroups)) {
        restore();
        return false;
    }

    auto getProgs = [&]() -> GlCompute::NativeProgs* {
        auto it = c.nprogs.find(np.source);
        if (it != c.nprogs.end()) return &it->second;
        if (c.nprogOrder.size() >= 32) {
            auto& old = c.nprogs[c.nprogOrder.front()];
            if (old.fast) glDeleteProgram(old.fast);
            if (old.capture) glDeleteProgram(old.capture);
            c.nprogs.erase(c.nprogOrder.front());
            c.nprogOrder.pop_front();
        }
        GlCompute::NativeProgs p{};
        std::string s0 = nativeVariant(np.source, 0);
        std::string s1 = nativeVariant(np.source, 1);
        GLuint cs0 = gpuCompile(GL_COMPUTE_SHADER, s0.c_str());
        GLuint cs1 = gpuCompile(GL_COMPUTE_SHADER, s1.c_str());
        if (cs0) {
            p.fast = gpuLink(cs0);
            glDeleteShader(cs0);
        }
        if (cs1) {
            p.capture = gpuLink(cs1);
            glDeleteShader(cs1);
        }
        if (!p.fast || !p.capture) {
            if (p.fast) glDeleteProgram(p.fast);
            if (p.capture) glDeleteProgram(p.capture);
            return nullptr;
        }
        p.uBaseFast = glGetUniformLocation(p.fast, "uBasePixel");
        p.uDispFast = glGetUniformLocation(p.fast, "uDispatchPixels");
        p.uColsFast = glGetUniformLocation(p.fast, "uCols");
        p.uTotalFast = glGetUniformLocation(p.fast, "uTotal");
        p.uSeedFast = glGetUniformLocation(p.fast, "uSeed");
        p.uBaseCap = glGetUniformLocation(p.capture, "uBasePixel");
        p.uDispCap = glGetUniformLocation(p.capture, "uDispatchPixels");
        p.uColsCap = glGetUniformLocation(p.capture, "uCols");
        p.uTotalCap = glGetUniformLocation(p.capture, "uTotal");
        p.uSeedCap = glGetUniformLocation(p.capture, "uSeed");
        auto r = c.nprogs.emplace(np.source, p);
        c.nprogOrder.push_back(np.source);
        return &r.first->second;
    };
    if (timing) tPh = tnow();
    GlCompute::NativeProgs* progs = getProgs();
    if (!progs) { restore(); return false; }

    if (!path) {
        if (!c.detCs) {
            GLuint s = gpuCompile(GL_COMPUTE_SHADER, kNativeDetSrc);
            if (s) { c.detCs = gpuLink(s); glDeleteShader(s); }
            if (!c.detCs) { restore(); return false; }
            c.detUTotal = glGetUniformLocation(c.detCs, "uTotal");
            c.detUColOff = glGetUniformLocation(c.detCs, "uColOff");
            c.detULast = glGetUniformLocation(c.detCs, "uLast");
        }
        if (!c.rowCs) {
            GLuint s = gpuCompile(GL_COMPUTE_SHADER, kNativeRowSrc);
            if (s) { c.rowCs = gpuLink(s); glDeleteShader(s); }
            if (!c.rowCs) { restore(); return false; }
            c.rowUCols = glGetUniformLocation(c.rowCs, "uCols");
            c.rowURows = glGetUniformLocation(c.rowCs, "uRows");
            c.rowUCap = glGetUniformLocation(c.rowCs, "uCap");
            c.rowUColOff = glGetUniformLocation(c.rowCs, "uColOff");
            c.rowUStepX = glGetUniformLocation(c.rowCs, "uStepX");
            c.rowUStepY = glGetUniformLocation(c.rowCs, "uStepY");
            c.rowUBrush = glGetUniformLocation(c.rowCs, "uBrush");
            c.rowUFence = glGetUniformLocation(c.rowCs, "uFence");
            c.rowULast = glGetUniformLocation(c.rowCs, "uLast");
        }
    }
    if (timing) { cpuPrograms = tms(tPh, tnow()); tPh = tnow(); }

    if (!c.nParamsB) {
        glGenBuffers(1, &c.nParamsB);
        glGenBuffers(1, &c.nMetaB);
        glGenBuffers(1, &c.nObB);
        glGenBuffers(1, &c.nLeB);
        glGenBuffers(1, &c.nAiB);
        glGenBuffers(1, &c.nAoB);
    }
    auto ensure = [&](GLuint buf, uint64_t need, size_t& cap) {
        if (cap < need) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
            glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(need),
                         nullptr, GL_STREAM_DRAW);
            cap = size_t(need);
        }
    };
    ensure(c.nParamsB, paramBytes, c.nParamCap);
    ensure(c.nMetaB, std::max<uint64_t>(metaBytes, 8), c.nMetaCap);
    ensure(c.nObB, obBytes, c.nObCap);
    ensure(c.nLeB, std::max<uint64_t>(leBytes, 8), c.nLeCap);
    ensure(c.recsB, recBytes, c.recCap);
    ensure(c.cntsB, uint64_t(std::max(drawLanes, total)) * 4, c.cntCap);
    if (hasAccum) {
        ensure(c.nAiB, accumBytes, c.nAiCap);
        ensure(c.nAoB, accumBytes, c.nAoCap);
    }

    if (c.nSceneProg != req.program || c.nScene != req.sceneVersion) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nMetaB);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        GLsizeiptr(metaBytes), req.listMeta);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nLeB);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        GLsizeiptr(leBytes), req.listData);
        c.nScene = req.sceneVersion;
        c.nSceneProg = req.program;
    }

    std::vector<float> pr(gpu::kNativeParamBase + req.nVars);
    pr[0] = float(req.cols);
    pr[1] = float(req.rows);
    pr[2] = req.res;
    pr[3] = float(total);
    std::memcpy(&pr[4], &req.baseSeed, 4);
    pr[5] = req.fenceL; pr[6] = req.fenceR;
    pr[7] = req.fenceB; pr[8] = req.fenceT;
    pr[9] = req.startX; pr[10] = req.startY;
    pr[11] = req.penSize;
    pr[12] = req.penRGBA[0]; pr[13] = req.penRGBA[1];
    pr[14] = req.penRGBA[2]; pr[15] = req.penRGBA[3];
    pr[16] = req.penDown ? 1.0f : 0.0f;
    pr[17] = req.lastColor;
    pr[18] = req.miscLimits ? 1.0f : 0.0f;
    pr[19] = req.accumReset ? 1.0f : 0.0f;
    pr[20] = float(req.accumLens[0]); pr[21] = float(req.accumLens[1]);
    pr[22] = float(req.accumLens[2]); pr[23] = float(req.accumLens[3]);
    pr[24] = float(recPerLane);
    pr[25] = float(req.nVars);
    std::memcpy(&pr[gpu::kNativeParamBase], req.vars, req.nVars * 4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nParamsB);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(paramBytes),
                    pr.data());

    if (hasAccum) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nAiB);
        if (req.accumIn) {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            GLsizeiptr(accumBytes), req.accumIn);
            c.nAccumValid = true;
        } else if (!c.nAccumValid || req.accumReset) {
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_RGBA32F,
                              GL_RGBA, GL_FLOAT, nullptr);
            c.nAccumValid = true;
        }
    }

    {
        uint32_t init[2] = {0u, 0xFFFFFFFFu};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nObB);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 8, init);
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, c.nParamsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, c.nMetaB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c.recsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, c.cntsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, c.nObB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, c.nLeB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, c.nAiB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, c.nAoB);
    if (timing) cpuUpload = tms(tPh, tnow());

    glUseProgram(progs->fast);
    glUniform1ui(progs->uBaseFast, 0u);
    glUniform1ui(progs->uDispFast, total);
    glUniform1ui(progs->uColsFast, req.cols);
    glUniform1ui(progs->uTotalFast, total);
    glUniform1ui(progs->uSeedFast, req.baseSeed);
    if (useQ) glBeginQuery(GL_TIME_ELAPSED, c.timQ[0]);
    glDispatchCompute((total + 63) / 64, 1, 1);
    if (useQ) glEndQuery(GL_TIME_ELAPSED);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);

    glUseProgram(progs->capture);
    glUniform1ui(progs->uBaseCap, total - 1);
    glUniform1ui(progs->uDispCap, 1u);
    glUniform1ui(progs->uColsCap, req.cols);
    glUniform1ui(progs->uTotalCap, total);
    glUniform1ui(progs->uSeedCap, req.baseSeed);
    if (useQ) glBeginQuery(GL_TIME_ELAPSED, c.timQ[1]);
    glDispatchCompute(1, 1, 1);
    if (useQ) glEndQuery(GL_TIME_ELAPSED);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);

    if (timing) tPh = tnow();
    uint32_t status[8];
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nObB);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(status), status);
    if (timing) cpuFastStatus = tms(tPh, tnow());
    req.overflow = status[0] != 0u;
    if (req.overflow) {
        if (std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "gpu native: err=%08x\n", status[0]);
        restore();
        return false;
    }

    if (!path) {
        uint32_t lastBits;
        std::memcpy(&lastBits, &req.lastColor, 4);
        glUseProgram(c.detCs);
        glUniform1ui(c.detUTotal, total);
        glUniform1ui(c.detUColOff, colOff);
        glUniform1ui(c.detULast, lastBits);
        if (useQ) glBeginQuery(GL_TIME_ELAPSED, c.timQ[2]);
        glDispatchCompute((total + 63) / 64, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                        GL_BUFFER_UPDATE_BARRIER_BIT);

        float sx = np.slotStepX >= 0 ? req.vars[np.slotStepX] : req.res;
        float sy = np.slotStepY >= 0 ? req.vars[np.slotStepY] : req.res;
        glUseProgram(c.rowCs);
        glUniform1ui(c.rowUCols, req.cols);
        glUniform1ui(c.rowURows, req.rows);
        glUniform1ui(c.rowUCap, recPerLane);
        glUniform1ui(c.rowUColOff, colOff);
        glUniform1f(c.rowUStepX, sx);
        glUniform1f(c.rowUStepY, sy);
        glUniform4f(c.rowUBrush, req.penRGBA[0], req.penRGBA[1],
                    req.penRGBA[2], req.penRGBA[3]);
        glUniform4f(c.rowUFence, req.fenceL, req.fenceR, req.fenceB,
                    req.fenceT);
        glUniform1ui(c.rowULast, lastBits);
        glDispatchCompute((req.rows + 63) / 64, 1, 1);
        if (useQ) glEndQuery(GL_TIME_ELAPSED);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                        GL_BUFFER_UPDATE_BARRIER_BIT |
                        GL_VERTEX_SHADER_BIT);

        if (timing) tPh = tnow();
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nObB);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(status),
                           status);
        if (timing) cpuRowStatus = tms(tPh, tnow());
        if (status[0] != 0u) {
            req.overflow = true;
            if (std::getenv("XYO_DEBUG"))
                std::fprintf(stderr, "gpu native: row err=%08x\n",
                             status[0]);
            restore();
            return false;
        }
    }

    if (timing) tPh = tnow();
    c.nScalar.resize(nWb + 11);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nObB);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 8,
                       GLsizeiptr((nWb + 10) * 4), c.nScalar.data());
    {
        uint32_t lc = 0;
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           GLsizeiptr(colOff + total - 1) * 4, 4, &lc);
        float lf;
        std::memcpy(&lf, &lc, 4);
        c.nScalar[nWb + 10] = lf;
    }
    req.scalarOut = c.nScalar.data();
    req.nScalarOut = nWb + 11;

    if (hasAccum) {
        c.nAccum.resize(size_t(total) * 4);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.nAoB);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                           GLsizeiptr(accumBytes), c.nAccum.data());
        req.accumOut = c.nAccum.data();
        std::swap(c.nAiB, c.nAoB);
        std::swap(c.nAiCap, c.nAoCap);
        c.nAccumValid = true;
    }
    if (timing) cpuOutputs = tms(tPh, tnow());

    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, penW_, penH_);
    if (!path) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(c.rp);
    glUniform2f(c.uSize, float(penW_), float(penH_));
    glUniform2f(c.uHalf, float(stageW_ / 2), float(stageH_ / 2));
    glUniform1f(c.uScale, float(penScale_));
    glUniform1ui(c.uMax, recPerLane);
    glUniform1i(c.uOrd, 0);   // native は lane-major パッド描画のまま
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c.recsB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, c.cntsB);
    if (c.vao) glBindVertexArray(c.vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (useQ) glBeginQuery(GL_TIME_ELAPSED, c.timQ[3]);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(drawLanes * recPerLane * 6));
    if (useQ) glEndQuery(GL_TIME_ELAPSED);
    if (glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        restore();
        return false;
    }
    g.fboDirty = true;
    g.sdlTexDirty = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (glGetError() != GL_NO_ERROR) {
        restore();
        return false;
    }

    GLuint64 qns[4] = {0, 0, 0, 0};
    if (useQ) {
        glGetQueryObjectui64v(c.timQ[0], GL_QUERY_RESULT, &qns[0]);
        glGetQueryObjectui64v(c.timQ[1], GL_QUERY_RESULT, &qns[1]);
        if (!path)
            glGetQueryObjectui64v(c.timQ[2], GL_QUERY_RESULT, &qns[2]);
        glGetQueryObjectui64v(c.timQ[3], GL_QUERY_RESULT, &qns[3]);
    }

    restore();
    req.executed = true;
    if (timing) {
        ++c.timCount;
        std::fprintf(stderr,
            "xyo-native-timing gpu_fast_ns=%llu gpu_capture_ns=%llu "
            "gpu_row_ns=%llu gpu_draw_ns=%llu cpu_programs_ms=%.3f "
            "cpu_upload_ms=%.3f cpu_fast_status_ms=%.3f "
            "cpu_row_status_ms=%.3f cpu_outputs_ms=%.3f "
            "cpu_total_ms=%.3f cols=%u rows=%u mode=%s\n",
            (unsigned long long)qns[0], (unsigned long long)qns[1],
            (unsigned long long)qns[2], (unsigned long long)qns[3],
            cpuPrograms, cpuUpload, cpuFastStatus, cpuRowStatus,
            cpuOutputs, tms(tTotal, tnow()), req.cols, req.rows,
            path ? "path" : "ray");
    }
    if (std::getenv("XYO_DEBUG"))
        std::fprintf(stderr, "gpu native: executed %ux%u\n", req.cols,
                     req.rows);
    return true;
}

void SdlRenderer::getFenceRect(int drawableID, double out[4]) const {
    out[0] = -stageW_ / 2.0; out[1] = stageW_ / 2.0;
    out[2] = -stageH_ / 2.0; out[3] = stageH_ / 2.0;
    const Drawable* d = drawableFor(drawableID);
    if (!d || !d->target) return;
    Target* t = d->target;
    double bounds[4] = {t->x, t->x, t->y, t->y};
    if (skinForTarget(t)) getDrawableBounds(drawableID, bounds);
    double inset = std::floor(
        std::min(bounds[1] - bounds[0], bounds[2] - bounds[3]) / 2);
    double sx = stageW_ / 2.0 - std::min(15.0, inset);
    double sy = stageH_ / 2.0 - std::min(15.0, inset);
    // getFencedPosition と同じ式の矩形化:
    // x ∈ [-sx-(boundsR-tx), sx-(boundsL-tx)]  y ∈ [-sy-(boundsT-ty), sy-(boundsB-ty)]
    out[0] = -sx - (bounds[1] - t->x);
    out[1] = sx - (bounds[0] - t->x);
    out[2] = -sy - (bounds[2] - t->y);
    out[3] = sy - (bounds[3] - t->y);
}

#else

bool SdlRenderer::penGpu() const { return false; }
bool SdlRenderer::initGlPen() { return false; }
void SdlRenderer::penFlushGpu() {}
void SdlRenderer::penSyncCpu() {}
void SdlRenderer::penStrokeGpu(const float[4], float, float, float,
                               float, float) {}
bool SdlRenderer::initGlCompute() { return false; }
bool SdlRenderer::penUploadGpu() { return false; }

#endif // XYO_PEN_GL

#if !defined(XYO_PEN_GL)
bool SdlRenderer::gpuExecParallel(gpu::ExecRequest&) { return false; }
bool SdlRenderer::gpuExecNative(gpu::NativeRequest&) { return false; }
void SdlRenderer::getFenceRect(int, double out[4]) const {
    out[0] = -stageW_ / 2.0; out[1] = stageW_ / 2.0;
    out[2] = -stageH_ / 2.0; out[3] = stageH_ / 2.0;
}
#endif

void SdlRenderer::penStroke(const float rgba[4], double diameter,
                            double x0, double y0, double x1, double y1) {
    static const bool penlog = std::getenv("XYO_PENLOG") != nullptr;
    static const bool penprof = std::getenv("XYO_PENPROF") != nullptr;
    static const bool penskip = std::getenv("XYO_PENSKIP") != nullptr;
    if (penskip) return;
    static std::chrono::nanoseconds penAcc{};
    static uint64_t penCalls = 0, penPixels = 0;
    static struct PenProfDump {
        ~PenProfDump() {
            std::fprintf(stderr,
                "PENPROF strokes=%llu pixels=%llu time=%.3fs (%.1fns/px)\n",
                (unsigned long long)penCalls, (unsigned long long)penPixels,
                penAcc.count() / 1e9,
                penPixels ? double(penAcc.count()) / penPixels : 0.0);
        }
    } penProfDump;
    struct PenGuard {
        bool on; std::chrono::steady_clock::time_point t0;
        uint64_t px = 0;
        PenGuard(bool o) : on(o) {
            if (on) t0 = std::chrono::steady_clock::now();
        }
        ~PenGuard() {
            if (!on) return;
            penAcc += std::chrono::steady_clock::now() - t0;
            ++penCalls;
            penPixels += px;
        }
    } penGuard(penprof);
    if (penlog)
        std::fprintf(stderr, "PEN d=%.6f (%.6f,%.6f)->(%.6f,%.6f) a=%.3f\n",
                     diameter, x0, y0, x1, y1, double(rgba[3]));
    double d = diameter;
    if (!(d > 0.0) && !(d < 0.0)) d = 1.0;
    double width = double(static_cast<float>(d));
    if (!std::isfinite(width) || width < 0.0) return;
    double offset = (d == 1.0 || d == 3.0) ? 0.5 : 0.0;
    float fx0 = static_cast<float>(x0 + offset);
    float fy0 = static_cast<float>(y0 + offset);
    float fdx = static_cast<float>(x1 - x0);
    float fdy = static_cast<float>(y1 - y0);
    if (!std::isfinite(fx0) || !std::isfinite(fy0) || !std::isfinite(fdx) ||
        !std::isfinite(fdy)) return;
    double dx = double(fdx), dy = -double(fdy);
    double L = double(static_cast<float>(std::hypot(x1 - x0, y1 - y0)));
    if (!std::isfinite(L)) return;
    if (penGpu()) {
        penStrokeGpu(rgba, static_cast<float>(width), fx0, fy0, fdx, fdy);
        return;
    }
    const double eps = double(static_cast<float>(1e-3));
    double basisX = (std::abs(dx) < eps && std::abs(dy) < eps) ? eps : dx;
    double denom = std::max(L, eps);
    double nx = basisX / denom, ny = dy / denom;
    double nn = nx * nx + ny * ny;
    if (!std::isfinite(nx) || !std::isfinite(ny) || !(nn > 0.0) ||
        !std::isfinite(nn)) return;
    double inv = 1.0 / nn;
    double ax = stageX(double(fx0)), ay = stageY(double(fy0));
    double bx = ax + nx * L, by = ay + ny * L;
    double radius = width * 0.5;
    double support = (radius + 0.5) * std::hypot(nx, ny);
    if (!std::isfinite(inv) || !std::isfinite(bx) || !std::isfinite(by) ||
        !std::isfinite(support)) return;
    double xlo = std::min(ax, bx) - support - 0.5;
    double xhi = std::max(ax, bx) + support - 0.5;
    double ylo = std::min(ay, by) - support - 0.5;
    double yhi = std::max(ay, by) + support - 0.5;
    if (xhi < 0.0 || xlo >= double(penW_) || yhi < 0.0 ||
        ylo >= double(penH_)) return;
    int ix0 = static_cast<int>(std::ceil(std::max(0.0, xlo)));
    int ix1 = static_cast<int>(
        std::floor(std::min(double(penW_ - 1), xhi)));
    int iy0 = static_cast<int>(std::ceil(std::max(0.0, ylo)));
    int iy1 = static_cast<int>(
        std::floor(std::min(double(penH_ - 1), yhi)));
    if (ix0 > ix1 || iy0 > iy1) return;

    if (dx == 0.0 && dy == 0.0 && width <= 8.0) {
        double fracX = ax - std::floor(ax);
        double fracY = ay - std::floor(ay);
        PenKernel* k = nullptr;
        for (PenKernel& e : penKernels_)
            if (e.width == width && e.fx == fracX && e.fy == fracY) {
                k = &e;
                break;
            }
        if (!k) {
            k = &penKernels_[penKernelNext_];
            penKernelNext_ = (penKernelNext_ + 1) % 8;
            k->width = width;
            k->fx = fracX;
            k->fy = fracY;
            k->n = 0;
            for (int oy = -4; oy <= 4; ++oy)
                for (int ox = -4; ox <= 4; ++ox) {
                    double dist = std::hypot(ox + 0.5 - fracX,
                                             oy + 0.5 - fracY);
                    double cov = std::clamp(radius + 0.5 - dist, 0.0, 1.0);
                    if (cov <= 0.0) continue;
                    k->ox[k->n] = static_cast<int8_t>(ox);
                    k->oy[k->n] = static_cast<int8_t>(oy);
                    k->cov[k->n] = static_cast<float>(cov);
                    ++k->n;
                }
        }
        int cx = static_cast<int>(std::floor(ax));
        int cy = static_cast<int>(std::floor(ay));
        for (int i = 0; i < k->n; ++i) {
            int x = cx + k->ox[i], y = cy + k->oy[i];
            if (x < 0 || y < 0 || x >= penW_ || y >= penH_) continue;
            penBlendPremult(
                &penPx_[(static_cast<size_t>(y) * penW_ + x) * 4], rgba,
                k->cov[i]);
        }
        return;
    }

    // --- ストローク単位でホイストしたブレンド定数 ---
    // 被覆率 1 の内部ピクセルでは sa はストローク色 alpha のみで決まる
    // ため、penBlendPremult と同じ式をここで事前計算する。
    const double sa1 = std::clamp(double(rgba[3]), 0.0, 1.0);
    const double om1 = 1.0 - sa1;
    const double srcR = std::clamp(double(rgba[0]), 0.0, 1.0) * sa1 * 255.0;
    const double srcG = std::clamp(double(rgba[1]), 0.0, 1.0) * sa1 * 255.0;
    const double srcB = std::clamp(double(rgba[2]), 0.0, 1.0) * sa1 * 255.0;
    const double sa255 = sa1 * 255.0;
    const bool solid = sa1 >= 1.0;
    const uint8_t sr8 = static_cast<uint8_t>(
        std::lround(std::clamp(double(rgba[0]), 0.0, 1.0) * 255.0));
    const uint8_t sg8 = static_cast<uint8_t>(
        std::lround(std::clamp(double(rgba[1]), 0.0, 1.0) * 255.0));
    const uint8_t sb8 = static_cast<uint8_t>(
        std::lround(std::clamp(double(rgba[2]), 0.0, 1.0) * 255.0));
    // cov>=1 確定ピクセルのブレンド（penBlendPremult(cov=1) と同一式）
    auto blendFull = [&](uint8_t* p) {
        if (solid) {
            p[0] = sr8; p[1] = sg8; p[2] = sb8; p[3] = 255;
            return;
        }
        p[0] = static_cast<uint8_t>(std::clamp(
            std::lround(srcR + p[0] * om1), 0L, 255L));
        p[1] = static_cast<uint8_t>(std::clamp(
            std::lround(srcG + p[1] * om1), 0L, 255L));
        p[2] = static_cast<uint8_t>(std::clamp(
            std::lround(srcB + p[2] * om1), 0L, 255L));
        p[3] = static_cast<uint8_t>(
            std::clamp(std::lround(sa255 + p[3] * om1), 0L, 255L));
    };

    const double D = radius + 0.5;    // cov>0 になる半径
    const double Di = radius - 0.5;   // cov>=1 になる半径
    const double Di2 = Di * Di;

    if (dx == 0.0 && dy == 0.0) {
        // ドット（width>8）: スキャンライン円充填。
        // 行ごとに 1 回の sqrt で横幅を求め、内部ピクセルは
        // 二乗距離比較のみ（hypot 不要）。
        const double D2 = D * D;
        for (int y = iy0; y <= iy1; ++y) {
            const double dyc = y + 0.5 - ay;
            const double dyy = dyc * dyc;
            if (dyy > D2) continue;
            const double hx = std::sqrt(D2 - dyy);
            const int rx0 = static_cast<int>(std::ceil(
                std::max(0.0, ax - hx - 0.5)));
            const int rx1 = static_cast<int>(std::floor(
                std::min(double(penW_ - 1), ax + hx - 0.5)));
            penGuard.px += static_cast<uint64_t>(rx1 - rx0 + 1);
            uint8_t* row =
                &penPx_[(static_cast<size_t>(y) * penW_) * 4];
            for (int x = rx0; x <= rx1; ++x) {
                const double dxc = x + 0.5 - ax;
                const double d2 = dxc * dxc + dyy;
                if (d2 <= Di2) {
                    blendFull(row + static_cast<size_t>(x) * 4);
                    continue;
                }
                const double cov = D - std::sqrt(d2);  // 0<cov<1
                if (cov > 0.0)
                    penBlendPremult(row + static_cast<size_t>(x) * 4,
                                    rgba, cov);
            }
        }
        return;
    }

    // カプセル（線分）: 本体区間 (0<=along<=L) の距離は |perp| のみ。
    // hypot は両端の丸キャップ部にだけ必要。
    for (int y = iy0; y <= iy1; ++y) {
        const double dY = y + 0.5 - ay;
        double dX = ix0 + 0.5 - ax;   // +1 刻みは double で厳密
        penGuard.px += static_cast<uint64_t>(ix1 - ix0 + 1);
        uint8_t* row = &penPx_[(static_cast<size_t>(y) * penW_) * 4];
        for (int x = ix0; x <= ix1; ++x, dX += 1.0) {
            const double along = (nx * dX + ny * dY) * inv;
            const double perp = (-ny * dX + nx * dY) * inv;
            const double al = along - std::clamp(along, 0.0, L);
            const double d2 = al * al + perp * perp;
            // dist <= Di → cov>=1 を二乗比較で先判定（内部ピクセルは
            // sqrt/hypot 不要）
            if (Di >= 0.0 && d2 <= Di2) {
                blendFull(row + static_cast<size_t>(x) * 4);
                continue;
            }
            // 本体区間は dist=|perp|（hypot(0,perp) と厳密に同一）、
            // キャップ部だけ hypot。
            const double dist = (along >= 0.0 && along <= L)
                ? std::fabs(perp) : std::hypot(al, perp);
            const double cov = D - dist;
            if (cov <= 0.0) continue;
            penBlendPremult(row + static_cast<size_t>(x) * 4, rgba,
                            std::min(cov, 1.0));
        }
    }
}

void SdlRenderer::penPoint(int skin, const float rgba[4], double diameter,
                           double x, double y) {
    if (skin != penSkinId_) return;
    penEnsure();
    penStroke(rgba, diameter, x, y, x, y);
    penDirty_ = true;
}

void SdlRenderer::penLine(int skin, const float rgba[4], double diameter,
                          double x0, double y0, double x1, double y1) {
    if (skin != penSkinId_) return;
    penEnsure();
    penStroke(rgba, diameter, x0, y0, x1, y1);
    penDirty_ = true;
}

void SdlRenderer::penClear(int skin) {
    if (skin != penSkinId_) return;
    penEnsure();
#if defined(XYO_PEN_GL)
    if (penGpu()) {
        GlPen& g = *glPen_;
        g.vLen = 0;   // 先行ストロークは消去されるので描画不要
        g.ixLen = 0;
        g.fboDirty = false;
        g.sdlTexDirty = true;
        if (SDL_GL_MakeCurrent(window_, g.ctx) == 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
        }
    }
#endif
    std::fill(penPx_.begin(), penPx_.end(), 0);
    penDirty_ = true;
}

void SdlRenderer::penStamp(int skin, int drawableID) {
    if (skin != penSkinId_) return;
    penEnsure();
    penSyncCpu();   // GPU バッチを先に描画して penPx_ を最新化
    Drawable* d = drawableFor(drawableID);
    if (!d || !d->target || !d->target->visible) return;
    const Skin* s = skinForTarget(d->target);
    if (!s) return;
    double b[4];
    getDrawableBounds(drawableID, b);
    int x0 = static_cast<int>(stageX(std::min(b[0], b[1])));
    int x1 = static_cast<int>(std::ceil(stageX(std::max(b[0], b[1]))));
    int y0 = static_cast<int>(stageY(std::max(b[2], b[3])));
    int y1 = static_cast<int>(std::ceil(stageY(std::min(b[2], b[3]))));
    uint8_t px4[4];
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (!skinPxAt(*d, x + 0.5, y + 0.5, px4)) continue;
            penBlendPx(static_cast<int>((x + 0.5) * penScale_),
                       static_cast<int>((y + 0.5) * penScale_), px4);
        }
#if defined(XYO_PEN_GL)
    // CPU 側のスタンプ結果を FBO へ戻して同期を保つ。
    // penPx_ は top-down、FBO は GL 座標（bottom-up）なので行反転する。
    if (penGpu() && SDL_GL_MakeCurrent(window_, glPen_->ctx) == 0) {
        const size_t row = static_cast<size_t>(penW_) * 4;
        std::vector<uint8_t> flip(row * penH_);
        for (int y = 0; y < penH_; ++y)
            std::memcpy(&flip[static_cast<size_t>(penH_ - 1 - y) * row],
                        &penPx_[static_cast<size_t>(y) * row], row);
        glBindTexture(GL_TEXTURE_2D, glPen_->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, penW_, penH_, GL_RGBA,
                        GL_UNSIGNED_BYTE, flip.data());
        glPen_->sdlTexDirty = true;
        if (sdlGlCtx_) SDL_GL_MakeCurrent(window_, sdlGlCtx_);
    }
#endif
    penDirty_ = true;
}

// ================================================================ 描画 ===

void SdlRenderer::draw() {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, 0x6e, 0x80, 0x8f, 255);
    SDL_RenderClear(renderer_);
    drawStageArea();

    // 描画順: backdrop → pen → sprites（order 昇順）
    std::vector<Drawable*> order;
    order.reserve(drawables_.size());
    for (auto& [id, d] : drawables_) order.push_back(&d);
    std::sort(order.begin(), order.end(), [](const Drawable* a,
                                             const Drawable* b) {
        return a->order < b->order;
    });
    for (Drawable* d : order)
        if (d->group == "backdrop") drawDrawable(*d);
    drawPenLayer();
    for (Drawable* d : order)
        if (d->group == "sprite") drawDrawable(*d);
    for (Drawable* d : order)
        if (d->group == "sprite" && d->target && d->target->visible &&
            !d->target->bubbleText.empty())
            drawBubble(d->target);
    // Scratch ではモニタはステージ canvas の上に載る HTML オーバーレイ
    // なので、green flag/stop や ask バーより後に描く。
    drawAskBar();
    drawUiChrome();
    drawMonitors();
    if (frameCapture_) frameCapture_();
    SDL_RenderPresent(renderer_);
}

static SDL_Rect sdlRect(const IRect& r) {
    return {r.x, r.y, r.w, r.h};
}

void SdlRenderer::drawStageArea() {
    SDL_Rect r = sdlRect(stageRect_);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer_, &r);
}

void SdlRenderer::drawDrawable(Drawable& d) {
    Target* t = d.target;
    if (!t) return;
    if (d.group == "sprite" && !t->visible) return;
    Skin* s = skinForTarget(t);
    if (!s || !s->tex) return;
    DrawXform f;
    if (!xformFor(d, *s, f)) return;
    Rendered r;
    const Rendered* rd = renderedFor(t, s, &r);
    if (!rd->tex) return;

    double scr = stageRect_.w / double(stageW_);      // window px / 論理 px
    double k = (t->size / 100.0) / s->scale * scr;    // tex px → window px
    double winCx = stageRect_.x + f.cx * scr;
    double winCy = stageRect_.y + f.cy * scr;
    SDL_Rect dst{
        static_cast<int>(std::lround(winCx - s->rcx * s->scale * k)),
        static_cast<int>(std::lround(winCy - s->rcy * s->scale * k)),
        static_cast<int>(std::lround(rd->w * k)),
        static_cast<int>(std::lround(rd->h * k)),
    };
    if (dst.w <= 0 || dst.h <= 0) return;

    double angle = 0;
    SDL_RendererFlip flip = SDL_FLIP_NONE;
    if (!t->isStage && t->rotationStyle == "all around")
        angle = t->direction - 90.0;
    else if (!t->isStage && t->rotationStyle == "left-right" &&
             t->direction < 0)
        flip = SDL_FLIP_HORIZONTAL;
    SDL_Point center{static_cast<int>(s->rcx * s->scale * k),
                     static_cast<int>(s->rcy * s->scale * k)};
    SDL_RenderCopyEx(renderer_, rd->tex, nullptr, &dst, angle, &center,
                     flip);
}

void SdlRenderer::drawPenLayer() {
    if (!penTex_) return;
#if defined(XYO_PEN_GL)
    if (!penUploadGpu())
#endif
    {
        penSyncCpu();   // GPU バッチが残っていれば FBO へ描画し penPx_ へ戻す
    }
    if (penDirty_) {
        for (size_t i = 0; i < penPx_.size(); i += 4) {
            uint8_t a = penPx_[i + 3];
            uint8_t* q = &penUploadPx_[i];
            if (a == 255) { std::memcpy(q, &penPx_[i], 4); continue; }
            if (a == 0) { std::memset(q, 0, 4); continue; }
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<uint8_t>(
                    std::min(255, (penPx_[i + c] * 255 + a / 2) / a));
            q[3] = a;
        }
        SDL_UpdateTexture(penTex_, nullptr, penUploadPx_.data(), penW_ * 4);
        penDirty_ = false;
    }
    SDL_Rect r = sdlRect(stageRect_);
    SDL_RenderCopy(renderer_, penTex_, nullptr, &r);
}

SDL_Texture* SdlRenderer::textTexture(const std::u16string& s, int px,
                                      uint8_t r, uint8_t g, uint8_t b,
                                      int* w, int* h, bool bold) {
    std::string key = SStr::toUtf8(s);
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "|%d|%02x%02x%02x|%d", px, r, g, b,
                  bold ? 1 : 0);
    key += suffix;
    auto it = textCache_.find(key);
    if (it != textCache_.end()) {
        *w = it->second.w;
        *h = it->second.h;
        return it->second.tex;
    }
    FontRenderer* fr = FontRenderer::get();
    std::vector<uint8_t> bmp;
    int tw = 0, th = 0;
    if (!fr->render(s, px, r, g, b, bmp, tw, th, bold)) return nullptr;
    SDL_Surface* surf =
        SDL_CreateRGBSurfaceWithFormatFrom(bmp.data(), tw, th, 32, tw * 4,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    textCache_[key] = {tex, tw, th};
    *w = tw;
    *h = th;
    return tex;
}

void SdlRenderer::drawBubble(Target* t) {
    FontRenderer* fr = FontRenderer::get();
    if (!fr->ok()) return;
    double scr = stageRect_.w / double(stageW_);
    int px = std::max(10, static_cast<int>(12 * scr));
    int tw = 0, th = 0;
    SDL_Texture* tex = textTexture(t->bubbleText, px, 0x2e, 0x2e, 0x2e, &tw, &th);
    if (!tex) return;
    // バブルはスプライト bounds の上に配置
    double b[4];
    int id = drawableOfTarget(t);
    getDrawableBounds(id, b);
    double anchorX = stageX(
        std::max(b[0], std::min(b[1], stageW_ / 2.0 - 60)));
    double anchorY = stageY(std::max(b[2], b[3]));
    int padX = static_cast<int>(8 * scr), padY = static_cast<int>(6 * scr);
    int bw = tw + 2 * padX, bh = th + 2 * padY;
    int bx = static_cast<int>(stageRect_.x + anchorX * scr);
    int by = static_cast<int>(stageRect_.y + (anchorY - bh / scr - 8) * scr);
    if (bx + bw > stageRect_.x + stageRect_.w)
        bx = stageRect_.x + stageRect_.w - bw;
    if (bx < stageRect_.x) bx = stageRect_.x;
    if (by < stageRect_.y) by = stageRect_.y;
    // 本体（角丸）+ しっぽ
    SDL_Rect box{bx, by, bw, bh};
    roundedBox(renderer_, box, static_cast<int>(8 * scr),
               255, 255, 255, 255, 0x94, 0xa0, 0xae, 255);
    if (t->bubbleType == "say") {
        SDL_Point tail[3] = {
            {bx + bw / 5, by + bh},
            {bx + bw / 5 + static_cast<int>(12 * scr), by + bh},
            {bx + bw / 8, by + bh + static_cast<int>(10 * scr)},
        };
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        for (int dy = 0; dy < static_cast<int>(8 * scr); ++dy)
            SDL_RenderDrawLine(renderer_, tail[0].x + dy / 2, tail[0].y + dy,
                               tail[1].x - dy / 2, tail[1].y + dy);
    } else {   // think — 円2つ
        int r1 = static_cast<int>(3 * scr), r2 = static_cast<int>(5 * scr);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_Rect c1{bx + bw / 4 - r1, by + bh + static_cast<int>(4 * scr),
                    2 * r1, 2 * r1};
        SDL_Rect c2{bx + bw / 4 - r2 + static_cast<int>(8 * scr),
                    by + bh - r2 / 2, 2 * r2, 2 * r2};
        SDL_RenderFillRect(renderer_, &c2);
        SDL_RenderFillRect(renderer_, &c1);
        SDL_SetRenderDrawColor(renderer_, 0x94, 0xa0, 0xae, 255);
        SDL_RenderDrawRect(renderer_, &c1);
    }
    SDL_Rect dt{bx + padX, by + padY, tw, th};
    SDL_RenderCopy(renderer_, tex, nullptr, &dt);
}

namespace {

/// scratch-gui opcode-labels.js のラベル表（モニタ表示用）。
/// data_variable / data_listcontents は params から名前を取る。
std::u16string lowerU16(std::u16string v) {
    for (char16_t& c : v)
        if (c >= u'A' && c <= u'Z') c = char16_t(c - u'A' + u'a');
    return v;
}

std::u16string monitorOpcodeLabel(
    const std::string& opcode,
    const std::map<std::string, std::u16string>& params) {
    auto param = [&](const char* k) -> std::u16string {
        auto it = params.find(k);
        return it == params.end() ? std::u16string() : it->second;
    };
    if (opcode == "data_variable") return param("VARIABLE");
    if (opcode == "data_listcontents") return param("LIST");
    if (opcode == "motion_direction") return u"direction";
    if (opcode == "motion_xposition") return u"x position";
    if (opcode == "motion_yposition") return u"y position";
    if (opcode == "looks_size") return u"size";
    if (opcode == "looks_costumenumbername")
        return lowerU16(param("NUMBER_NAME")) == u"number" ? u"costume number"
                                                           : u"costume name";
    if (opcode == "looks_backdropnumbername")
        return lowerU16(param("NUMBER_NAME")) == u"number"
                   ? u"backdrop number"
                   : u"backdrop name";
    if (opcode == "looks_backdropname") return u"backdrop name";
    if (opcode == "sound_volume") return u"volume";
    if (opcode == "sound_tempo") return u"tempo";
    if (opcode == "sensing_answer") return u"answer";
    if (opcode == "sensing_loudness") return u"loudness";
    if (opcode == "sensing_online") return u"online";
    if (opcode == "sensing_username") return u"username";
    if (opcode == "sensing_timer") return u"timer";
    if (opcode == "sensing_current") {
        const std::u16string m = lowerU16(param("CURRENTMENU"));
        if (m == u"year") return u"year";
        if (m == u"month") return u"month";
        if (m == u"date") return u"date";
        if (m == u"dayofweek") return u"day of week";
        if (m == u"hour") return u"hour";
        if (m == u"minute") return u"minute";
        if (m == u"second") return u"second";
        return u"";
    }
    // 未知 opcode は最初の param（従来動作）
    for (auto& [k, v] : params) {
        (void)k;
        return v;
    }
    return std::u16string();
}

/// モニタの値チップ色（scratch-gui color-mode default）: カテゴリ色。
void monitorChipColor(const std::string& opcode, uint8_t out[3]) {
    if (opcode.rfind("data_listcontents", 0) == 0 || opcode == "data_listcontents") {
        out[0] = 0xFF; out[1] = 0x66; out[2] = 0x1A; return;   // data_lists
    }
    if (opcode.rfind("motion_", 0) == 0) { out[0]=0x4C; out[1]=0x97; out[2]=0xFF; return; }
    if (opcode.rfind("looks_", 0) == 0) { out[0]=0x99; out[1]=0x66; out[2]=0xFF; return; }
    if (opcode.rfind("sound_", 0) == 0) { out[0]=0xCF; out[1]=0x63; out[2]=0xCF; return; }
    if (opcode.rfind("sensing_", 0) == 0) { out[0]=0x5C; out[1]=0xB1; out[2]=0xD6; return; }
    if (opcode.rfind("operator_", 0) == 0) { out[0]=0x59; out[1]=0xC0; out[2]=0x59; return; }
    if (opcode.rfind("pen_", 0) == 0) { out[0]=0x0F; out[1]=0xBD; out[2]=0x8C; return; }
    out[0] = 0xFF; out[1] = 0x8C; out[2] = 0x1A;   // data（変数）
}

// scratch-gui（monitor.css / monitor.jsx / color-mode default）の色。
constexpr uint8_t kMonBg[3] = {0xE5, 0xF0, 0xFF};            // $ui-primary
constexpr uint8_t kMonText[3] = {0x57, 0x5E, 0x75};          // $text-primary
constexpr uint8_t kMonVar[3] = {0xFF, 0x8C, 0x1A};           // data
constexpr uint8_t kMonList[3] = {0xFF, 0x66, 0x1A};          // data_lists
constexpr uint8_t kMonBorder[4] = {0, 0, 0, 38};             // rgba(0,0,0,.15)

/// scratch-gui monitor-adapter.js 相当: 数値は toFixed(6) → Number →
/// 文字列（リスト項目は丸めない — GUI 側は item.toString() のみ）。
std::u16string monitorText(Value v, bool round6) {
    if (round6 && v.isNumber()) {
        const double d = v.asNumberUnchecked();
        if (!std::isfinite(d) || std::fabs(d) >= 1e21)
            return jsnum::toString16(d);   // JS toFixed の指数フォールバック
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", d);
        v = Value::numCanonical(std::strtod(buf, nullptr));
    }
    SStr* s = cast::toString(v);
    return s ? std::u16string(s->view()) : std::u16string();
}

} // namespace

/// 変数・リストモニタ。scratch-gui の monitor.css / monitor.jsx
/// （light color mode）に合わせる:
///   default : [太字ラベル #575E75] [値チップ #FF8C1A + 白文字]
///   large   : カテゴリ色の箱 + 白 16px の値だけ
///   slider  : default + range 入力
///   list    : 白ヘッダ（下線・中央太字）/ 本体 #E5F0FF / 行 24px
///             （太字 index + 値チップ #FF661A + 白文字 + 1px 枠）/ 白フッタ
/// コンテナは #E5F0FF + 1px rgba(0,0,0,.15) + radius 4（$space/2）。
void SdlRenderer::drawMonitors() {
    FontRenderer* fr = FontRenderer::get();
    if (!fr->ok()) return;

    const double scr = stageRect_.w / double(stageW_);
    auto S = [&](double v) {
        return static_cast<int>(std::lround(v * scr));
    };
    const int fontPx = std::max(8, S(12));     // 0.75rem
    const int largePx = std::max(9, S(16));    // 1rem
    const int rad = std::max(1, S(4));         // $space / 2
    const int pad = std::max(1, S(3));         // container / header padding
    const int mar = std::max(1, S(5));         // label / value の margin
    const int rowH = std::max(8, S(24));       // .list-row 高さ
    const int chipH = std::max(6, S(22));      // .list-value 高さ
    const int innerPad = std::max(1, S(3));    // .value-inner padding
    const int lineH = std::max(8, fr->lineHeight(fontPx));

    sliderRects_.clear();
    sliderTravel_.clear();

    // FontRenderer に太字が無いので 1px 重ねで近似する。
    auto blit = [&](SDL_Texture* t, int x, int y, int w, int h) {
        if (!t) return;
        SDL_Rect d{x, y, w, h};
        SDL_RenderCopy(renderer_, t, nullptr, &d);
    };
    // 上端 / 下端だけ角丸に塗る（リストのヘッダ・フッタ）
    auto fillTopRounded = [&](const SDL_Rect& r, int radius, uint8_t cr,
                              uint8_t cg, uint8_t cb) {
        fillRoundedRect(renderer_, r, radius, cr, cg, cb, 255);
        SDL_SetRenderDrawColor(renderer_, cr, cg, cb, 255);
        SDL_Rect lower{r.x, r.y + radius, r.w, r.h - radius};
        SDL_RenderFillRect(renderer_, &lower);
    };
    auto fillBottomRounded = [&](const SDL_Rect& r, int radius, uint8_t cr,
                                 uint8_t cg, uint8_t cb) {
        fillRoundedRect(renderer_, r, radius, cr, cg, cb, 255);
        SDL_SetRenderDrawColor(renderer_, cr, cg, cb, 255);
        SDL_Rect upper{r.x, r.y, r.w, r.h - radius};
        SDL_RenderFillRect(renderer_, &upper);
    };

    for (auto& [id, rec] : rt_->monitorState.records) {
        if (!rec.visible || rec.id.empty()) continue;

        // ラベル = "Sprite1: name" or "name"（opcode 依存 — opcode-labels 準拠）
        const std::u16string name = monitorOpcodeLabel(rec.opcode, rec.params);
        const std::u16string label =
            rec.spriteName.empty() ? name : rec.spriteName + u": " + name;
        uint8_t chip[3];
        monitorChipColor(rec.opcode, chip);
        const int mx0 = stageRect_.x + static_cast<int>(rec.x * scr);
        const int my0 = stageRect_.y + static_cast<int>(rec.y * scr);

        if (rec.mode == "list") {
            // ---- リストモニタ ----
            std::u16string listName;
            if (auto it = rec.params.find("LIST"); it != rec.params.end())
                listName = it->second;
            SList* list = nullptr;
            for (Target* t : rt_->targets) {
                if (!t->sprite) continue;
                if (!rec.targetId.empty() && t->id != rec.targetId &&
                    !(rec.spriteName.empty() && t->isStage))
                    continue;
                if (Variable* var =
                        t->lookupVariableByNameAndType(listName, "list")) {
                    list = var->list;
                    break;
                }
            }
            const uint32_t rows = list ? list->len : 0;
            const int w = S(rec.width > 0 ? rec.width : 110);
            const int h = S(rec.height > 0 ? rec.height : 200);
            const int mx =
                std::clamp(mx0, stageRect_.x,
                           std::max(stageRect_.x,
                                    stageRect_.x + stageRect_.w - w));
            const int my =
                std::clamp(my0, stageRect_.y,
                           std::max(stageRect_.y,
                                    stageRect_.y + stageRect_.h - h));
            SDL_Rect box{mx, my, w, h};
            roundedBox(renderer_, box, rad, kMonBg[0], kMonBg[1], kMonBg[2],
                       255, kMonBorder[0], kMonBorder[1], kMonBorder[2],
                       kMonBorder[3]);

            // ヘッダ（白・下線・中央太字）
            int hw = 0, hh = 0;
            SDL_Texture* ht =
                textTexture(label, fontPx, kMonText[0], kMonText[1],
                            kMonText[2], &hw, &hh, true);
            const int band = std::max(hh + 2 * pad, S(22));
            SDL_Rect hdr{mx + 1, my + 1, w - 2, band};
            fillTopRounded(hdr, std::max(1, rad - 1), 0xFF, 0xFF, 0xFF);
            SDL_SetRenderDrawColor(renderer_, kMonBorder[0], kMonBorder[1],
                                   kMonBorder[2], kMonBorder[3]);
            SDL_RenderDrawLine(renderer_, hdr.x, hdr.y + band - 1,
                               hdr.x + hdr.w - 1, hdr.y + band - 1);
            blit(ht, mx + (w - hw) / 2, my + 1 + (band - hh) / 2, hw, hh);

            // フッタ（白）: 左 "+" / 中央 "length N" / 右 "="
            SDL_Rect ftr{mx + 1, my + h - 1 - band, w - 2, band};
            fillBottomRounded(ftr, std::max(1, rad - 1), 0xFF, 0xFF, 0xFF);
            char lenBuf[32];
            std::snprintf(lenBuf, sizeof(lenBuf), "length %u", rows);
            std::u16string lenText;
            for (const char* p = lenBuf; *p; ++p)
                lenText.push_back(static_cast<char16_t>(*p));
            int lw = 0, lh = 0;
            SDL_Texture* ltx =
                textTexture(lenText, fontPx, kMonText[0], kMonText[1],
                            kMonText[2], &lw, &lh, true);
            blit(ltx, mx + (w - lw) / 2, ftr.y + (band - lh) / 2, lw, lh);
            int pw = 0, ph = 0;
            SDL_Texture* plus = textTexture(u"+", fontPx, kMonText[0],
                                            kMonText[1], kMonText[2], &pw,
                                            &ph, true);
            blit(plus, mx + pad + 1, ftr.y + (band - ph) / 2, pw, ph);
            int ew = 0, eh = 0;
            SDL_Texture* eq = textTexture(u"=", fontPx, kMonText[0],
                                          kMonText[1], kMonText[2], &ew, &eh,
                                          true);
            blit(eq, mx + w - pad - 1 - ew, ftr.y + (band - eh) / 2, ew, eh);

            // 本体: 行 24px、はみ出す分はクリップ + スクロールバー
            SDL_Rect body{mx + 1, my + 1 + band, w - 2, h - 2 - 2 * band};
            SDL_RenderSetClipRect(renderer_, &body);
            if (rows == 0) {
                int ew2 = 0, eh2 = 0;
                SDL_Texture* et =
                    textTexture(u"(empty)", fontPx, kMonText[0], kMonText[1],
                                kMonText[2], &ew2, &eh2);
                blit(et, mx + (w - ew2) / 2, body.y + pad, ew2, eh2);
            }
            const int visible = std::max(1, body.h / rowH);
            const bool scroll = int(rows) > visible;
            const int sbW = scroll ? std::max(2, S(8)) : 0;
            for (int i = 0; i < visible && i < int(rows); ++i) {
                const int ry = body.y + i * rowH;
                char tmp[16];
                std::snprintf(tmp, sizeof(tmp), "%d", i + 1);
                std::u16string idx;
                for (const char* p = tmp; *p; ++p)
                    idx.push_back(static_cast<char16_t>(*p));
                int iw = 0, ih = 0;
                SDL_Texture* itx =
                    textTexture(idx, fontPx, kMonText[0], kMonText[1],
                                kMonText[2], &iw, &ih, true);
                blit(itx, body.x + S(2) + innerPad, ry + (rowH - ih) / 2, iw,
                     ih);
                const int vx = body.x + S(2) + innerPad + iw + 2 * S(3);
                const int vw2 =
                    std::max(S(40), body.x + body.w - sbW - S(2) - vx -
                                        S(3));
                SDL_Rect vb{vx, ry + (rowH - chipH) / 2, vw2, chipH};
                roundedBox(renderer_, vb, rad, chip[0], chip[1], chip[2],
                           255, kMonBorder[0], kMonBorder[1], kMonBorder[2],
                           kMonBorder[3]);
                const std::u16string item =
                    list ? monitorText(list->items[i], false)
                         : std::u16string();
                int tw = 0, th = 0;
                SDL_Texture* tt =
                    textTexture(item, fontPx, 0xFF, 0xFF, 0xFF, &tw, &th);
                SDL_Rect clip{vb.x + 1, vb.y, std::max(1, vb.w - 2), vb.h};
                SDL_RenderSetClipRect(renderer_, &clip);
                blit(tt, vb.x + innerPad + 1, vb.y + (chipH - th) / 2,
                     std::min(tw, std::max(1, vb.w - 2 * innerPad - 2)), th);
                SDL_RenderSetClipRect(renderer_, &body);
            }
            SDL_RenderSetClipRect(renderer_, nullptr);
            if (scroll) {
                const int trackX = body.x + body.w - sbW;
                SDL_Rect tr{trackX, body.y, sbW, body.h};
                SDL_SetRenderDrawColor(renderer_, 0xCE, 0xCD, 0xCE, 255);
                SDL_RenderFillRect(renderer_, &tr);
                const int thumbH =
                    std::max(S(12), body.h * visible / int(rows));
                SDL_Rect tb{trackX, body.y, sbW, thumbH};
                SDL_SetRenderDrawColor(renderer_, 0x9E, 0x9D, 0x9E, 255);
                SDL_RenderFillRect(renderer_, &tb);
            }
            continue;
        }

        if (rec.mode == "large") {
            // ---- 大表示: カテゴリ色の箱 + 白 16px の値のみ ----
            const std::u16string val = monitorText(rec.value, true);
            int vw = 0, vh = 0;
            SDL_Texture* vt = textTexture(val, largePx, 0xFF, 0xFF, 0xFF,
                                          &vw, &vh);
            const int w = std::max(S(48) + S(8), vw + S(8));
            const int h = std::max(S(22), vh + S(4));
            SDL_Rect box{mx0, my0, w, h};
            roundedBox(renderer_, box, rad, chip[0], chip[1], chip[2], 255,
                       kMonBorder[0], kMonBorder[1], kMonBorder[2],
                       kMonBorder[3]);
            blit(vt, mx0 + (w - vw) / 2, my0 + (h - vh) / 2, vw, vh);
            continue;
        }

        // ---- 通常 / スライダ ----
        const std::u16string val = monitorText(rec.value, true);
        int lw = 0, lh = 0, vw = 0, vh = 0;
        SDL_Texture* lt = textTexture(label, fontPx, kMonText[0],
                                      kMonText[1], kMonText[2], &lw, &lh,
                                      true);
        SDL_Texture* vt =
            textTexture(val, fontPx, 0xFF, 0xFF, 0xFF, &vw, &vh);
        const bool slider = rec.mode == "slider";
        const int rowHH = std::max({lh, vh, lineH});
        const int chipW = std::max(vw + 2 * S(2), S(40));
        const int sliderRow = slider ? std::max(S(18), S(16)) : 0;
        const int w = pad + mar + lw + 2 * mar + chipW + mar + pad;
        const int h = pad + rowHH + sliderRow + pad;
        SDL_Rect box{mx0, my0, w, h};
        roundedBox(renderer_, box, rad, kMonBg[0], kMonBg[1], kMonBg[2], 255,
                   kMonBorder[0], kMonBorder[1], kMonBorder[2],
                   kMonBorder[3]);
        blit(lt, mx0 + pad + mar, my0 + pad + (rowHH - lh) / 2, lw, lh);
        const int chipX = mx0 + pad + mar + lw + 2 * mar;
        fillRoundedRect(renderer_, SDL_Rect{chipX, my0 + pad, chipW, rowHH},
                        rad, chip[0], chip[1], chip[2], 255);
        blit(vt, chipX + (chipW - vw) / 2, my0 + pad + (rowHH - vh) / 2, vw,
             vh);
        if (slider) {
            // Scratch の slider は素の <input type=range>（ブラウザ既定の
            // 見た目）: 灰色トラック + 青いノブ。
            const int knobR = std::max(3, S(7));
            const int trackH = std::max(2, S(4));
            const int trackX = mx0 + pad + knobR;
            const int trackW = std::max(1, w - 2 * pad - 2 * knobR);
            const int cy = my0 + pad + rowHH + sliderRow / 2;
            fillRoundedRect(renderer_,
                            SDL_Rect{trackX, cy - trackH / 2, trackW, trackH},
                            trackH / 2, 0xD3, 0xD3, 0xD3, 255);
            const double range = rec.sliderMax - rec.sliderMin;
            double frac = range != 0
                              ? (cast::toNumber(rec.value) - rec.sliderMin) /
                                    range
                              : 0;
            if (rec.isDiscrete) {
                const double step = range != 0 ? 1.0 / range : 0;
                frac = step != 0 ? std::round(frac / step) * step : frac;
            }
            frac = std::clamp(frac, 0.0, 1.0);
            const int kx = trackX + static_cast<int>(frac * (trackW - 1));
            fillDisc(renderer_, kx, cy, knobR, 0x4C, 0x97, 0xFF, 255);
            if (sliderVar_) fillDisc(renderer_, kx, cy, knobR, 0x42, 0x80,
                                     0xD7, 255);
            sliderRects_[rec.id] = IRect{mx0 + pad, cy - sliderRow / 2,
                                         w - 2 * pad, sliderRow};
            sliderTravel_[rec.id] = {trackX, trackW};
        }
    }
}

// ---- ask and wait 入力バー（ステージ下端のオーバレイ）-------------------

namespace {
constexpr double kAskBarH = 40.0;   // 論理 px
}

static int askBarHeightPx(const IRect& stageRect, int stageH) {
    double scr = stageRect.h / double(stageH);
    return static_cast<int>(kAskBarH * scr);
}

bool SdlRenderer::askHitTest(int mx, int my) const {
    if (!askActive_) return false;
    int bh = askBarHeightPx(stageRect_, stageH_);
    return mx >= stageRect_.x && mx < stageRect_.x + stageRect_.w &&
           my >= stageRect_.y + stageRect_.h - bh &&
           my < stageRect_.y + stageRect_.h;
}

void SdlRenderer::askSubmit() {
    std::u16string ans = askInput_;
    askActive_ = false;
    askInput_.clear();
    askCaret_ = 0;
    SDL_StopTextInput();
    rt_->emitAnswer(ans);   // 次の質問があれば onQuestion で再度アクティブ化
}

void SdlRenderer::drawAskBar() {
    if (!askActive_) return;
    double scr = stageRect_.h / double(stageH_);
    int barH = askBarHeightPx(stageRect_, stageH_);
    SDL_Rect bar{stageRect_.x, stageRect_.y + stageRect_.h - barH,
                 stageRect_.w, barH};
    SDL_SetRenderDrawColor(renderer_, 0x4a, 0x54, 0x64, 235);
    SDL_RenderFillRect(renderer_, &bar);
    // 上端の区切り線
    SDL_SetRenderDrawColor(renderer_, 0x2e, 0x36, 0x44, 255);
    SDL_RenderDrawLine(renderer_, bar.x, bar.y, bar.x + bar.w - 1, bar.y);

    FontRenderer* fr = FontRenderer::get();
    if (!fr->ok()) return;
    int px = std::max(10, static_cast<int>(13 * scr));
    int pad = static_cast<int>(8 * scr);
    int fx = bar.x + pad;
    int fw = bar.w - 2 * pad;
    if (!askQuestion_.empty()) {
        int tw = 0, th = 0;
        SDL_Texture* q =
            textTexture(askQuestion_, px, 0xe6, 0xf0, 0xff, &tw, &th);
        if (q) {
            int qw = std::min(tw, fw * 3 / 5);
            SDL_Rect qs{0, 0, qw, th};
            SDL_Rect qd{fx, bar.y + (barH - th) / 2, qw, th};
            SDL_RenderCopy(renderer_, q, &qs, &qd);
            fx += qw + pad;
            fw -= qw + pad;
        }
    }
    // 送信ボタン（右端の緑円 + チェック）
    int btnR = (barH - pad) / 2;
    int bcx = bar.x + bar.w - pad - btnR;
    int bcy = bar.y + barH / 2;
    askSubmitRect_ = IRect{bcx - btnR - 2, bcy - btnR - 2,
                           2 * btnR + 4, 2 * btnR + 4};
    fw -= 2 * btnR + pad;
    // 入力フィールド（丸角白）
    SDL_Rect field{fx, bar.y + pad / 2, fw, barH - pad};
    askFieldRect_ = IRect{field.x, field.y, field.w, field.h};
    roundedBox(renderer_, field, field.h / 3, 255, 255, 255, 255,
               0x8b, 0x95, 0xa5, 255);
    SDL_SetTextInputRect(&field);

    int tx = field.x + static_cast<int>(6 * scr);
    int maxTw = field.w - 2 * static_cast<int>(6 * scr);
    // カーソル位置 = カーソル前方テキスト幅
    int cw = 0, ch2 = 0;
    std::u16string before = askInput_.substr(0, askCaret_);
    textTexture(before, px, 0, 0, 0, &cw, &ch2);
    int tw = 0, th = 0;
    SDL_Texture* it =
        textTexture(askInput_, px, 0x2e, 0x2e, 0x2e, &tw, &th);
    int scroll = std::max(0, cw - maxTw);   // カーソルを常に表示
    if (it) {
        int vw = std::min(tw - scroll, maxTw);
        if (vw > 0) {
            SDL_Rect ss{scroll, 0, vw, th};
            SDL_Rect sd{tx, field.y + (field.h - th) / 2, vw, th};
            SDL_RenderCopy(renderer_, it, &ss, &sd);
        }
    }
    // カーソル（点滅、ブランドカラーで太め）
    if ((SDL_GetTicks() / 530) % 2 == 0) {
        int cx = tx + cw - scroll;
        SDL_Rect caret{cx, field.y + static_cast<int>(4 * scr),
                       std::max(2, static_cast<int>(1.5 * scr)),
                       field.h - 2 * static_cast<int>(4 * scr)};
        SDL_SetRenderDrawColor(renderer_, 0x4c, 0x97, 0xff, 255);
        SDL_RenderFillRect(renderer_, &caret);
    }
    // ✓ ボタン
    {
        int hov = 0;
        { int mx2 = 0, my2 = 0; SDL_GetMouseState(&mx2, &my2);
          const IRect& sr = askSubmitRect_;
          hov = (mx2 >= sr.x && mx2 < sr.x + sr.w &&
                 my2 >= sr.y && my2 < sr.y + sr.h) ? 1 : 0; }
        fillDisc(renderer_, bcx, bcy, btnR, hov ? 0x4f : 0x45,
                 hov ? 0xb5 : 0x9f, hov ? 0x4f : 0x45, 255);
        // チェックマーク
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        int s = btnR;
        for (int i = -std::max(1, s / 8); i <= std::max(1, s / 8); ++i) {
            SDL_RenderDrawLine(renderer_, bcx - s / 2 + i / 2,
                               bcy, bcx - s / 6 + i / 2, bcy + s / 2);
            SDL_RenderDrawLine(renderer_, bcx - s / 6 + i / 2,
                               bcy + s / 2, bcx + s / 2 + i / 2,
                               bcy - s / 3);
        }
    }
}

// ---- スライダモニタ -------------------------------------------------------

bool SdlRenderer::sliderHitTest(int mx, int my) {
    for (auto& [id, r] : sliderRects_) {
        if (mx < r.x || mx >= r.x + r.w || my < r.y || my >= r.y + r.h)
            continue;
        auto it = rt_->monitorState.records.find(id);
        if (it == rt_->monitorState.records.end()) continue;
        const MonitorRecord& rec = it->second;
        // モニタ対象ターゲットの解決（list 描画と同規則）
        Target* mt = nullptr;
        if (!rec.targetId.empty())
            mt = rt_->getTargetById(rec.targetId);
        if (!mt && !rec.spriteName.empty())
            for (Target* t2 : rt_->targets) {
                if (t2->sprite && t2->isOriginal &&
                    t2->getName() == rec.spriteName) {
                    mt = t2;
                    break;
                }
            }
        if (!mt) mt = rt_->getTargetForStage();
        if (!mt) continue;
        std::u16string name;
        for (auto& [pk, pv] : rec.params) { name = pv; break; }
        Variable* var =
            mt->lookupVariableByNameAndType(name, Variable::kScalarType);
        if (!var || var->isCloud) continue;
        sliderVar_ = var;
        sliderMin_ = rec.sliderMin;
        sliderMax_ = rec.sliderMax;
        sliderDiscrete_ = rec.isDiscrete;
        auto tr = sliderTravel_.find(id);
        if (tr != sliderTravel_.end()) {
            sliderX0_ = tr->second.first;
            sliderW_ = tr->second.second;
        } else {
            sliderX0_ = r.x;
            sliderW_ = r.w;
        }
        sliderDragTo(mx);
        return true;
    }
    return false;
}

void SdlRenderer::sliderDragTo(int mx) {
    if (!sliderVar_) return;
    double frac =
        sliderW_ > 0 ? (mx - sliderX0_) / double(sliderW_) : 0.0;
    frac = std::clamp(frac, 0.0, 1.0);
    double v = sliderMin_ + frac * (sliderMax_ - sliderMin_);
    if (sliderDiscrete_) v = std::round(v);
    sliderVar_->value = Value::num(v);
}

void SdlRenderer::drawUiChrome() {
    int hov = -1;
    {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        if (hitChrome(mx, my, hov)) { /* hov set */ }
        else hov = -1;
    }
    int rad = std::max(3, flagRect_[2] / 5);

    // green flag ボタン
    {
        SDL_Rect r{flagRect_[0], flagRect_[1], flagRect_[2], flagRect_[3]};
        uint8_t bg = hov == 0 ? 255 : 235;
        roundedBox(renderer_, r, rad, bg, bg, bg, 235,
                   0x94, 0xa0, 0xae, 200);
        int w = r.w;
        int poleX = r.x + w * 5 / 16, poleY = r.y + w * 3 / 16;
        int poleH = w * 5 / 8, poleW = std::max(2, w / 14);
        // 旗（右向きペナント: ポール右に三角旗＋左下の切れ込み）
        uint8_t fr, fg, fb;
        if (rt_->turboMode) { fr = 0xff; fg = 0xa0; fb = 0x1e; }
        else                { fr = 0x45; fg = 0x9f; fb = 0x45; }
        SDL_SetRenderDrawColor(renderer_, fr, fg, fb, 255);
        int flagH = w * 3 / 8;
        int flagW = w * 7 / 16;
        for (int dy = 0; dy < flagH; ++dy) {
            // 上辺と下辺が右端で尖る三角旗
            int t = dy < flagH / 2 ? dy : flagH - dy;
            int hw = flagW - (flagH / 2 - t) * flagW / (flagH / 2 + 1);
            if (hw < 0) hw = 0;
            SDL_RenderDrawLine(renderer_, poleX + poleW - 1,
                               poleY + dy,
                               poleX + poleW - 1 + hw, poleY + dy);
        }
        // ポール
        SDL_SetRenderDrawColor(renderer_, 0x5a, 0x6a, 0x7e, 255);
        SDL_Rect pole{poleX, poleY, poleW, poleH};
        SDL_RenderFillRect(renderer_, &pole);
        fillDisc(renderer_, poleX + poleW / 2, poleY + poleH, poleW);
    }
    // stop ボタン
    {
        SDL_Rect r{stopRect_[0], stopRect_[1], stopRect_[2], stopRect_[3]};
        uint8_t bg = hov == 1 ? 255 : 235;
        roundedBox(renderer_, r, rad, bg, bg, bg, 235,
                   0x94, 0xa0, 0xae, 200);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        int rad2 = r.w * 5 / 16;
        SDL_SetRenderDrawColor(renderer_, 0xc0, 0x30, 0x30, 255);
        fillOctagon(renderer_, cx, cy, rad2);
        SDL_SetRenderDrawColor(renderer_, 0xec, 0x59, 0x59, 255);
        fillOctagon(renderer_, cx, cy, rad2 - std::max(1, r.w / 18));
    }
}

bool SdlRenderer::hitChrome(int mx, int my, int& which) const {
    if (mx >= flagRect_[0] && mx < flagRect_[0] + flagRect_[2] &&
        my >= flagRect_[1] && my < flagRect_[1] + flagRect_[3]) {
        which = 0;
        return true;
    }
    if (mx >= stopRect_[0] && mx < stopRect_[0] + stopRect_[2] &&
        my >= stopRect_[1] && my < stopRect_[1] + stopRect_[3]) {
        which = 1;
        return true;
    }
    return false;
}

void SdlRenderer::setTitle(const char* title) {
    if (window_ && title) SDL_SetWindowTitle(window_, title);
}

bool SdlRenderer::savePPM(const char* path) {
    if (!renderer_) return false;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer_, &w, &h);
    if (w <= 0 || h <= 0) return false;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGBA32,
                             px.data(), w * 4) != 0)
        return false;
    FILE* f = fopenUtf8(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        fputc(px[i * 4 + 0], f);
        fputc(px[i * 4 + 1], f);
        fputc(px[i * 4 + 2], f);
    }
    std::fclose(f);
    return true;
}

// ================================================================ 入力 ===

void SdlRenderer::updateCursor(int mx, int my) {
    SDL_Cursor* want = curArrow_;
    if (askActive_) {
        const IRect& f = askFieldRect_, &s = askSubmitRect_;
        if (mx >= s.x && mx < s.x + s.w && my >= s.y && my < s.y + s.h)
            want = curHand_;
        else if (mx >= f.x && mx < f.x + f.w && my >= f.y &&
                 my < f.y + f.h)
            want = curIbeam_;
    }
    if (want == curArrow_) {
        int w2;
        if (hitChrome(mx, my, w2)) want = curHand_;
        else {
            for (auto& [id, r] : sliderRects_)
                if (mx >= r.x && mx < r.x + r.w && my >= r.y &&
                    my < r.y + r.h) {
                    want = curHand_;
                    break;
                }
        }
    }
    if (want == curArrow_ && (dragTarget_ || sliderVar_))
        want = curHand_;
    else if (want == curArrow_) {
        int id = pick(mx - stageRect_.x, my - stageRect_.y);
        for (Target* t : rt_->targets)
            if (t->drawableID == id && t->draggable && t->visible) {
                want = curHand_;
                break;
            }
    }
    if (want != curActive_) {
        SDL_SetCursor(want ? want : curArrow_);
        curActive_ = want ? want : curArrow_;
    }
}

void SdlRenderer::postMouse(int mx, int my, bool hasDown, bool down,
                            int button, bool wasDragged) {
    Mouse::PostData pd;
    pd.x = mx - stageRect_.x;
    pd.y = my - stageRect_.y;
    pd.canvasWidth = stageRect_.w;
    pd.canvasHeight = stageRect_.h;
    pd.hasXY = true;
    pd.hasIsDown = hasDown;
    pd.isDown = down;
    pd.button = button;
    pd.wasDragged = wasDragged;
    rt_->ioDevices.mouse.postData(pd);
}

// window px → scratch 座標（ドラッグ位置計算用。丸めなし）
static void winToScratch(const IRect& r, int stageW, int stageH,
                         int wx, int wy, double& sx, double& sy) {
    sx = (wx - r.x) / double(r.w) * stageW - stageW / 2.0;
    sy = stageH / 2.0 - (wy - r.y) / double(r.h) * stageH;
}

// ask 入力中のキー編集（UTF-16 サロゲートペア対応）。
void SdlRenderer::askEditKey(int sym) {
    auto& input = askInput_;
    auto& caret = askCaret_;
    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        askSubmit();
        return;
    case SDLK_BACKSPACE:
        if (caret > 0) {
            size_t n = 1;
            if (caret >= 2 && input[caret - 1] >= 0xDC00 &&
                input[caret - 1] <= 0xDFFF &&
                input[caret - 2] >= 0xD800 && input[caret - 2] <= 0xDBFF)
                n = 2;   // サロゲートペア
            input.erase(caret - n, n);
            caret -= n;
        }
        return;
    case SDLK_DELETE:
        if (caret < input.size()) {
            size_t n = 1;
            if (input[caret] >= 0xD800 && input[caret] <= 0xDBFF &&
                caret + 1 < input.size()) n = 2;
            input.erase(caret, n);
        }
        return;
    case SDLK_LEFT:
        if (caret > 0) {
            --caret;
            if (caret > 0 && input[caret] >= 0xDC00 && input[caret] <= 0xDFFF)
                --caret;
        }
        return;
    case SDLK_RIGHT:
        if (caret < input.size()) {
            ++caret;
            if (caret < input.size() && input[caret] >= 0xDC00 &&
                input[caret] <= 0xDFFF)
                ++caret;
        }
        return;
    case SDLK_HOME: caret = 0; return;
    case SDLK_END: caret = input.size(); return;
    default: return;   // 入力中は他キーはランタイムへ流さない
    }
}

bool SdlRenderer::pollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (!handleEvent(ev)) return false;
    return true;
}

bool SdlRenderer::handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_QUIT:
        return false;
    case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
            ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            recomputeViewport();
        else if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
            return false;
        break;
    case SDL_TEXTINPUT:
        if (askActive_) {
            std::u16string ins = utf8ToU16(ev.text.text);
            askInput_.insert(askCaret_, ins);
            askCaret_ += ins.size();
        }
        break;
    case SDL_MOUSEMOTION:
        updateCursor(ev.motion.x, ev.motion.y);
        postMouse(ev.motion.x, ev.motion.y, false, false, 0);
        if (sliderVar_) sliderDragTo(ev.motion.x);
        if (dragTarget_) {
            if (ev.motion.state & SDL_BUTTON_LMASK) {
                dragMoved_ = true;
                double sx, sy;
                winToScratch(stageRect_, stageW_, stageH_,
                             ev.motion.x, ev.motion.y, sx, sy);
                dragTarget_->setXY(sx + dragOffX_, sy + dragOffY_,
                                   true);
            }
        } else if (ev.motion.state & SDL_BUTTON_LMASK) {
            dragMoved_ = true;
        }
        break;
    case SDL_MOUSEBUTTONDOWN: {
        int which;
        if (ev.button.button == SDL_BUTTON_LEFT &&
            hitChrome(ev.button.x, ev.button.y, which)) {
            if (which == 0) {
                SDL_Keymod mod = SDL_GetModState();
                if (mod & KMOD_SHIFT) rt_->turboMode = true;
                rt_->greenFlag();
            } else {
                rt_->stopAll();
            }
            break;
        }
        dragMoved_ = false;
        if (ev.button.button == SDL_BUTTON_LEFT) {
            // ✓送信ボタン
            if (askActive_) {
                const IRect& sr = askSubmitRect_;
                if (ev.button.x >= sr.x && ev.button.x < sr.x + sr.w &&
                    ev.button.y >= sr.y && ev.button.y < sr.y + sr.h) {
                    askSubmit();
                    break;
                }
            }
            // UI 要素のクリックはステージ入力を消費（isDown を流さない）
            if (askHitTest(ev.button.x, ev.button.y) ||
                sliderHitTest(ev.button.x, ev.button.y)) {
                postMouse(ev.button.x, ev.button.y, false, false, 0);
                break;
            }
            // ドラッグ可能スプライトの掴み（クリックハットは mouse-up
            // で postData 側が判定するため postMouse は通常経路）
            int id = pick(ev.button.x - stageRect_.x,
                          ev.button.y - stageRect_.y);
            Target* pt = nullptr;
            for (Target* t : rt_->targets)
                if (t->drawableID == id) { pt = t; break; }
            if (pt && !pt->isStage && pt->draggable && pt->visible) {
                dragTarget_ = pt;
                pt->dragging = true;
                double sx, sy;
                winToScratch(stageRect_, stageW_, stageH_,
                             ev.button.x, ev.button.y, sx, sy);
                dragOffX_ = pt->x - sx;
                dragOffY_ = pt->y - sy;
            }
        }
        postMouse(ev.button.x, ev.button.y, true, true,
                  ev.button.button);
        break;
    }
    case SDL_MOUSEBUTTONUP: {
        if (ev.button.button == SDL_BUTTON_LEFT) {
            if (dragTarget_) {
                dragTarget_->dragging = false;
                dragTarget_ = nullptr;
            }
            sliderVar_ = nullptr;
        }
        postMouse(ev.button.x, ev.button.y, true, false,
                  ev.button.button, dragMoved_);
        dragMoved_ = false;
        break;
    }
    case SDL_MOUSEWHEEL:
        rt_->ioDevices.mouseWheel.postData(ev.wheel.preciseX,
                                           -ev.wheel.preciseY);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (askActive_) {   // 入力中はキーをランタイムへ流さず全て消費
            if (ev.type == SDL_KEYDOWN)
                askEditKey(ev.key.keysym.sym);
            break;
        }
        if (ev.type == SDL_KEYDOWN && ev.key.repeat) break;
        std::string name = SDL_GetKeyName(ev.key.keysym.sym);
        // SDL 名 → Scratch 名（keyStringToScratchKey が解釈する形式）
        std::u16string key;
        if (name == "Space") key = u" ";
        else if (name == "Return" || name == "Keypad Enter")
            key = u"Enter";
        else if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
            key = char16_t(name[0] - 'A' + 'a');
        else {
            std::string utf8 = name;
            key.assign(utf8.begin(), utf8.end());
        }
        rt_->ioDevices.keyboard.postData(key, ev.type == SDL_KEYDOWN,
                                         ev.key.keysym.sym);
        break;
    }
    default:
        break;
    }
    return true;
}

} // namespace xyo

#endif // XYO_HAVE_SDL2
