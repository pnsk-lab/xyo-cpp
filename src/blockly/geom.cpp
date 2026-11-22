// xyo-cpp: geom.hpp の実装。
// 描画バックエンドは blockly::Canvas 抽象（setCanvas() で設定）。
// SDL2 ビルドでは ui::DrawList へマップする DrawListCanvas が既定。
// SDL_Renderer 引数はシグネチャ互換のため残してある。
#include "geom.hpp"

#if defined(XYO_HAVE_SDL2) && !defined(__EMSCRIPTEN__)

#include <SDL.h>

#include "ui/draw.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace xyo {
namespace blockly {

namespace {

constexpr float kPi = 3.14159265358979f;

thread_local Canvas* g_canvas = nullptr;
thread_local std::vector<float> g_xy;

/// xy 配列をスケールして返す（n = 点の個数 → 要素数は 2n）。
const float* scalePts(const float* xy, int n, float s) {
    auto& v = g_xy;
    v.resize((size_t)n * 2);
    for (int i = 0; i < n * 2; i++) v[i] = xy[i] * s;
    return v.data();
}
const float* scalePts(const Path::Pt* pts, int n, float s) {
    auto& v = g_xy;
    v.resize((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        v[i * 2] = pts[i].x * s;
        v[i * 2 + 1] = pts[i].y * s;
    }
    return v.data();
}

/// ui::DrawList へプリミティブを落とす SDL2 実装。
class DrawListCanvas final : public Canvas {
public:
    explicit DrawListCanvas(ui::DrawList* dl) : dl_(dl) {}

    void fillPoly(const float* xy, int n, Color c) override {
        pts_.resize((size_t)n);
        for (int i = 0; i < n; i++) pts_[i] = {xy[i * 2], xy[i * 2 + 1]};
        dl_->AddConcavePolyFilled(pts_.data(), n, col(c));
    }
    void convexPoly(const float* xy, int n, Color c) {
        pts_.resize((size_t)n);
        for (int i = 0; i < n; i++) pts_[i] = {xy[i * 2], xy[i * 2 + 1]};
        dl_->AddConvexPolyFilled(pts_.data(), n, col(c));
    }
    void strokePoly(const float* xy, int n, Color c, float t,
                    bool close) override {
        pts_.resize((size_t)n);
        for (int i = 0; i < n; i++) pts_[i] = {xy[i * 2], xy[i * 2 + 1]};
        dl_->AddPolyline(pts_.data(), n, col(c),
                         close ? ui::DrawFlags_Closed : ui::DrawFlags_None, t);
    }
    void fillRect(float x, float y, float w, float h, Color c) override {
        dl_->AddRectFilled({x, y}, {x + w, y + h}, col(c));
    }
    void strokeRect(float x, float y, float w, float h, Color c,
                    float t) override {
        dl_->AddRect({x, y}, {x + w, y + h}, col(c), 0, 0, t);
    }
    void fillRoundRect(float x, float y, float w, float h, float rad,
                       Color c) override {
        dl_->AddRectFilled({x, y}, {x + w, y + h}, col(c), rad);
    }
    void strokeRoundRect(float x, float y, float w, float h, float rad,
                         Color c, float t) override {
        dl_->AddRect({x, y}, {x + w, y + h}, col(c), rad, 0, t);
    }
    void line(float x0, float y0, float x1, float y1, Color c,
              float t) override {
        dl_->AddLine({x0, y0}, {x1, y1}, col(c), t);
    }
    void fillCircle(float cx, float cy, float rad, Color c) override {
        dl_->AddCircleFilled({cx, cy}, rad, col(c));
    }
    void strokeCircle(float cx, float cy, float rad, float t,
                      Color c) override {
        dl_->AddCircle({cx, cy}, rad, col(c), 0, t);
    }
    void icon(const IconPx* ipx, SDL_Renderer* r, float x, float y,
              float w, float h, float alpha) override {
        if (!ipx || !r) return;
        if (owner_ != r) {
            for (auto& [k, t] : cache_) SDL_DestroyTexture(t);
            cache_.clear();
            owner_ = r;
        }
        SDL_Texture* t = cache_[ipx];
        if (!t) {
            t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STATIC, ipx->w, ipx->h);
            if (!t) return;
            SDL_UpdateTexture(t, nullptr, ipx->px, ipx->w * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            cache_[ipx] = t;
        }
        int a = std::min(255, (int)(alpha * 255));
        dl_->AddImage((ui::TextureID)(uintptr_t)t, {x, y}, {x + w, y + h},
                      {0, 0}, {1, 1}, ui::rgba(255, 255, 255, a));
    }
    void pushClip(float x0, float y0, float x1, float y1,
                  bool intersect) override {
        dl_->PushClipRect({x0, y0}, {x1, y1}, intersect);
    }
    void popClip() override { dl_->PopClipRect(); }
    void clearIcons() override {
        for (auto& [k, t] : cache_) SDL_DestroyTexture(t);
        cache_.clear();
        owner_ = nullptr;
    }

    ui::DrawList* dl_ = nullptr;

private:
    static ui::PackedColor col(Color c) {
        return ui::rgba(c.r, c.g, c.b, c.a);
    }
    std::vector<ui::Vec2> pts_;
    std::map<const IconPx*, SDL_Texture*> cache_;
    SDL_Renderer* owner_ = nullptr;
};

/// setDrawList() 互換用の既定 Canvas（g_dl に束ねる）。
thread_local ui::DrawList* g_dl = nullptr;
thread_local DrawListCanvas* g_dlc = nullptr;

} // namespace

void setCanvas(Canvas* c) { g_canvas = c; }
Canvas* canvas() { return g_canvas; }

void setDrawList(ui::DrawList* dl) {
    g_dl = dl;
    if (!g_dlc) g_dlc = new DrawListCanvas(dl);
    else g_dlc->dl_ = dl;
    g_canvas = dl ? static_cast<Canvas*>(g_dlc) : nullptr;
}
ui::DrawList* drawList() { return g_dl; }

// ---- Path ---------------------------------------------------------------
void Path::cubicTo(float c1x, float c1y, float c2x, float c2y,
                   float ex, float ey, int seg) {
    if (pts.empty()) { moveTo(ex, ey); return; }
    float x0 = pts.back().x, y0 = pts.back().y;
    for (int i = 1; i <= seg; i++) {
        float t = i / float(seg), u = 1 - t;
        float x = u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*ex;
        float y = u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*ey;
        lineTo(x, y);
    }
}

void Path::arcTo(float cx, float cy, float r, float a0deg, float a1deg,
                 int seg) {
    float a0 = a0deg * kPi / 180.f, a1 = a1deg * kPi / 180.f;
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (a1 - a0) * (i / float(seg));
        float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        if (i == 0 && pts.empty()) moveTo(x, y);
        else lineTo(x, y);
    }
}

void Path::cornerTo(float cx, float cy, float r, float ex, float ey) {
    if (pts.empty()) { moveTo(ex, ey); return; }
    // 現在位置 p、角 c、次点 e。p→c と c→e の接線方向に r の円弧を引く。
    float px = pts.back().x, py = pts.back().y;
    float ux = cx - px, uy = cy - py;
    float vx = ex - cx, vy = ey - cy;
    float ul = std::hypot(ux, uy), vl = std::hypot(vx, vy);
    if (ul < 1e-4 || vl < 1e-4) { lineTo(cx, cy); return; }
    ux /= ul; uy /= ul; vx /= vl; vy /= vl;
    // 入側接点 t0 = c - u*r、出側接点 t1 = c + v*r
    float t0x = cx - ux * r, t0y = cy - uy * r;
    float t1x = cx + vx * r, t1y = cy + vy * r;
    // 円弧の中心は角 c ではなく内側へ r 入った o = t0 + n*r
    // （n = 折れの内側法線。y 下向き座標で cross(u,v)>0 なら左折で n=(-uy,ux)）
    float nx, ny;
    if (ux * vy - uy * vx > 0) { nx = -uy; ny = ux; }
    else                       { nx = uy;  ny = -ux; }
    float ox = t0x + nx * r, oy = t0y + ny * r;
    lineTo(t0x, t0y);
    // t0→t1 を円弧で（角度の小さい回り）
    float a0 = std::atan2(t0y - oy, t0x - ox);
    float a1 = std::atan2(t1y - oy, t1x - ox);
    float d = a1 - a0;
    while (d > kPi) d -= 2 * kPi;
    while (d < -kPi) d += 2 * kPi;
    int seg = std::max(2, (int)(std::fabs(d) / (kPi / 12.f)));
    for (int i = 1; i <= seg; i++) {
        float a = a0 + d * (i / float(seg));
        lineTo(ox + r * std::cos(a), oy + r * std::sin(a));
    }
    lineTo(t1x, t1y);
}

// ---- 塗り / 輪郭 ---------------------------------------------------------
void fillPath(SDL_Renderer* r, const Path& p, float s, Color c) {
    if (!g_canvas) return;
    if (p.sub.empty()) {
        if (p.pts.size() >= 3)
            g_canvas->fillPoly(scalePts(p.pts.data(), (int)p.pts.size(), s),
                               (int)p.pts.size(), c);
        return;
    }
    int m = (int)p.sub.size();
    for (int i = 0; i < m; i++) {
        int st = p.sub[i];
        int en = (i + 1 < m) ? p.sub[i + 1] : (int)p.pts.size();
        if (en - st >= 3)
            g_canvas->fillPoly(scalePts(&p.pts[st], en - st, s), en - st, c);
    }
}

void fillPoly(SDL_Renderer* r, const float* xy, int n, float s, Color c) {
    if (!g_canvas || n < 3) return;
    g_canvas->fillPoly(scalePts(xy, n, s), n, c);
}

void strokePath(SDL_Renderer* r, const Path& p, float s, Color c, float t,
                bool close) {
    if (!g_canvas || p.pts.size() < 2) return;
    g_canvas->strokePoly(scalePts(p.pts.data(), (int)p.pts.size(), s),
                         (int)p.pts.size(), c, std::max(t, 0.75f), close);
}

// ---- プリミティブ --------------------------------------------------------
void fill(SDL_Renderer* r, float x, float y, float w, float h, Color c) {
    if (!g_canvas) return;
    g_canvas->fillRect(x, y, w, h, c);
}

void outline(SDL_Renderer* r, float x, float y, float w, float h, Color c) {
    if (!g_canvas) return;
    g_canvas->strokeRect(x, y, w, h, c, 1.0f);
}

void rrFill(SDL_Renderer* r, float x, float y, float w, float h, float rad,
            Color c) {
    if (!g_canvas || w <= 0 || h <= 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    g_canvas->fillRoundRect(x, y, w, h, rad, c);
}

void rrOutline(SDL_Renderer* r, float x, float y, float w, float h, float rad,
               Color c, float t) {
    if (!g_canvas || w <= 0 || h <= 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    g_canvas->strokeRoundRect(x, y, w, h, rad, c, std::max(t, 0.75f));
}

void hexFill(SDL_Renderer* r, float x, float y, float w, float h, float n,
             Color c) {
    if (!g_canvas) return;
    n = std::max(0.f, n);
    float v[12] = {x + n, y,     x + w - n, y,     x + w, y + h / 2,
                   x + w - n, y + h, x + n, y + h, x, y + h / 2};
    g_canvas->fillPoly(v, 6, c);
}

void hexOutline(SDL_Renderer* r, float x, float y, float w, float h, float n,
                Color c, float t) {
    if (!g_canvas) return;
    n = std::max(0.f, n);
    float v[12] = {x + n, y,     x + w - n, y,     x + w, y + h / 2,
                   x + w - n, y + h, x + n, y + h, x, y + h / 2};
    g_canvas->strokePoly(v, 6, c, std::max(t, 0.75f), true);
}

void tri(SDL_Renderer* r, float x0, float y0, float x1, float y1,
         float x2, float y2, Color c) {
    if (!g_canvas) return;
    float v[6] = {x0, y0, x1, y1, x2, y2};
    g_canvas->fillPoly(v, 3, c);
}

void line(SDL_Renderer* r, float x0, float y0, float x1, float y1, Color c,
          float t) {
    if (!g_canvas) return;
    g_canvas->line(x0, y0, x1, y1, c, std::max(t, 0.75f));
}

void discFill(SDL_Renderer* r, float cx, float cy, float rad, Color c) {
    if (!g_canvas) return;
    g_canvas->fillCircle(cx, cy, rad, c);
}

void ring(SDL_Renderer* r, float cx, float cy, float rad, float t, Color c) {
    if (!g_canvas) return;
    g_canvas->strokeCircle(cx, cy, rad, std::max(t, 0.75f), c);
}

} // namespace blockly
} // namespace xyo

#else

// SDL2/UI 無しビルド: Path のジオメトリだけ残し、描画は何もしない。
#include <cmath>

namespace xyo {
namespace blockly {

constexpr float kPi = 3.14159265358979f;

void setCanvas(Canvas*) {}
Canvas* canvas() { return nullptr; }

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y,
                   float ex, float ey, int seg) {
    if (pts.empty()) { moveTo(ex, ey); return; }
    float x0 = pts.back().x, y0 = pts.back().y;
    for (int i = 1; i <= seg; i++) {
        float t = i / float(seg), u = 1 - t;
        float x = u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*ex;
        float y = u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*ey;
        lineTo(x, y);
    }
}
void Path::arcTo(float cx, float cy, float r, float a0deg, float a1deg,
                 int seg) {
    float a0 = a0deg * kPi / 180.f, a1 = a1deg * kPi / 180.f;
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (a1 - a0) * (i / float(seg));
        float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        if (i == 0 && pts.empty()) moveTo(x, y);
        else lineTo(x, y);
    }
}
void Path::cornerTo(float cx, float cy, float r, float ex, float ey) {
    if (pts.empty()) { moveTo(ex, ey); return; }
    lineTo(cx, cy);
    lineTo(ex, ey);
}

// 描画関数はスタブ（呼び出し側も headless では走らないが、リンクは通す）
void fillPath(SDL_Renderer*, const Path&, float, Color) {}
void fillPoly(SDL_Renderer*, const float*, int, float, Color) {}
void strokePath(SDL_Renderer*, const Path&, float, Color, float, bool) {}
void fill(SDL_Renderer*, float, float, float, float, Color) {}
void outline(SDL_Renderer*, float, float, float, float, Color) {}
void rrFill(SDL_Renderer*, float, float, float, float, float, Color) {}
void rrOutline(SDL_Renderer*, float, float, float, float, float, Color,
               float) {}
void hexFill(SDL_Renderer*, float, float, float, float, float, Color) {}
void hexOutline(SDL_Renderer*, float, float, float, float, float, Color,
                float) {}
void tri(SDL_Renderer*, float, float, float, float, float, float, Color) {}
void line(SDL_Renderer*, float, float, float, float, Color, float) {}
void discFill(SDL_Renderer*, float, float, float, Color) {}
void ring(SDL_Renderer*, float, float, float, float, Color) {}
void setDrawList(ui::DrawList*) {}
ui::DrawList* drawList() { return nullptr; }

} // namespace blockly
} // namespace xyo

#endif  // XYO_HAVE_SDL2 && !__EMSCRIPTEN__
