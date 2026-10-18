// xyo-cpp: util/color.js の移植。
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace xyo {

struct Rgb { int r, g, b; };
struct RgbF { double r, g, b; };
struct Rgba { int r, g, b, a; };
struct Hsv { double h, s, v; };

namespace color {

inline std::string decimalToHex(int64_t decimal) {
    if (decimal < 0) decimal += 0xFFFFFF + 1;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(decimal & 0xFFFFFF));
    return buf;
}

inline Rgba decimalToRgb(int64_t decimal) {
    int a = static_cast<int>((decimal >> 24) & 0xFF);
    return {
        static_cast<int>((decimal >> 16) & 0xFF),
        static_cast<int>((decimal >> 8) & 0xFF),
        static_cast<int>(decimal & 0xFF),
        a > 0 ? a : 255};
}

/// "#RRGGBB"/"RRGGBB"/3桁形式 → RGB。失敗は nullopt。
std::optional<Rgb> hexToRgb(std::string_view hex);
int rgbToDecimal(Rgb rgb);
std::optional<int> hexToDecimal(std::string_view hex);
std::string rgbToHex(Rgb rgb);
Rgb hsvToRgb(Hsv hsv);
Hsv rgbToHsv(Rgb rgb);
/// JS版は double を返す（floor しない）
RgbF mixRgb(Rgb a, Rgb b, double t);

} // namespace color
} // namespace xyo
