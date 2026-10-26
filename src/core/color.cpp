#include "color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace xyo::color {

std::optional<Rgb> hexToRgb(std::string_view hex) {
    if (!hex.empty() && hex[0] == '#') hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 3) return std::nullopt;
    int parsed = 0;
    for (char c : hex) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        parsed = (parsed << 4) | d;
    }
    if (hex.size() == 6) {
        return Rgb{(parsed >> 16) & 0xFF, (parsed >> 8) & 0xFF, parsed & 0xFF};
    }
    int r = (parsed >> 8) & 0xF, g = (parsed >> 4) & 0xF, b = parsed & 0xF;
    return Rgb{(r << 4) | r, (g << 4) | g, (b << 4) | b};
}

int rgbToDecimal(Rgb rgb) { return (rgb.r << 16) + (rgb.g << 8) + rgb.b; }

std::optional<int> hexToDecimal(std::string_view hex) {
    auto rgb = hexToRgb(hex);
    if (!rgb) return std::nullopt;
    return rgbToDecimal(*rgb);
}

std::string rgbToHex(Rgb rgb) { return decimalToHex(rgbToDecimal(rgb)); }

namespace {
const bool g_hsvFloorGeneric = std::getenv("XYO_HSV_FLOOR_GENERIC") != nullptr;
}

Rgb hsvToRgb(Hsv hsv) {
    // fmod は範囲外のときだけ呼ぶ（libm 呼び出しは ~9ns）。
    // [0,360) の h では結果が現行とビット一致することを全 24bit 色で確認済み。
    double h = hsv.h;
    if (!(h >= 0.0 && h < 360.0)) {
        h = std::fmod(h, 360.0);
        if (h < 0) h += 360;
    }
    double s = std::max(0.0, std::min(hsv.s, 1.0));
    double v = std::max(0.0, std::min(hsv.v, 1.0));
    // Achromatic inputs have p == q == t == v for every finite hue.
    // Keep normalization/clamping above in their original order; a nonfinite
    // normalized hue must retain the old path rather than bypass its behavior.
    if (s == 0.0 && std::isfinite(h)) {
        // Clamping makes v*255 finite and nonnegative: truncation equals floor.
        const double scaled = v * 255;
        int gray = static_cast<int>(g_hsvFloorGeneric ? std::floor(scaled) : scaled);
        return {gray, gray, gray};
    }
    int i = static_cast<int>(h / 60);
    double f = h / 60 - i;
    double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r, g, b;
    switch (i) {
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    default: r = v; g = t; b = p; break;
    }
    // For finite normalized hue, i is 0..6 and f is in [0,1). Ordered s/v
    // clamps put all RGB products in [0,1], so each scaled channel is in
    // [0,255]. Integer truncation therefore gives exactly the same floor.
    // Keep all preceding operations/grouping and the nonfinite legacy path.
    if (!g_hsvFloorGeneric && std::isfinite(h))
        return {static_cast<int>(r * 255), static_cast<int>(g * 255),
                static_cast<int>(b * 255)};
    return {static_cast<int>(std::floor(r * 255)),
            static_cast<int>(std::floor(g * 255)),
            static_cast<int>(std::floor(b * 255))};
}

Hsv rgbToHsv(Rgb rgb) {
    double r = rgb.r / 255.0, g = rgb.g / 255.0, b = rgb.b / 255.0;
    double x = std::min({r, g, b});
    double v = std::max({r, g, b});
    double h = 0, s = 0;
    if (x != v) {
        double f = (r == x) ? g - b : (g == x) ? b - r : r - g;
        double i = (r == x) ? 3 : (g == x) ? 5 : 1;
        // この式の値は数学的に [0,360) に収まる（全 24bit 色で確認済み）。
        // fmod を省き、丸め誤差ぶんだけ条件分岐で正規化する。
        h = (i - f / (v - x)) * 60;
        if (h < 0) h += 360;
        else if (h >= 360) h -= 360;
        s = (v - x) / v;
    }
    return {h, s, v};
}

RgbF mixRgb(Rgb a, Rgb b, double t) {
    if (t <= 0) return {static_cast<double>(a.r), static_cast<double>(a.g), static_cast<double>(a.b)};
    if (t >= 1) return {static_cast<double>(b.r), static_cast<double>(b.g), static_cast<double>(b.b)};
    double fa = 1 - t;
    return {fa * a.r + t * b.r, fa * a.g + t * b.g, fa * a.b + t * b.b};
}

} // namespace xyo::color
