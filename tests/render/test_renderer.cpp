#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "blocks/pen_api.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "render/sdl_renderer.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

namespace xyo {

struct SdlRendererTestAccess {
    static size_t skinCount(const SdlRenderer& r) { return r.skins_.size(); }
    static bool cached(const SdlRenderer& r, const Costume* c) {
        return r.skins_.find(c) != r.skins_.end();
    }
    static const void* cachedSkin(const SdlRenderer& r, const Costume* c) {
        auto it = r.skins_.find(c);
        return it == r.skins_.end() ? nullptr : it->second.get();
    }
    static const SdlRenderer::Skin* skin(const SdlRenderer& r,
                                         const Costume* c) {
        auto it = r.skins_.find(c);
        return it == r.skins_.end() ? nullptr : it->second.get();
    }
    static uint8_t* penPx(SdlRenderer& r) {
        // GL ペン層では penPx_ は遅延同期（CPU 読者の直前でのみ readback）。
        // interop 表示経路は readback を行わないため、内部バッファを直接
        // 覗くテストはここで明示的に同期させる。
        r.penSyncCpu();
        return r.penPx_.data();
    }
    static int penW(const SdlRenderer& r) { return r.penW_; }
    static int penH(const SdlRenderer& r) { return r.penH_; }
    static void penBlendPx(SdlRenderer& r, int x, int y, const uint8_t c[4]) {
        r.penBlendPx(x, y, c);
    }
};

}

using TA = SdlRendererTestAccess;

namespace {

const char* kSvgZero =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0" viewBox="0 0 0 0"></svg>)SVG";
const char* kSvgRect =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10"><rect x="0" y="0" width="20" height="10" fill="#ff0000"/></svg>)SVG";
const char* kSvgTextRed =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="60"><text x="12" y="42" font-size="32" fill="#ff0000">HI</text></svg>)SVG";
const char* kSvgTextTspan =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120"><g transform="translate(12,8)"><text x="10" y="28" font-size="24" fill="#ff0000"><tspan>HELLO</tspan><tspan x="10" dy="32" fill="#0000ff">WORLD</tspan></text></g></svg>)SVG";
const char* kSvgTextOpacity =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="60"><text x="10" y="44" font-size="40" fill="#00ff00" opacity="0.5">A</text></svg>)SVG";
const char* kSvgTextAnchor =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="80"><text x="120" y="50" font-size="36" fill="#ff0000" text-anchor="middle">TXT</text></svg>)SVG";
const char* kSvgViewBox =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="-50 -30 100 60"><rect x="-50" y="-30" width="25" height="15" fill="#ff0000"/></svg>)SVG";
const char* kSvgTextJp =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="60"><text x="8" y="42" font-size="32" font-family="sans-serif" fill="#000000">海 日本語</text></svg>)SVG";
const char* kSvgTextStroke =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><text x="10" y="55" font-size="48" fill="none" stroke="#ff0000" stroke-width="2">A</text></svg>)SVG";
const char* kSvgBroken = "<svg width='10' height='10'";
const char* kSvgJunk = "not xml at all <<<>>>";
const char* kSvgUnsized =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect width="10" height="10" fill="#ff0000"/></svg>)SVG";
const char* kSvgBadViewBox =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 -10 -10"><rect width="5" height="5" fill="#ff0000"/></svg>)SVG";
const char* kSvgNegDim =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="-5"></svg>)SVG";

// SVG のラスタ化バックエンド: librsvg（有り）または nanosvg（無し = Windows 等）。
// nanosvg は <text>/<tspan> を描画せず、viewBox の無い SVG を図形の
// バウンディングボックスから採寸する。文字や厳密な画素の検査は librsvg
// ビルド専用なので、フォールバックでは XYO_TEST_RSVG=0 で読み飛ばす。
#if defined(XYO_HAVE_RSVG)
#define XYO_TEST_RSVG 1
#else
#define XYO_TEST_RSVG 0
#endif

Target* spriteOf(Runtime& rt) {
    for (Target* t : rt.targets) if (t->sprite && !t->isStage) return t;
    return nullptr;
}

bool loadProject(Runtime& rt, const char* costumesJson) {
    std::string json = std::string(R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "costumes": )JSON") + costumesJson + R"JSON(,
     "sounds": [], "currentCostume": 0,
     "x": 0, "y": 0, "size": 100, "direction": 90,
     "rotationStyle": "all around", "visible": true, "layerOrder": 1}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";
    Sb3LoadResult r = loadSb3(rt, json, nullptr);
    if (!r.ok) std::printf("load: %s\n", r.error.c_str());
    return r.ok;
}

void setCostumeData(Target* t, int i, const char* bytes, size_t n) {
    Costume& c = t->sprite->costumes[i];
    c.data.assign(bytes, bytes + n);
}

std::pair<double, double> fence(const SdlRenderer& ren, int drawableID,
                                double x = 1000.0, double y = 1000.0) {
    ren.getFencedPosition(drawableID, x, y, true);
    return {x, y};
}

int test_negative_cache_zero_svg() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"a","name":"a",
        "dataFormat":"svg","md5ext":"a.svg","bitmapResolution":1,
        "rotationCenterX":10,"rotationCenterY":5}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const Costume* c = sp->getCurrentCostume();
    CHECK(c != nullptr);
    const int id = sp->drawableID;
    CHECK(id >= 0);

    for (int i = 0; i < 3; ++i) {
        auto [x, y] = fence(ren, id);
        CHECK_EQ(x, 240.0);
        CHECK_EQ(y, 180.0);
    }
    ren.draw();
    CHECK_EQ(ren.pick(240, 180), -1);

    CHECK(TA::cached(ren, c));
    CHECK_EQ(TA::cachedSkin(ren, c), nullptr);
    CHECK_EQ(TA::skinCount(ren), 1u);
    for (int i = 0; i < 3; ++i) fence(ren, id);
    ren.draw();
    CHECK_EQ(TA::skinCount(ren), 1u);
    std::printf("neg-zero-svg: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_negative_cache_empty_and_bad_bitmap() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([
        {"assetId":"e","name":"e","dataFormat":"svg","md5ext":"e.svg",
         "bitmapResolution":1,"rotationCenterX":0,"rotationCenterY":0},
        {"assetId":"b","name":"b","dataFormat":"png","md5ext":"b.png",
         "bitmapResolution":1,"rotationCenterX":0,"rotationCenterY":0}
    ])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    const char junk[] = "not a png image";
    setCostumeData(sp, 1, junk, sizeof(junk));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const Costume* cEmpty = &sp->sprite->costumes[0];
    const Costume* cBad = &sp->sprite->costumes[1];
    const int id = sp->drawableID;

    sp->currentCostume = 0;
    for (int i = 0; i < 3; ++i) {
        auto [x, y] = fence(ren, id);
        CHECK_EQ(x, 240.0);
        CHECK_EQ(y, 180.0);
    }
    CHECK(TA::cached(ren, cEmpty));
    CHECK_EQ(TA::cachedSkin(ren, cEmpty), nullptr);

    sp->currentCostume = 1;
    for (int i = 0; i < 3; ++i) {
        auto [x, y] = fence(ren, id);
        CHECK_EQ(x, 240.0);
        CHECK_EQ(y, 180.0);
    }
    CHECK(TA::cached(ren, cBad));
    CHECK_EQ(TA::cachedSkin(ren, cBad), nullptr);
    CHECK_EQ(TA::skinCount(ren), 2u);

    sp->currentCostume = 0;
    fence(ren, id);
    sp->currentCostume = 1;
    fence(ren, id);
    ren.draw();
    CHECK_EQ(TA::skinCount(ren), 2u);
    std::printf("neg-empty+badpng: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_positive_cache_valid_svg() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"v","name":"v",
        "dataFormat":"svg","md5ext":"v.svg","bitmapResolution":1,
        "rotationCenterX":10,"rotationCenterY":5}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgRect, std::strlen(kSvgRect));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const Costume* c = sp->getCurrentCostume();
    const int id = sp->drawableID;

    auto [x, y] = fence(ren, id);
    CHECK_EQ(x, 245.0);
    CHECK_EQ(y, 180.0);
    CHECK(TA::cached(ren, c));
    const void* s1 = TA::cachedSkin(ren, c);
    CHECK(s1 != nullptr);
    CHECK_EQ(TA::skinCount(ren), 1u);

    for (int i = 0; i < 3; ++i) {
        auto [x2, y2] = fence(ren, id);
        CHECK_EQ(x2, 245.0);
        CHECK_EQ(y2, 180.0);
        CHECK_EQ(TA::cachedSkin(ren, c), s1);
    }
    ren.draw();
    CHECK_EQ(TA::skinCount(ren), 1u);

    double ux = 1000.0, uy = 1000.0;
    ren.getFencedPosition(id, ux, uy, false);
    CHECK_EQ(ux, 1000.0);
    CHECK_EQ(uy, 1000.0);
    std::printf("pos-valid-svg: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_costume_switch_and_clone() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([
        {"assetId":"z","name":"z","dataFormat":"svg","md5ext":"z.svg",
         "bitmapResolution":1,"rotationCenterX":0,"rotationCenterY":0},
        {"assetId":"v","name":"v","dataFormat":"svg","md5ext":"v.svg",
         "bitmapResolution":1,"rotationCenterX":10,"rotationCenterY":5}
    ])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));
    setCostumeData(sp, 1, kSvgRect, std::strlen(kSvgRect));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const Costume* cBad = &sp->sprite->costumes[0];
    const Costume* cGood = &sp->sprite->costumes[1];
    const int id = sp->drawableID;

    sp->currentCostume = 0;
    auto p = fence(ren, id);
    CHECK_EQ(p.first, 240.0);
    CHECK_EQ(p.second, 180.0);
    CHECK(TA::cached(ren, cBad));
    CHECK_EQ(TA::cachedSkin(ren, cBad), nullptr);

    sp->currentCostume = 1;
    p = fence(ren, id);
    CHECK_EQ(p.first, 245.0);
    CHECK_EQ(p.second, 180.0);
    CHECK(TA::cached(ren, cGood));
    CHECK(TA::cachedSkin(ren, cGood) != nullptr);

    sp->currentCostume = 0;
    p = fence(ren, id);
    CHECK_EQ(p.first, 240.0);
    CHECK_EQ(p.second, 180.0);
    CHECK_EQ(TA::skinCount(ren), 2u);
    ren.draw();
    CHECK_EQ(TA::skinCount(ren), 2u);

    Target* cl = sp->makeClone();
    CHECK(cl != nullptr);
    if (cl) {
        CHECK(cl->drawableID >= 0);
        cl->currentCostume = 1;
        CHECK_EQ(cl->getCurrentCostume(), cGood);
        p = fence(ren, cl->drawableID);
        CHECK_EQ(p.first, 245.0);
        CHECK_EQ(p.second, 180.0);
        cl->currentCostume = 0;
        CHECK_EQ(cl->getCurrentCostume(), cBad);
        p = fence(ren, cl->drawableID);
        CHECK_EQ(p.first, 240.0);
        CHECK_EQ(p.second, 180.0);
        CHECK_EQ(TA::skinCount(ren), 2u);
        ren.draw();
        CHECK_EQ(TA::skinCount(ren), 2u);
        cl->dispose();
        delete cl;
    }
    std::printf("switch+clone: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

void refPremult(uint8_t p[4], const float rgba[4], double cov) {
    double sa = std::clamp(double(rgba[3]) * cov, 0.0, 1.0);
    double om = 1.0 - sa;
    for (int i = 0; i < 3; ++i)
        p[i] = static_cast<uint8_t>(std::clamp(
            std::lround(std::clamp(double(rgba[i]), 0.0, 1.0) * sa * 255.0 +
                        p[i] * om), 0L, 255L));
    p[3] = static_cast<uint8_t>(
        std::clamp(std::lround(sa * 255.0 + p[3] * om), 0L, 255L));
}

void refStroke(std::vector<uint8_t>& buf, int w, int h, const float rgba[4],
               double diameter, double x0, double y0, double x1, double y1) {
    double d = (diameter == 0.0 || std::isnan(diameter)) ? 1.0 : diameter;
    double width = double(static_cast<float>(d));
    if (!std::isfinite(width) || width < 0) return;
    double off = (d == 1.0 || d == 3.0) ? 0.5 : 0.0;
    float fx0 = static_cast<float>(x0 + off);
    float fy0 = static_cast<float>(y0 + off);
    float fdx = static_cast<float>(x1 - x0), fdy = static_cast<float>(y1 - y0);
    if (!std::isfinite(fx0) || !std::isfinite(fy0) || !std::isfinite(fdx) ||
        !std::isfinite(fdy)) return;
    double dx = double(fdx), dys = -double(fdy);
    double L = double(static_cast<float>(std::hypot(x1 - x0, y1 - y0)));
    if (!std::isfinite(L)) return;
    double eps = double(static_cast<float>(1e-3));
    double den = std::max(L, eps);
    double nx = (std::abs(dx) < eps && std::abs(dys) < eps ? eps : dx) / den;
    double ny = dys / den;
    double inv = 1.0 / (nx * nx + ny * ny);
    double ax = 240.0 + fx0, ay = 180.0 - fy0;
    double ex = ax + nx * L, ey = ay + ny * L;
    double rad = width / 2.0, sup = (rad + 0.5) * std::hypot(nx, ny);
    if (std::max(ax, ex) + sup - 0.5 < 0.0 ||
        std::min(ax, ex) - sup - 0.5 >= double(w) ||
        std::max(ay, ey) + sup - 0.5 < 0.0 ||
        std::min(ay, ey) - sup - 0.5 >= double(h)) return;
    int xlo = int(std::ceil(std::max(0.0, std::min(ax, ex) - sup - 0.5)));
    int xhi = int(std::floor(
        std::min(double(w - 1), std::max(ax, ex) + sup - 0.5)));
    int ylo = int(std::ceil(std::max(0.0, std::min(ay, ey) - sup - 0.5)));
    int yhi = int(std::floor(
        std::min(double(h - 1), std::max(ay, ey) + sup - 0.5)));
    for (int y = ylo; y <= yhi; ++y)
        for (int x = xlo; x <= xhi; ++x) {
            double dX = x + 0.5 - ax, dY = y + 0.5 - ay;
            double al = (nx * dX + ny * dY) * inv;
            double pp = (-ny * dX + nx * dY) * inv;
            double dist = std::hypot(al - std::clamp(al, 0.0, L), pp);
            double cov = std::clamp(rad + 0.5 - dist, 0.0, 1.0);
            if (cov > 0.0)
                refPremult(&buf[(static_cast<size_t>(y) * w + x) * 4], rgba,
                           cov);
        }
}

int penNonZero(const uint8_t* buf, int w, int h) {
    int n = 0;
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; i += 4)
        if (buf[i + 3]) ++n;
    return n;
}

void checkPixSet(const uint8_t* buf, int w, int h,
                 std::initializer_list<std::array<int, 3>> px, int tol) {
    int listed = 0;
    for (const auto& e : px) {
        const uint8_t* p =
            &buf[(static_cast<size_t>(e[1]) * w + e[0]) * 4];
        CHECK(std::abs(int(p[3]) - e[2]) <= tol);
        CHECK(std::abs(int(p[0]) - int(p[3])) <= tol);
        CHECK_EQ(p[1], 0);
        CHECK_EQ(p[2], 0);
        ++listed;
    }
    CHECK_EQ(penNonZero(buf, w, h), listed);
}

const uint8_t* pxAt(const uint8_t* buf, int w, int x, int y) {
    return &buf[(static_cast<size_t>(y) * w + x) * 4];
}

int test_pen_points_gpu() {
    Runtime rt;
    CHECK(loadProject(rt, "[]"));
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int skin = ren.createPenSkin();
    CHECK(skin >= 0);
    const int w = TA::penW(ren), h = TA::penH(ren);
    CHECK_EQ(w, 480);
    CHECK_EQ(h, 360);
    uint8_t* buf = TA::penPx(ren);
    const float red[4] = {1, 0, 0, 1};
    const double sq2 = 1.4142135623730951;

    ren.penPoint(skin, red, 1.0, 0.0, 0.0);
    checkPixSet(buf, w, h, {{240, 179, 255}}, 0);

    ren.penClear(skin);
    ren.penPoint(skin, red, sq2, 0.0, 0.0);
    checkPixSet(buf, w, h,
                {{239, 179, 127}, {240, 179, 127}, {239, 180, 127},
                 {240, 180, 127}}, 1);

    ren.penClear(skin);
    ren.penPoint(skin, red, sq2, 0.5, 0.5);
    checkPixSet(buf, w, h,
                {{240, 179, 255}, {239, 179, 53}, {241, 179, 53},
                 {240, 178, 53}, {240, 180, 53}}, 1);

    ren.penClear(skin);
    ren.penPoint(skin, red, sq2, 0.25, 0.25);
    checkPixSet(buf, w, h,
                {{239, 179, 106}, {240, 179, 218}, {239, 180, 37},
                 {240, 180, 106}}, 1);

    ren.penClear(skin);
    ren.penPoint(skin, red, 3.0, 0.0, 0.0);
    checkPixSet(buf, w, h,
                {{240, 179, 255}, {239, 179, 255}, {241, 179, 255},
                 {240, 178, 255}, {240, 180, 255}, {239, 178, 149},
                 {241, 178, 149}, {239, 180, 149}, {241, 180, 149}}, 1);

    ren.penClear(skin);
    ren.penPoint(skin, red, std::nextafter(1.0, 2.0), 0.0, 0.0);
    checkPixSet(buf, w, h,
                {{239, 179, 75}, {240, 179, 75}, {239, 180, 75},
                 {240, 180, 75}}, 1);

    ren.penClear(skin);
    ren.penPoint(skin, red, std::nextafter(3.0, 4.0), 0.0, 0.0);
    checkPixSet(buf, w, h,
                {{240, 180, 255}, {239, 180, 255}, {240, 179, 255},
                 {239, 179, 255}, {238, 179, 107}, {241, 179, 107},
                 {238, 180, 107}, {241, 180, 107}, {239, 178, 107},
                 {240, 178, 107}, {239, 181, 107}, {240, 181, 107}}, 1);

    ren.penClear(skin);
    for (double d : {1.0, sq2, 2.7})
        for (double px : {0.3, -0.4}) {
            ren.penPoint(skin, red, d, px, px * 2.0);
            std::vector<uint8_t> snap(buf, buf + size_t(w) * h * 4);
            ren.penClear(skin);
            ren.penLine(skin, red, d, px, px * 2.0, px, px * 2.0);
            CHECK(std::memcmp(buf, snap.data(), snap.size()) == 0);
            ren.penClear(skin);
        }

    std::printf("pen-gpu-pts: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

// A zero-length line is the generic-path oracle for the point specialization.
// Explicit readback makes this test valid on live GL as well as dummy/CPU.
int test_pen_point_record_parity(int stageWidth = 480, int stageHeight = 360) {
    Runtime rt;
    CHECK(loadProject(rt, "[]"));
    rt.stageWidth = stageWidth;
    rt.stageHeight = stageHeight;
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int skin = ren.createPenSkin();
    const size_t bytes = size_t(TA::penW(ren)) * TA::penH(ren) * 4;
    const float color[4] = {0.2f, 0.7f, 0.4f, 0.375f};
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double huge = std::numeric_limits<double>::max();
    const double tiny = std::numeric_limits<double>::denorm_min();
    auto compare = [&](double d, double x, double y) {
        ren.penClear(skin);
        const auto beforePoint = penStats();
        ren.penPoint(skin, color, d, x, y);
        const auto afterPoint = penStats();
        const uint8_t* p = TA::penPx(ren);
        std::vector<uint8_t> snap(p, p + bytes);
        ren.penClear(skin);
        const auto beforeLine = penStats();
        ren.penLine(skin, color, d, x, y, x, y);
        const auto afterLine = penStats();
        CHECK(std::memcmp(TA::penPx(ren), snap.data(), bytes) == 0);
        CHECK_EQ(afterPoint.strokes - beforePoint.strokes,
                 afterLine.strokes - beforeLine.strokes);
        CHECK_EQ(afterPoint.quads - beforePoint.quads,
                 afterLine.quads - beforeLine.quads);
    };
    for (double d : {0.0, -0.0, nan, inf, -inf, huge, -1.0, tiny, -tiny,
                     1.0, std::nextafter(1.0, 0.0), std::nextafter(1.0, 2.0),
                     3.0, std::nextafter(3.0, 2.0), std::nextafter(3.0, 4.0),
                     2.7, 12.5})
        for (const auto& xy : {std::pair{0.0, -0.0}, std::pair{0.25, -0.75},
                               std::pair{-240.0, 180.0}})
            compare(d, xy.first, xy.second);
    for (double v : {nan, inf, -inf, huge, -huge}) {
        compare(1.0, v, 0.0);
        compare(1.0, 0.0, v);
    }
    // Cross the append flush threshold; alternate with generic lines to check
    // order, then exercise readback and discarding a pending batch via clear.
    for (int n : {99999, 100000, 100001}) {
        auto feed = [&](bool points) {
            ren.penClear(skin);
            for (int i = 0; i < n; ++i) {
                double x = (i % 480) - 240.0, y = ((i / 480) % 360) - 180.0;
                const float alternate[4] = {0.8f, 0.1f, 0.9f, 0.625f};
                const float* brush = (i % 2) ? color : alternate;
                if (points && (i % 3)) ren.penPoint(skin, brush, 2.7, x, y);
                else ren.penLine(skin, brush, 2.7, x, y, x, y);
            }
        };
        feed(true);
        const uint8_t* p = TA::penPx(ren);
        std::vector<uint8_t> snap(p, p + bytes);
        feed(false);
        CHECK(std::memcmp(TA::penPx(ren), snap.data(), bytes) == 0);
        ren.penPoint(skin, color, 100, 0, 0);
        ren.penClear(skin);
        CHECK_EQ(penNonZero(TA::penPx(ren), TA::penW(ren), TA::penH(ren)), 0);
    }
    const auto before = penStats();
    ren.penPoint(skin + 100, color, 1, 0, 0);
    CHECK_EQ(penStats().strokes, before.strokes);
    CHECK_EQ(penStats().quads, before.quads);
    if (std::getenv("XYO_TEST_PEN_RECORDS")) {
        CHECK(penStats().quads > 0); // Do not silently validate CPU fallback.
        CHECK(penStats().flushes > 0);
        CHECK(penStats().records > 0); // Also reject the legacy quad path.
    }
    std::printf("pen-point-parity: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_pen_lines() {
    Runtime rt;
    CHECK(loadProject(rt, "[]"));
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int skin = ren.createPenSkin();
    CHECK(skin >= 0);
    const int w = TA::penW(ren), h = TA::penH(ren);
    uint8_t* buf = TA::penPx(ren);
    const float red[4] = {1, 0, 0, 1};

    ren.penLine(skin, red, 2.0, -10.0, 0.0, 10.0, 0.0);
    std::vector<uint8_t> exp(size_t(w) * h * 4, 0);
    refStroke(exp, w, h, red, 2.0, -10.0, 0.0, 10.0, 0.0);
    CHECK(std::memcmp(buf, exp.data(), exp.size()) == 0);
    CHECK_EQ(pxAt(buf, w, 240, 179)[3], 255);
    CHECK_EQ(pxAt(buf, w, 240, 180)[3], 255);
    CHECK_EQ(pxAt(buf, w, 240, 178)[3], 0);
    CHECK_EQ(pxAt(buf, w, 240, 181)[3], 0);
    CHECK_EQ(pxAt(buf, w, 229, 179)[3], 202);
    CHECK_EQ(pxAt(buf, w, 229, 180)[3], 202);
    CHECK_EQ(pxAt(buf, w, 228, 180)[3], 0);

    ren.penClear(skin);
    ren.penLine(skin, red, 2.0, -10.0, -10.0, 10.0, 10.0);
    std::fill(exp.begin(), exp.end(), 0);
    refStroke(exp, w, h, red, 2.0, -10.0, -10.0, 10.0, 10.0);
    CHECK(std::memcmp(buf, exp.data(), exp.size()) == 0);
    CHECK_EQ(pxAt(buf, w, 240, 180)[3], 202);
    CHECK_EQ(pxAt(buf, w, 230, 190)[3], 202);
    CHECK_EQ(pxAt(buf, w, 240, 179)[3], 255);
    CHECK_EQ(pxAt(buf, w, 241, 180)[3], 22);

    ren.penClear(skin);
    ren.penLine(skin, red, 2.0, 0.0, 0.0, 5e-4, 5e-4);
    std::fill(exp.begin(), exp.end(), 0);
    refStroke(exp, w, h, red, 2.0, 0.0, 0.0, 5e-4, 5e-4);
    CHECK(std::memcmp(buf, exp.data(), exp.size()) == 0);

    const float half[4] = {0, 1, 0, 0.5f};
    ren.penClear(skin);
    ren.penLine(skin, half, 2.0, -10.0, 0.0, 10.0, 0.0);
    std::fill(exp.begin(), exp.end(), 0);
    refStroke(exp, w, h, half, 2.0, -10.0, 0.0, 10.0, 0.0);
    CHECK(std::memcmp(buf, exp.data(), exp.size()) == 0);

    ren.penClear(skin);
    ren.penLine(skin, red, 2.0, -1e20, 0.0, 1e20, 0.0);
    CHECK_EQ(pxAt(buf, w, 240, 179)[3], 255);
    CHECK_EQ(pxAt(buf, w, 240, 180)[3], 255);
    CHECK_EQ(pxAt(buf, w, 240, 178)[3], 0);
    CHECK_EQ(pxAt(buf, w, 240, 181)[3], 0);
    CHECK_EQ(penNonZero(buf, w, h), 960);

    std::printf("pen-lines: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_pen_blend_and_robust() {
    Runtime rt;
    CHECK(loadProject(rt, "[]"));
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int skin = ren.createPenSkin();
    CHECK(skin >= 0);
    const int w = TA::penW(ren), h = TA::penH(ren);
    uint8_t* buf = TA::penPx(ren);
    const float red[4] = {1, 0, 0, 1};

    const int bx = w / 2 + 3, by = h / 2 - 5;
    uint8_t* cell = &buf[(static_cast<size_t>(by) * w + bx) * 4];
    const uint8_t alphas[] = {0, 1, 127, 254, 255};
    const uint8_t srcColors[][3] = {{0, 0, 0}, {255, 255, 255}, {17, 128, 243}};
    const uint8_t dsts[][4] = {{0, 0, 0, 0}, {64, 64, 64, 128},
                               {200, 100, 50, 255}, {0, 0, 0, 255},
                               {10, 20, 30, 40}};
    for (uint8_t sa : alphas)
        for (const auto& sc : srcColors)
            for (const auto& dc : dsts) {
                uint8_t c[4] = {sc[0], sc[1], sc[2], sa};
                float rgba[4] = {sc[0] / 255.0f, sc[1] / 255.0f,
                                 sc[2] / 255.0f, sa / 255.0f};
                uint8_t ref[4] = {dc[0], dc[1], dc[2], dc[3]};
                refPremult(ref, rgba, 1.0);
                std::memcpy(cell, dc, 4);
                TA::penBlendPx(ren, bx, by, c);
                for (int i = 0; i < 4; ++i) CHECK_EQ(cell[i], ref[i]);
            }

    std::vector<uint8_t> snap(buf, buf + size_t(w) * h * 4);
    const uint8_t opaque[4] = {11, 22, 33, 255};
    const uint8_t translucent[4] = {44, 55, 66, 127};
    for (const uint8_t* c : {opaque, translucent}) {
        TA::penBlendPx(ren, -1, 0, c);
        TA::penBlendPx(ren, 0, -1, c);
        TA::penBlendPx(ren, w, 0, c);
        TA::penBlendPx(ren, 0, h, c);
        CHECK(std::memcmp(buf, snap.data(), snap.size()) == 0);
    }

    ren.penClear(skin);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            ren.penPoint(skin, red, 1.0, i, j);
    CHECK_EQ(penNonZero(buf, w, h), 25);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            const uint8_t* p = pxAt(buf, w, 240 + i, 179 - j);
            CHECK_EQ(p[0], 255);
            CHECK_EQ(p[1], 0);
            CHECK_EQ(p[2], 0);
            CHECK_EQ(p[3], 255);
        }

    ren.penClear(skin);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    ren.penPoint(skin, red, nan, 0.0, 0.0);
    checkPixSet(buf, w, h, {{240, 179, 255}}, 0);
    ren.penClear(skin);
    ren.penPoint(skin, red, inf, 0.0, 0.0);
    ren.penPoint(skin, red, -2.0, 0.0, 0.0);
    ren.penPoint(skin, red, 1.0, 10000.0, 0.0);
    ren.penPoint(skin, red, 1.0, nan, 0.0);
    ren.penPoint(skin, red, 1.0, 0.0, inf);
    ren.penLine(skin, red, 1.0, 0.0, 0.0, inf, 0.0);
    ren.penLine(skin, red, 1.0, 0.0, 0.0, nan, nan);
    CHECK_EQ(penNonZero(buf, w, h), 0);

    ren.penClear(skin);
    ren.penPoint(skin, red, 1.0, 0.9, 0.9);
    CHECK_EQ(pxAt(buf, w, 240, 179)[3], 0);
    CHECK(std::abs(int(pxAt(buf, w, 241, 178)[3]) - 219) <= 1);
    CHECK(penNonZero(buf, w, h) > 0);

    std::printf("pen-blend+robust: %d checks, %d failures\n", g_checks,
                g_fail);
    return g_fail;
}

int test_pen_alpha_accum() {
    Runtime rt;
    CHECK(loadProject(rt, "[]"));
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int skin = ren.createPenSkin();
    CHECK(skin >= 0);
    const int w = TA::penW(ren);
    uint8_t* buf = TA::penPx(ren);

    const float halfBlue[4] = {0, 0, 1, 0.5f};
    ren.penPoint(skin, halfBlue, 1.0, 0.0, 0.0);
    const uint8_t* p = pxAt(buf, w, 240, 179);
    CHECK_EQ(p[0], 0);
    CHECK_EQ(p[1], 0);
    CHECK_EQ(p[2], 128);
    CHECK_EQ(p[3], 128);

    ren.penPoint(skin, halfBlue, 1.0, 0.0, 0.0);
    CHECK_EQ(p[0], 0);
    CHECK_EQ(p[1], 0);
    CHECK(std::abs(int(p[2]) - 192) <= 1);
    CHECK(std::abs(int(p[3]) - 192) <= 1);

    ren.penClear(skin);
    const float c[4] = {1, 0.5f, 0.25f, 1};
    ren.penPoint(skin, halfBlue, 1.0, 0.0, 0.0);
    ren.penPoint(skin, c, 1.0, 0.0, 0.0);
    CHECK_EQ(p[0], 255);
    CHECK_EQ(p[1], 128);
    CHECK_EQ(p[2], 64);
    CHECK_EQ(p[3], 255);
    CHECK_EQ(penNonZero(buf, w, TA::penH(ren)), 1);

    std::printf("pen-alpha: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

template <typename F>
int countPix(const uint8_t* px, int w, int x0, int y0, int x1, int y1, F f) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (f(&px[(static_cast<size_t>(y) * w + x) * 4])) ++n;
    return n;
}

void dumpStage(SdlRenderer& ren, const char* path) {
    if (!std::getenv("XYO_SVG_DUMP")) return;
    ren.draw();
    CHECK(ren.savePPM(path));
}

auto svgSkin(SdlRenderer& ren, Target* sp, int i) {
    sp->currentCostume = i;
    double x = 1000.0, y = 1000.0;
    ren.getFencedPosition(sp->drawableID, x, y, true);
    return TA::skin(ren, &sp->sprite->costumes[i]);
}

bool loadSvgCostumes(Runtime& rt, int n, int rw, int rh) {
    std::string costumes = "[";
    for (int i = 0; i < n; ++i) {
        if (i) costumes += ",";
        costumes += "{\"assetId\":\"c" + std::to_string(i) +
                    "\",\"name\":\"c" + std::to_string(i) +
                    "\",\"dataFormat\":\"svg\",\"md5ext\":\"c" +
                    std::to_string(i) + ".svg\",\"bitmapResolution\":1,"
                    "\"rotationCenterX\":" + std::to_string(rw) +
                    ",\"rotationCenterY\":" + std::to_string(rh) + "}";
    }
    costumes += "]";
    return loadProject(rt, costumes.c_str());
}

int test_svg_text_render() {
    Runtime rt;
    CHECK(loadSvgCostumes(rt, 5, 0, 0));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgTextRed, std::strlen(kSvgTextRed));
    setCostumeData(sp, 1, kSvgTextTspan, std::strlen(kSvgTextTspan));
    setCostumeData(sp, 2, kSvgTextOpacity, std::strlen(kSvgTextOpacity));
    setCostumeData(sp, 3, kSvgTextJp, std::strlen(kSvgTextJp));
    setCostumeData(sp, 4, kSvgTextStroke, std::strlen(kSvgTextStroke));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

#if !XYO_TEST_RSVG
    // nanosvg フォールバックは <text>/<tspan> を描かないため、画素検査は
    // librsvg ビルドでのみ行う。ここでは「文字だけのコスチュームでも
    // 採用された寸法でデコードされ、キャッシュに載る」ことだけ検証する。
    for (int i = 0; i < 5; ++i) CHECK(svgSkin(ren, sp, i) != nullptr);
    CHECK_EQ(TA::skinCount(ren), 5u);
    std::printf("svg-text: %d checks, %d failures (nanosvg: 画素検査スキップ)\n",
                g_checks, g_fail);
    return g_fail;
#endif

    const auto* s = svgSkin(ren, sp, 0);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 320);
        CHECK_EQ(s->h, 120);
        const uint8_t* px = s->px.data();
        auto red = [](const uint8_t* p) {
            return p[3] > 0 && p[0] > 128 && p[1] < 80 && p[2] < 80;
        };
        CHECK(countPix(px, s->w, 0, 10, s->w, 110, red) > 0);
        CHECK_EQ(countPix(px, s->w, 0, 0, s->w, 10,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        CHECK_EQ(countPix(px, s->w, 0, 110, s->w, 120,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        CHECK_EQ(countPix(px, s->w, 312, 0, 320, 120,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        CHECK_EQ(countPix(px, s->w, 0, 0, s->w, 120,
                          [](const uint8_t* p) {
                              return p[3] > 0 &&
                                     (p[0] != 255 || p[1] != 0 || p[2] != 0);
                          }), 0);
    }

    s = svgSkin(ren, sp, 1);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 480);
        CHECK_EQ(s->h, 240);
        const uint8_t* px = s->px.data();
        auto red = [](const uint8_t* p) {
            return p[3] > 0 && p[0] > 128 && p[1] < 96 && p[2] < 96;
        };
        auto blue = [](const uint8_t* p) {
            return p[3] > 0 && p[2] > 128 && p[0] < 96 && p[1] < 96;
        };
        CHECK(countPix(px, s->w, 0, 16, s->w, 80, red) > 0);
        CHECK(countPix(px, s->w, 0, 88, s->w, 150, blue) > 0);
        CHECK_EQ(countPix(px, s->w, 0, 16, s->w, 80, blue), 0);
        CHECK_EQ(countPix(px, s->w, 0, 88, s->w, 150, red), 0);
        CHECK_EQ(countPix(px, s->w, 0, 0, s->w, 10,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        CHECK_EQ(countPix(px, s->w, 0, 230, s->w, 240,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        std::vector<uint8_t> snap(px, px + static_cast<size_t>(s->w) * s->h * 4);
        ren.draw();
        const auto* s2 = TA::skin(ren, &sp->sprite->costumes[1]);
        CHECK_EQ(s2, s);
        CHECK_EQ(TA::skinCount(ren), 2u);
        CHECK(std::memcmp(px, snap.data(), snap.size()) == 0);
        dumpStage(ren, "/tmp/xyo_svg_tspan.ppm");
    }

    s = svgSkin(ren, sp, 2);
    CHECK(s != nullptr);
    if (s) {
        const uint8_t* px = s->px.data();
        int maxA = 0;
        for (size_t i = 3; i < s->px.size(); i += 4)
            if (px[i] > maxA) maxA = px[i];
        CHECK(maxA >= 96 && maxA <= 160);
        int opaqueGreen = countPix(px, s->w, 0, 0, s->w, s->h,
                                   [](const uint8_t* p) {
                                       return p[3] >= 96 && p[1] > 200 &&
                                              p[0] < 80 && p[2] < 80;
                                   });
        CHECK(opaqueGreen > 0);
    }

    s = svgSkin(ren, sp, 3);
    CHECK(s != nullptr);
    if (s) {
        const uint8_t* px = s->px.data();
        int n = countPix(px, s->w, 0, 0, s->w, s->h,
                         [](const uint8_t* p) {
                             return p[3] > 0 && p[0] < 80 && p[1] < 80 &&
                                    p[2] < 80;
                         });
        CHECK(n > 0);
        dumpStage(ren, "/tmp/xyo_svg_jp.ppm");
    }

    s = svgSkin(ren, sp, 4);
    CHECK(s != nullptr);
    if (s) {
        const uint8_t* px = s->px.data();
        int n = countPix(px, s->w, 0, 0, s->w, s->h,
                         [](const uint8_t* p) {
                             return p[3] > 0 && p[0] > 128 && p[1] < 96 &&
                                    p[2] < 96;
                         });
        CHECK(n > 0);
    }
    CHECK_EQ(TA::skinCount(ren), 5u);
    std::printf("svg-text: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_svg_anchor_and_viewbox() {
    Runtime rt;
    CHECK(loadSvgCostumes(rt, 2, 0, 0));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgTextAnchor, std::strlen(kSvgTextAnchor));
    setCostumeData(sp, 1, kSvgViewBox, std::strlen(kSvgViewBox));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    const auto* s = svgSkin(ren, sp, 0);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 480);
        CHECK_EQ(s->h, 160);
#if XYO_TEST_RSVG
        const uint8_t* px = s->px.data();
        auto red = [](const uint8_t* p) {
            return p[3] > 0 && p[0] > 128 && p[1] < 96 && p[2] < 96;
        };
        CHECK(countPix(px, s->w, 0, 0, 235, 160, red) > 0);
        CHECK(countPix(px, s->w, 245, 0, 480, 160, red) > 0);
#endif
    }

    s = svgSkin(ren, sp, 1);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 200);
        CHECK_EQ(s->h, 120);
        const uint8_t* px = s->px.data();
        CHECK(countPix(px, s->w, 0, 0, 50, 30,
                       [](const uint8_t* p) {
                           return p[3] > 0 && p[0] > 128;
                       }) > 0);
        CHECK_EQ(countPix(px, s->w, 50, 0, 200, 120,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
        CHECK_EQ(countPix(px, s->w, 0, 30, 50, 120,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
    }
    std::printf("svg-anchor+viewbox: %d checks, %d failures\n", g_checks,
                g_fail);
    return g_fail;
}

int test_svg_bad_decode() {
    Runtime rt;
    CHECK(loadSvgCostumes(rt, 5, 0, 0));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgBroken, std::strlen(kSvgBroken));
    setCostumeData(sp, 1, kSvgJunk, std::strlen(kSvgJunk));
    setCostumeData(sp, 2, kSvgUnsized, std::strlen(kSvgUnsized));
    setCostumeData(sp, 3, kSvgBadViewBox, std::strlen(kSvgBadViewBox));
    setCostumeData(sp, 4, kSvgNegDim, std::strlen(kSvgNegDim));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    for (int i = 0; i < 5; ++i) {
        const auto* s = svgSkin(ren, sp, i);
#if XYO_TEST_RSVG
        CHECK(s == nullptr);
#else
        // nanosvg は viewBox の無い SVG を図形のバウンディングボックスから
        // 採寸するため、librsvg が拒否する入力（kSvgUnsized）も成功し得る。
        // 壊れた寸法のスキンが描画へ渡らないことだけ検証する。
        if (s) CHECK(s->w > 0 && s->h > 0);
#endif
        CHECK(TA::cached(ren, &sp->sprite->costumes[i]));
    }
    CHECK_EQ(TA::skinCount(ren), 5u);
    sp->currentCostume = 0;
    double x = 1000.0, y = 1000.0;
    ren.getFencedPosition(sp->drawableID, x, y, true);
    ren.draw();
    CHECK_EQ(TA::skinCount(ren), 5u);
    std::printf("svg-bad: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_svg_text_usage() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"u","name":"u",
        "dataFormat":"svg","md5ext":"u.svg","bitmapResolution":1,
        "rotationCenterX":50,"rotationCenterY":25}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
#if XYO_TEST_RSVG
    const char* kUse =
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><text x="8" y="38" font-size="30" fill="#ff0000">AB</text></svg>)SVG";
#else
    // nanosvg フォールバックは <text> を描かないため、同じ 100x50 を塗る
    // 矩形で pick / touchingColor / stamp の経路を検証する。
    const char* kUse =
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><rect x="0" y="0" width="100" height="50" fill="#ff0000"/></svg>)SVG";
#endif
    setCostumeData(sp, 0, kUse, std::strlen(kUse));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    const int penSkin = ren.createPenSkin();
    const int id = sp->drawableID;
    const auto* s = svgSkin(ren, sp, 0);
    CHECK(s != nullptr);
    if (!s) return g_fail;

    int ix = -1, iy = -1;
    for (int y = 0; y < s->h && ix < 0; ++y)
        for (int x = 0; x < s->w; ++x) {
            const uint8_t* p =
                &s->px[(static_cast<size_t>(y) * s->w + x) * 4];
            if (p[3] >= 200 && p[0] > 200) { ix = x; iy = y; break; }
        }
    CHECK(ix >= 0);
    if (ix < 0) return g_fail;
    double px = 240.0 + (ix + 0.5) / s->scale - s->rcx;
    double py = 180.0 + (iy + 0.5) / s->scale - s->rcy;

    CHECK_EQ(ren.pick(px, py), id);

    const float red[4] = {1, 0, 0, 1};
    ren.penPoint(penSkin, red, 60.0, px - 240.0, 180.0 - py);
    CHECK(ren.isTouchingColor(id, 0xFF0000));
    CHECK(!ren.isTouchingColor(id, 0x00FF00));

    ren.penClear(penSkin);
    ren.penStamp(penSkin, id);
    uint8_t* pen = TA::penPx(ren);
    const uint8_t* q = pxAt(pen, TA::penW(ren),
                            static_cast<int>(px), static_cast<int>(py));
    CHECK(q[0] > 200 && q[1] < 80 && q[2] < 80 && q[3] > 200);

    sp->visible = false;
    CHECK_EQ(ren.pick(px, py), -1);
    CHECK(!ren.isTouchingColor(id, 0xFF0000));
    sp->visible = true;
    std::printf("svg-usage: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_fence_empty_point() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"a","name":"a",
        "dataFormat":"svg","md5ext":"a.svg","bitmapResolution":1,
        "rotationCenterX":10,"rotationCenterY":5}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    const double corners[4][2] = {
        {240, 180}, {240, -180}, {-240, 180}, {-240, -180}};
    for (auto& c : corners) {
        sp->setXY(c[0], c[1]);
        CHECK_EQ(sp->x, c[0]);
        CHECK_EQ(sp->y, c[1]);
    }
    sp->setXY(239.5, -179.5);
    CHECK_EQ(sp->x, 239.5);
    CHECK_EQ(sp->y, -179.5);
    sp->setXY(0, 0);
    sp->setXY(1000, 1000);
    CHECK_EQ(sp->x, 240.0);
    CHECK_EQ(sp->y, 180.0);
    sp->setXY(0, 0);
    sp->setXY(-1000, -1000);
    CHECK_EQ(sp->x, -240.0);
    CHECK_EQ(sp->y, -180.0);
    sp->setXY(67.5, -31.25);
    sp->setXY(1000, -1000);
    CHECK_EQ(sp->x, 240.0);
    CHECK_EQ(sp->y, -180.0);
    sp->visible = false;
    sp->setXY(0, 0);
    sp->setXY(-1000, 1000);
    CHECK_EQ(sp->x, -240.0);
    CHECK_EQ(sp->y, 180.0);
    sp->visible = true;

    Target* c = sp->makeClone();
    CHECK(c != nullptr);
    CHECK_EQ(TA::skinCount(ren), 1u);
    if (c) {
        c->setXY(1000, -1000);
        CHECK_EQ(c->x, 240.0);
        CHECK_EQ(c->y, -180.0);
        CHECK_EQ(TA::skinCount(ren), 1u);
        c->dispose();
        delete c;
    }
    sp->setXY(0, 0);
    std::printf("fence-empty: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_fence_aabb() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([
        {"assetId":"c","name":"c","dataFormat":"svg","md5ext":"c.svg",
         "bitmapResolution":1,"rotationCenterX":10,"rotationCenterY":5},
        {"assetId":"t","name":"t","dataFormat":"svg","md5ext":"t.svg",
         "bitmapResolution":1,"rotationCenterX":0,"rotationCenterY":0}
    ])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgRect, std::strlen(kSvgRect));
    setCostumeData(sp, 1, kSvgRect, std::strlen(kSvgRect));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    struct Case {
        int costume;
        double dir;
        const char* style;
        double size;
        double x, y;
        double nx, ny;
        double cx, cy;
        double cnx, cny;
    };
    const Case cases[] = {
        {0,  90, "all around",  100,  245, 180, -245, -180,  240, 180, -240, -180},
        {1,  90, "all around",  100,  235, 185, -255, -175,  235, 180, -240, -175},
        {1, -90, "left-right",  100,  255, 185, -235, -175,  240, 180, -235, -175},
        {0,   0, "all around",  100,  240, 185, -240, -185,  240, 180, -240, -180},
        {0,  90, "all around", 3000,  525, 315, -525, -315,  240, 180, -240, -180},
    };
    for (const Case& cs : cases) {
        sp->currentCostume = cs.costume;
        sp->direction = cs.dir;
        sp->rotationStyle = cs.style;
        sp->size = cs.size;
        sp->setXY(0, 0);
        sp->setXY(1000, 1000);
        CHECK_EQ(sp->x, cs.x);
        CHECK_EQ(sp->y, cs.y);
        sp->setXY(0, 0);
        sp->setXY(-1000, -1000);
        CHECK_EQ(sp->x, cs.nx);
        CHECK_EQ(sp->y, cs.ny);
        sp->setXY(0, 0);
        sp->setXY(240, 180);
        CHECK_EQ(sp->x, cs.cx);
        CHECK_EQ(sp->y, cs.cy);
        sp->setXY(-240, -180);
        CHECK_EQ(sp->x, cs.cnx);
        CHECK_EQ(sp->y, cs.cny);
        sp->setXY(0, 0);
    }
    sp->currentCostume = 0;
    sp->direction = 90;
    sp->rotationStyle = "all around";
    sp->size = 100;

    rt.fencing = false;
    sp->setXY(1000, 1000);
    CHECK_EQ(sp->x, 1000.0);
    CHECK_EQ(sp->y, 1000.0);
    rt.fencing = true;

    {
        Runtime rt2;
        rt2.stageWidth = 640;
        rt2.stageHeight = 400;
        CHECK(loadProject(rt2, R"JSON([{"assetId":"a","name":"a",
            "dataFormat":"svg","md5ext":"a.svg","bitmapResolution":1,
            "rotationCenterX":0,"rotationCenterY":0}])JSON"));
        Target* sp2 = spriteOf(rt2);
        CHECK(sp2 != nullptr);
        if (sp2) {
            setCostumeData(sp2, 0, kSvgZero, std::strlen(kSvgZero));
            SdlRenderer ren2;
            CHECK(ren2.init(rt2, 1));
            rt2.renderer = &ren2;
            sp2->setXY(1000, 1000);
            CHECK_EQ(sp2->x, 320.0);
            CHECK_EQ(sp2->y, 200.0);
            sp2->setXY(-1000, -1000);
            CHECK_EQ(sp2->x, -320.0);
            CHECK_EQ(sp2->y, -200.0);
        }
    }
    std::printf("fence-aabb: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

const char* kPenBorderProject = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {}, "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "clr",
               "parent": null, "inputs": {}, "fields": {}, "topLevel": true},
       "clr": {"opcode": "pen_clear", "next": "sz", "parent": "hat",
               "inputs": {}, "fields": {}},
       "sz": {"opcode": "pen_setPenSizeTo", "next": "col", "parent": "clr",
              "inputs": {"SIZE": [1, [4, "2"]]}, "fields": {}},
       "col": {"opcode": "pen_setPenColorToColor", "next": "g0",
               "parent": "sz",
               "inputs": {"COLOR": [1, [9, "#ff0000"]]}, "fields": {}},
       "g0": {"opcode": "motion_gotoxy", "next": "down", "parent": "col",
              "inputs": {"X": [1, [4, "-240"]], "Y": [1, [4, "-180"]]},
              "fields": {}},
       "down": {"opcode": "pen_penDown", "next": "g1", "parent": "g0",
                "inputs": {}, "fields": {}},
       "g1": {"opcode": "motion_gotoxy", "next": "g2", "parent": "down",
              "inputs": {"X": [1, [4, "240"]], "Y": [1, [4, "-180"]]},
              "fields": {}},
       "g2": {"opcode": "motion_gotoxy", "next": "g3", "parent": "g1",
              "inputs": {"X": [1, [4, "240"]], "Y": [1, [4, "180"]]},
              "fields": {}},
       "g3": {"opcode": "motion_gotoxy", "next": "g4", "parent": "g2",
              "inputs": {"X": [1, [4, "-240"]], "Y": [1, [4, "180"]]},
              "fields": {}},
       "g4": {"opcode": "motion_gotoxy", "next": "up", "parent": "g3",
              "inputs": {"X": [1, [4, "-240"]], "Y": [1, [4, "-180"]]},
              "fields": {}},
       "up": {"opcode": "pen_penUp", "next": null, "parent": "g4",
              "inputs": {}, "fields": {}}
     },
     "costumes": [
       {"assetId": "a", "name": "a", "dataFormat": "svg",
        "md5ext": "a.svg", "bitmapResolution": 1,
        "rotationCenterX": 0, "rotationCenterY": 0}
     ],
     "sounds": [], "currentCostume": 0,
     "x": 0, "y": 0, "size": 100, "direction": 90,
     "rotationStyle": "all around", "visible": false, "layerOrder": 1}
  ],
  "monitors": [], "extensions": ["pen"],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

int test_pen_border_fenced() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kPenBorderProject, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load: %s\n", r.error.c_str()); return g_fail; }
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    rt.greenFlag();
    const bool jitExpected =
        rt.jit != nullptr && std::getenv("XYO_JIT_DISABLE") == nullptr;
    if (jitExpected)
        for (Thread* t : rt.threads) CHECK(t->isCompiled);
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    CHECK(rt.threads.empty());
    CHECK_EQ(sp->x, -240.0);
    CHECK_EQ(sp->y, -180.0);

    const uint8_t* pen = TA::penPx(ren);
    const int w = TA::penW(ren), h = TA::penH(ren);
    CHECK_EQ(w, 480);
    CHECK_EQ(h, 360);
    auto redPx = [&](int x, int y) {
        const uint8_t* p = pxAt(pen, w, x, y);
        return p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255;
    };
    int rowTop = 0, rowBot = 0, colL = 0, colR = 0;
    for (int x = 0; x < w; ++x) {
        if (redPx(x, 0)) ++rowTop;
        if (redPx(x, h - 1)) ++rowBot;
    }
    for (int y = 0; y < h; ++y) {
        if (redPx(0, y)) ++colL;
        if (redPx(w - 1, y)) ++colR;
    }
    CHECK_EQ(rowTop, w);
    CHECK_EQ(rowBot, w);
    CHECK_EQ(colL, h);
    CHECK_EQ(colR, h);
    dumpStage(ren, "/tmp/xyo_pen_border.ppm");
    std::printf("pen-border: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

int test_pen_full_coverage() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"a","name":"a",
        "dataFormat":"svg","md5ext":"a.svg","bitmapResolution":1,
        "rotationCenterX":0,"rotationCenterY":0}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));
    sp->visible = false;

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    penrt::clear(&rt);
    penrt::size(sp, &rt, Value::num(2.0), false);
    penrt::setColorToColor(sp, Value::string(SStr::create(u"#ff0000")));
    for (int row = 0; row < 360; ++row) {
        double y = 180.0 - (row + 0.5);
        penrt::up(sp, &rt);
        sp->setXY(-240, y);
        penrt::down(sp, &rt);
        sp->setXY(240, y);
        penrt::up(sp, &rt);
    }
    const uint8_t* pen = TA::penPx(ren);
    const int w = TA::penW(ren), h = TA::penH(ren);
    CHECK_EQ(w, 480);
    CHECK_EQ(h, 360);
    int red = 0;
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = pen + i * 4;
        if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) ++red;
    }
    CHECK_EQ(red, 480 * 360);
    dumpStage(ren, "/tmp/xyo_pen_full.ppm");
    std::printf("pen-full: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

template <typename F>
int ppmCount(const char* path, F f) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return -1;
    int w = 0, h = 0, mx = 0;
    if (std::fscanf(fp, "P6 %d %d %d", &w, &h, &mx) != 3 || w <= 0 || h <= 0) {
        std::fclose(fp);
        return -1;
    }
    std::fgetc(fp);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    size_t got = std::fread(px.data(), 3, static_cast<size_t>(w) * h, fp);
    std::fclose(fp);
    if (got != static_cast<size_t>(w) * h) return -1;
    int n = 0;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        if (f(&px[i * 3])) ++n;
    return n;
}

int test_frame_capture() {
    Runtime rt;
    CHECK(loadProject(rt, R"JSON([{"assetId":"a","name":"a",
        "dataFormat":"svg","md5ext":"a.svg","bitmapResolution":1,
        "rotationCenterX":0,"rotationCenterY":0}])JSON"));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    setCostumeData(sp, 0, kSvgZero, std::strlen(kSvgZero));
    sp->visible = false;

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    const char* p1 = "/tmp/xyo_frame_capture_a.ppm";
    const char* p2 = "/tmp/xyo_frame_capture_b.ppm";
    int calls = 0;
    ren.setFrameCapture([&] {
        ++calls;
        CHECK(ren.savePPM(calls == 1 ? p1 : p2));
    });
    ren.draw();
    CHECK_EQ(calls, 1);

    penrt::clear(&rt);
    penrt::size(sp, &rt, Value::num(8.0), false);
    penrt::setColorToColor(sp, Value::string(SStr::create(u"#ff0000")));
    sp->setXY(0, 0);
    penrt::down(sp, &rt);
    penrt::up(sp, &rt);

    ren.draw();
    CHECK_EQ(calls, 2);
    ren.setFrameCapture(nullptr);
    ren.draw();
    CHECK_EQ(calls, 2);

    auto isRed = [](const uint8_t* p) {
        return p[0] > 200 && p[1] < 60 && p[2] < 60;
    };
    auto isWhite = [](const uint8_t* p) {
        return p[0] == 255 && p[1] == 255 && p[2] == 255;
    };
    CHECK(ppmCount(p1, isWhite) > 0);
    CHECK_EQ(ppmCount(p1, isRed), 0);
    CHECK(ppmCount(p2, isRed) > 0);

    std::printf("frame-capture: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}

// 逆順合成（遅延バッチ + (T,P) 半群 + 早期終了）の回帰テスト。
// 強制的に RC 経路を通し、前向き逐次合成との差が知覚閾値内であることを
// 検証する。順序（描画順）が保たれることも併せて確認する。
int test_pen_reverse_composite() {
    const float cols[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.5f, 1.0f, 0.5f},
        {0.2f, 0.9f, 0.3f, 0.25f},
        {1.0f, 1.0f, 1.0f, 1.0f},
    };
    auto run = [&](bool rc, std::vector<uint8_t>& out) {
        if (rc) setenv("XYO_PEN_RC", "1", 1);
        else setenv("XYO_PEN_RC", "0", 1);
        Runtime rt;
        if (!loadProject(rt, "[]")) return false;
        SdlRenderer ren;
        if (!ren.init(rt, 1)) return false;
        rt.renderer = &ren;
        const int skin = ren.createPenSkin();
        if (skin < 0) return false;
        // 400 本を狭い領域へ重ねる（バッチ閾値 256 を超え、重なりも深い）
        for (int i = 0; i < 400; ++i) {
            const float* c = cols[i % 4];
            const double x = -20.0 + double((i * 37) % 40);
            const double y = -20.0 + double((i * 53) % 40);
            ren.penLine(skin, c, 3.0 + double(i % 5), x, y, x + 6.0,
                        y + 4.0);
        }
        const uint8_t* p = TA::penPx(ren);
        out.assign(p, p + size_t(TA::penW(ren)) * TA::penH(ren) * 4);
        return true;
    };
    std::vector<uint8_t> fwd, rev;
    const bool ok1 = run(false, fwd);
    const bool ok2 = run(true, rev);
    unsetenv("XYO_PEN_RC");
    CHECK(ok1);
    CHECK(ok2);
    if (!ok1 || !ok2) return g_fail;
    CHECK_EQ(fwd.size(), rev.size());
    int maxErr = 0;
    long sumErr = 0, nz = 0;
    for (size_t i = 0; i < fwd.size(); i += 4) {
        for (int c = 0; c < 4; ++c) {
            const int e = std::abs(int(fwd[i + c]) - int(rev[i + c]));
            if (e > maxErr) maxErr = e;
            sumErr += e;
        }
        if (fwd[i + 3]) ++nz;
    }
    // 描画されている（バッチ経路が実際に走った）
    CHECK(nz > 1000);
    // 知覚できない差（タッチ色判定の許容 ±5 以内）
    CHECK(maxErr <= 4);
    CHECK(sumErr / long(fwd.size()) <= 2);
    std::printf("pen-reverse: %d checks, %d failures (maxErr=%d mean=%.3f)\n",
                g_checks, g_fail, maxErr, double(sumErr) / double(fwd.size()));
    return g_fail;
}

#if !XYO_TEST_RSVG
// librsvg の無いビルド（Windows 等）の nanosvg フォールバックの回帰テスト。
// 図形は描かれ、文字は描かれない（nanosvg の既知の制約）ことを固定する。
int test_svg_nanosvg_fallback() {
    Runtime rt;
    CHECK(loadSvgCostumes(rt, 3, 0, 0));
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return g_fail;
    const char* kShape =
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10"><rect x="0" y="0" width="20" height="10" fill="#ff0000"/><circle cx="5" cy="5" r="2" fill="#00ff00"/></svg>)SVG";
    const char* kTextOnly =
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="30"><text x="4" y="24" font-size="20" fill="#ff0000">AB</text></svg>)SVG";
    setCostumeData(sp, 0, kShape, std::strlen(kShape));
    setCostumeData(sp, 1, kTextOnly, std::strlen(kTextOnly));
    setCostumeData(sp, 2, kSvgUnsized, std::strlen(kSvgUnsized));

    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;

    const auto* s = svgSkin(ren, sp, 0);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 40);   // librsvg 経路と同じ 2x ラスタ
        CHECK_EQ(s->h, 20);
        CHECK_EQ(int(s->scale), 2);
        const uint8_t* px = s->px.data();
        CHECK(countPix(px, s->w, 0, 0, s->w, s->h,
                       [](const uint8_t* p) {
                           return p[3] > 200 && p[0] > 200 && p[1] < 60 &&
                                  p[2] < 60;
                       }) > 0);
        // circle(cx=5,cy=5,r=2) は device px で (10,10) 付近
        CHECK(countPix(px, s->w, 6, 6, 14, 14,
                       [](const uint8_t* p) {
                           return p[3] > 200 && p[1] > 200 && p[0] < 60;
                       }) > 0);
    }

    s = svgSkin(ren, sp, 1);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 120);
        CHECK_EQ(s->h, 60);
        CHECK_EQ(countPix(s->px.data(), s->w, 0, 0, s->w, s->h,
                          [](const uint8_t* p) { return p[3] > 0; }), 0);
    }

    // viewBox の無い SVG は図形のバウンディングボックスで採寸される
    s = svgSkin(ren, sp, 2);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->w, 20);
        CHECK_EQ(s->h, 20);
    }
    std::printf("svg-nanosvg: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
#endif

} // namespace

int main() {
    if (std::getenv("XYO_TEST_PEN_RECORDS")) {
        test_pen_point_record_parity();
        test_pen_point_record_parity(640, 480);
        return g_fail ? 1 : 0;
    }
    test_pen_point_record_parity();
    test_negative_cache_zero_svg();
    test_negative_cache_empty_and_bad_bitmap();
    test_positive_cache_valid_svg();
    test_costume_switch_and_clone();
    test_pen_points_gpu();
    test_pen_lines();
    test_pen_blend_and_robust();
    test_pen_alpha_accum();
    test_svg_text_render();
    test_svg_anchor_and_viewbox();
    test_svg_bad_decode();
    test_svg_text_usage();
    test_fence_empty_point();
    test_fence_aabb();
    test_pen_border_fenced();
    test_pen_full_coverage();
    test_frame_capture();
    test_pen_reverse_composite();
#if !XYO_TEST_RSVG
    test_svg_nanosvg_fallback();
#endif
    std::printf("render: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
