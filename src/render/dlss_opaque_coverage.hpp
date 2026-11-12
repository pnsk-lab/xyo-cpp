// Conservative, fixed-resolution alpha coverage for DLSS motion overrides.
//
// A bit is set only when every source texel covered by its tile, plus a
// one-texel safety border, has alpha=255. False negatives are deliberate:
// never override optical-flow vectors for transparent/antialiased pixels.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xyo::render {

constexpr int kDlssCoverageSide = 128;
constexpr int kDlssCoverageWords = kDlssCoverageSide * kDlssCoverageSide / 32;

using DlssOpaqueCoverage = std::array<uint32_t, kDlssCoverageWords>;

inline bool dlssCoverageContains(const DlssOpaqueCoverage& mask,
                                 int x, int y) noexcept {
    if (x < 0 || y < 0 ||
        x >= kDlssCoverageSide || y >= kDlssCoverageSide)
        return false;
    const unsigned index = unsigned(y * kDlssCoverageSide + x);
    return (mask[index >> 5] & (1u << (index & 31u))) != 0;
}

inline DlssOpaqueCoverage dlssBuildOpaqueCoverage(
    std::span<const uint8_t> rgba, int width, int height) {
    DlssOpaqueCoverage result{};
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        rgba.size() != size_t(width) * size_t(height) * 4)
        return result;

    for (int y = 0; y < kDlssCoverageSide; ++y) {
        const int minY = std::max(0, y * height / kDlssCoverageSide - 1);
        const int maxY = std::min(height - 1,
            ((y + 1) * height + kDlssCoverageSide - 1) /
                kDlssCoverageSide);
        for (int x = 0; x < kDlssCoverageSide; ++x) {
            const int minX = std::max(0, x * width / kDlssCoverageSide - 1);
            const int maxX = std::min(width - 1,
                ((x + 1) * width + kDlssCoverageSide - 1) /
                    kDlssCoverageSide);
            bool opaque = true;
            for (int yy = minY; yy <= maxY && opaque; ++yy) {
                for (int xx = minX; xx <= maxX; ++xx) {
                    if (rgba[(size_t(yy) * width + xx) * 4 + 3] != 255) {
                        opaque = false;
                        break;
                    }
                }
            }
            if (opaque) {
                const unsigned index = unsigned(y * kDlssCoverageSide + x);
                result[index >> 5] |= 1u << (index & 31u);
            }
        }
    }
    return result;
}

inline bool dlssCoverageHasOpaqueArea(
    const DlssOpaqueCoverage& mask) noexcept {
    for (uint32_t word : mask)
        if (word) return true;
    return false;
}

} // namespace xyo::render
