// One-time classifier for intrinsically semi-transparent costume pixels.
// Only used by the opt-in DLSS alpha-safety policy; does not affect drawing.
// Thin antialias edges of fully opaque vector artwork should NOT trigger it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xyo::render {

inline bool hasInteriorTranslucency(const uint8_t* rgba,
                                   int width, int height) noexcept {
    if (!rgba || width < 3 || height < 3) return false;

    // Require a flat 3x3 area of partial alpha. An isolated 1px
    // anti-alias edge transitions between 0 and 255 and fails this test.
    // No allocations or cache invalidation are necessary after decoding.
    constexpr int minAlpha = 8;
    constexpr int maxAlpha = 247;
    constexpr int maxVariation = 24;
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    for (int y = 1; y + 1 < height; ++y) {
        const auto* row = rgba + static_cast<std::size_t>(y) * stride;
        for (int x = 1; x + 1 < width; ++x) {
            const int a = row[static_cast<std::size_t>(x) * 4 + 3];
            if (a < minAlpha || a > maxAlpha) continue;
            bool stable = true;
            for (int dy = -1; dy <= 1 && stable; ++dy) {
                const auto* neighborRow =
                    rgba + static_cast<std::size_t>(y + dy) * stride;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int b = neighborRow[
                        static_cast<std::size_t>(x + dx) * 4 + 3];
                    if (b < minAlpha || b > maxAlpha ||
                        b < a - maxVariation || b > a + maxVariation) {
                        stable = false;
                        break;
                    }
                }
            }
            if (stable) return true;
        }
    }
    return false;
}

} // namespace xyo::render
