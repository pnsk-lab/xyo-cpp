// xyo-cpp: Scratch グラフィックエフェクト（scratch-render sprite.frag の CPU 移植）。
// 参照: refs/scratch-editor/packages/scratch-render/src/shaders/sprite.frag
//       refs/scratch-editor/packages/scratch-render/src/ShaderManager.js

#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace xyo {

namespace {

constexpr float kEpsilon = 1e-3f;

// --- ShaderManager.EFFECT_INFO の converter 準拠 -------------------------
struct FxUniforms {
    float color = 0;       // (x/200) % 1
    float fisheye = 0;     // max(0,(x+100)/100)
    float whirl = 0;       // -x * PI/180
    float pixelate = 0;    // |x|/10
    int mosaic = 1;        // round((|x|+10)/10) clamp [1,512]
    float brightness = 0;  // clamp(x,-100,100)/100
    float ghost = 0;       // 1 - clamp(x,0,100)/100  （alpha 乗数）
};

double eff(const std::map<std::string, double>& e, const char* k) {
    auto it = e.find(k);
    return it != e.end() ? it->second : 0.0;
}

FxUniforms toUniforms(const std::map<std::string, double>& e) {
    FxUniforms u;
    u.color = std::fmod(eff(e, "color") / 200.0, 1.0);
    u.fisheye = std::max(0.0, (eff(e, "fisheye") + 100.0) / 100.0);
    u.whirl = -eff(e, "whirl") * M_PI / 180.0;
    u.pixelate = std::abs(eff(e, "pixelate")) / 10.0;
    {
        double m = std::round((std::abs(eff(e, "mosaic")) + 10.0) / 10.0);
        u.mosaic = static_cast<int>(std::clamp(m, 1.0, 512.0));
    }
    u.brightness = std::clamp(eff(e, "brightness"), -100.0, 100.0) / 100.0;
    u.ghost = 1.0f - std::clamp(eff(e, "ghost"), 0.0, 100.0) / 100.0;
    return u;
}

// --- sprite.frag の convertRGB2HSV / convertHSV2RGB（chilliant）----------
struct Hsv { float h, s, v; };

Hsv rgb2hsv(float r, float g, float b) {
    // vec4 temp1 = rgb.b > rgb.g ? vec4(rgb.bg, wz) : vec4(rgb.gb, xy)
    float t1x, t1y, t1z;
    if (b > g) { t1x = b; t1y = g; t1z = -1.0f / 3.0f; }
    else       { t1x = g; t1y = b; t1z = 0.0f; }
    float w = b > g ? 2.0f / 3.0f : -1.0f;
    // vec4 temp2 = rgb.r > temp1.x ? vec4(r, temp1.yzx) : vec4(temp1.xyw, r)
    float M, y_, z_, m2;
    if (r > t1x) { M = r; y_ = t1y; z_ = t1z; m2 = t1x; }
    else         { M = t1x; y_ = t1y; z_ = w; m2 = r; }
    float m = std::min(y_, m2);
    float C = M - m;
    float h = std::abs(z_ + (m2 - y_) / (6.0f * C + kEpsilon));
    return {h, C / (M + kEpsilon), M};
}

void hue2rgb(float hue, float& r, float& g, float& b) {
    r = std::abs(hue * 6.0f - 3.0f) - 1.0f;
    g = 2.0f - std::abs(hue * 6.0f - 2.0f);
    b = 2.0f - std::abs(hue * 6.0f - 4.0f);
    r = std::clamp(r, 0.0f, 1.0f);
    g = std::clamp(g, 0.0f, 1.0f);
    b = std::clamp(b, 0.0f, 1.0f);
}

void hsv2rgb(Hsv hsv, float& r, float& g, float& b) {
    hue2rgb(hsv.h, r, g, b);
    float c = hsv.v * hsv.s;
    float base = hsv.v - c;
    r = r * c + base;
    g = g * c + base;
    b = b * c + base;
}

// バイリニアサンプル（CLAMP_TO_EDGE 相当）。straight-alpha RGBA。
void sampleBilinear(const uint8_t* src, int w, int h, double u, double v,
                    float out[4]) {
    double x = u * w - 0.5, y = v * h - 0.5;
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    double fx = x - x0, fy = y - y0;
    auto at = [&](int ix, int iy) -> const uint8_t* {
        ix = std::clamp(ix, 0, w - 1);
        iy = std::clamp(iy, 0, h - 1);
        return src + (static_cast<size_t>(iy) * w + ix) * 4;
    };
    const uint8_t *p00 = at(x0, y0), *p10 = at(x0 + 1, y0),
                  *p01 = at(x0, y0 + 1), *p11 = at(x0 + 1, y0 + 1);
    for (int i = 0; i < 4; ++i) {
        double top = p00[i] + (p10[i] - p00[i]) * fx;
        double bot = p01[i] + (p11[i] - p01[i]) * fx;
        out[i] = static_cast<float>(top + (bot - top) * fy);
    }
}

} // namespace

bool hasActiveEffects(const std::map<std::string, double>& effects) {
    for (auto& [k, v] : effects)
        if (v != 0.0) return true;
    return false;
}

std::string effectKey(const std::map<std::string, double>& effects) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                  eff(effects, "color"), eff(effects, "fisheye"),
                  eff(effects, "whirl"), eff(effects, "pixelate"),
                  eff(effects, "mosaic"), eff(effects, "brightness"),
                  eff(effects, "ghost"));
    return buf;
}

void applyEffects(const uint8_t* src, int w, int h,
                  const std::map<std::string, double>& effects,
                  std::vector<uint8_t>& out) {
    const FxUniforms u = toUniforms(effects);
    out.resize(static_cast<size_t>(w) * h * 4);

    const bool spatial = u.mosaic > 1 || u.pixelate > 0.001 ||
                         u.whirl != 0 || u.fisheye > 0.001;
    const bool colorize = u.color != 0 || u.brightness != 0;
    const bool ghosted = u.ghost != 0;

    const double pixelCellsX = u.pixelate > 0.001 ? w / u.pixelate : 0;
    const double pixelCellsY = u.pixelate > 0.001 ? h / u.pixelate : 0;
    constexpr double kC = 0.5;

    float px4[4];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double u0 = (x + 0.5) / w;
            double v0 = (y + 0.5) / h;
            double tu = u0, tv = v0;
            if (spatial) {
                // mosaic → pixelate → whirl → fisheye（シェーダ適用順）
                if (u.mosaic > 1) {
                    tu = std::fmod(tu * u.mosaic, 1.0);
                    tv = std::fmod(tv * u.mosaic, 1.0);
                    if (tu < 0) tu += 1.0;
                    if (tv < 0) tv += 1.0;
                }
                if (u.pixelate > 0.001) {
                    tu = (std::floor(tu * pixelCellsX) + kC) / pixelCellsX;
                    tv = (std::floor(tv * pixelCellsY) + kC) / pixelCellsY;
                }
                if (u.whirl != 0) {
                    double ox = tu - kC, oy = tv - kC;
                    double m = std::sqrt(ox * ox + oy * oy);
                    double f = std::max(1.0 - m / 0.5, 0.0);
                    double a = u.whirl * f * f;
                    double sn = std::sin(a), cs = std::cos(a);
                    tu = cs * ox - sn * oy + kC;
                    tv = sn * ox + cs * oy + kC;
                }
                if (u.fisheye > 0.001) {
                    double vx = (tu - kC) / kC, vy = (tv - kC) / kC;
                    double l = std::sqrt(vx * vx + vy * vy) + 1e-9;
                    double r = std::pow(std::min(l, 1.0), u.fisheye) *
                               std::max(1.0, l);
                    tu = kC + r * vx / l * kC;
                    tv = kC + r * vy / l * kC;
                }
            }
            sampleBilinear(src, w, h, tu, tv, px4);
            float rr = px4[0], gg = px4[1], bb = px4[2], aa = px4[3];
            if (colorize) {
                // premult 除算相当: straight-alpha なので rgb に直接適用
                if (u.color != 0) {
                    Hsv hsv = rgb2hsv(rr / 255.f, gg / 255.f, bb / 255.f);
                    const float minL = 0.11f / 2.0f, minS = 0.09f;
                    if (hsv.v < minL) hsv = {0.f, 1.f, minL};
                    else if (hsv.s < minS) hsv = {0.f, minS, hsv.v};
                    hsv.h = std::fmod(hsv.h + u.color, 1.0f);
                    if (hsv.h < 0) hsv.h += 1.0f;
                    hsv2rgb(hsv, rr, gg, bb);
                    rr *= 255.f; gg *= 255.f; bb *= 255.f;
                }
                if (u.brightness != 0) {
                    float d = u.brightness * 255.f;
                    rr = std::clamp(rr + d, 0.f, 255.f);
                    gg = std::clamp(gg + d, 0.f, 255.f);
                    bb = std::clamp(bb + d, 0.f, 255.f);
                }
            }
            if (ghosted) {
                rr *= u.ghost; gg *= u.ghost; bb *= u.ghost; aa *= u.ghost;
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            o[0] = static_cast<uint8_t>(std::clamp(rr + 0.5f, 0.f, 255.f));
            o[1] = static_cast<uint8_t>(std::clamp(gg + 0.5f, 0.f, 255.f));
            o[2] = static_cast<uint8_t>(std::clamp(bb + 0.5f, 0.f, 255.f));
            o[3] = static_cast<uint8_t>(std::clamp(aa + 0.5f, 0.f, 255.f));
        }
    }
}

} // namespace xyo
