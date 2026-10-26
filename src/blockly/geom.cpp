// xyo-cpp: geom.hpp の実装。
// 描画バックエンドは 専用バッチ描画（ui::DrawList）。SDL_Renderer 引数は
// シグネチャ互換のため残しており、プリミティブ自体は setDrawList() で
// 設定した ui::DrawList へ描く（フレーム先頭で 1 回設定する）。
#include "geom.hpp"

#if defined(XYO_HAVE_SDL2) && !defined(__EMSCRIPTEN__)

#include "ui/draw.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace xyo {
namespace blockly {

namespace {

constexpr float kPi = 3.14159265358979f;

thread_local ui::DrawList* g_dl = nullptr;
thread_local std::vector<ui::Vec2> g_points;

ui::PackedColor colOf(Color c) { return ui::rgba(c.r, c.g, c.b, c.a); }

/// Path の点列を ui::Vec2（スケール適用）へ。
const std::vector<ui::Vec2>& toVec(const Path::Pt* pts, int n, float s) {
    auto& v = g_points;
    v.resize((size_t)n);
    for (int i = 0; i < n; i++)
        v[i] = {pts[i].x * s, pts[i].y * s};
    return v;
}
const std::vector<ui::Vec2>& toVec(const float* xy, int n, float s) {
    auto& v = g_points;
    v.resize((size_t)n);
    for (int i = 0; i < n; i++)
        v[i] = {xy[i * 2] * s, xy[i * 2 + 1] * s};
    return v;
}

} // namespace

void setDrawList(ui::DrawList* dl) { g_dl = dl; }
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
    if (!g_dl) return;
    if (p.sub.empty()) {
        if (p.pts.size() >= 3) {
            const auto& v = toVec(p.pts.data(), (int)p.pts.size(), s);
            g_dl->AddConcavePolyFilled(v.data(), (int)v.size(), colOf(c));
        }
        return;
    }
    int m = (int)p.sub.size();
    for (int i = 0; i < m; i++) {
        int st = p.sub[i];
        int en = (i + 1 < m) ? p.sub[i + 1] : (int)p.pts.size();
        if (en - st >= 3) {
            const auto& v = toVec(&p.pts[st], en - st, s);
            g_dl->AddConcavePolyFilled(v.data(), (int)v.size(), colOf(c));
        }
    }
}

void fillPoly(SDL_Renderer* r, const float* xy, int n, float s, Color c) {
    if (!g_dl || n < 3) return;
    const auto& v = toVec(xy, n, s);
    g_dl->AddConcavePolyFilled(v.data(), n, colOf(c));
}

void strokePath(SDL_Renderer* r, const Path& p, float s, Color c, float t,
                bool close) {
    if (!g_dl || p.pts.size() < 2) return;
    const auto& v = toVec(p.pts.data(), (int)p.pts.size(), s);
    g_dl->AddPolyline(v.data(), (int)v.size(), colOf(c),
                      close ? ui::DrawFlags_Closed : ui::DrawFlags_None,
                      std::max(t, 0.75f));
}

// ---- プリミティブ --------------------------------------------------------
void fill(SDL_Renderer* r, float x, float y, float w, float h, Color c) {
    if (!g_dl) return;
    g_dl->AddRectFilled({x, y}, {x + w, y + h}, colOf(c));
}

void outline(SDL_Renderer* r, float x, float y, float w, float h, Color c) {
    if (!g_dl) return;
    g_dl->AddRect({x, y}, {x + w, y + h}, colOf(c));
}

void rrFill(SDL_Renderer* r, float x, float y, float w, float h, float rad,
            Color c) {
    if (!g_dl || w <= 0 || h <= 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    g_dl->AddRectFilled({x, y}, {x + w, y + h}, colOf(c), rad);
}

void rrOutline(SDL_Renderer* r, float x, float y, float w, float h, float rad,
               Color c, float t) {
    if (!g_dl || w <= 0 || h <= 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    g_dl->AddRect({x, y}, {x + w, y + h}, colOf(c), rad, 0,
                  std::max(t, 0.75f));
}

void hexFill(SDL_Renderer* r, float x, float y, float w, float h, float n,
             Color c) {
    if (!g_dl) return;
    n = std::max(0.f, n);
    ui::Vec2 v[6] = {{x + n, y},     {x + w - n, y}, {x + w, y + h / 2},
                   {x + w - n, y + h}, {x + n, y + h}, {x, y + h / 2}};
    g_dl->AddConvexPolyFilled(v, 6, colOf(c));
}

void hexOutline(SDL_Renderer* r, float x, float y, float w, float h, float n,
                Color c, float t) {
    if (!g_dl) return;
    n = std::max(0.f, n);
    ui::Vec2 v[6] = {{x + n, y},     {x + w - n, y}, {x + w, y + h / 2},
                   {x + w - n, y + h}, {x + n, y + h}, {x, y + h / 2}};
    g_dl->AddPolyline(v, 6, colOf(c), ui::DrawFlags_Closed,
                      std::max(t, 0.75f));
}

void tri(SDL_Renderer* r, float x0, float y0, float x1, float y1,
         float x2, float y2, Color c) {
    if (!g_dl) return;
    g_dl->AddTriangleFilled({x0, y0}, {x1, y1}, {x2, y2}, colOf(c));
}

void line(SDL_Renderer* r, float x0, float y0, float x1, float y1, Color c,
          float t) {
    if (!g_dl) return;
    g_dl->AddLine({x0, y0}, {x1, y1}, colOf(c), std::max(t, 0.75f));
}

void discFill(SDL_Renderer* r, float cx, float cy, float rad, Color c) {
    if (!g_dl) return;
    g_dl->AddCircleFilled({cx, cy}, rad, colOf(c));
}

void ring(SDL_Renderer* r, float cx, float cy, float rad, float t, Color c) {
    if (!g_dl) return;
    g_dl->AddCircle({cx, cy}, rad, colOf(c), 0, std::max(t, 0.75f));
}

} // namespace blockly
} // namespace xyo

#else

// SDL2/UI 無しビルド: Path のジオメトリだけ残し、描画は何もしない。
#include <cmath>

namespace xyo {
namespace blockly {

constexpr float kPi = 3.14159265358979f;

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
