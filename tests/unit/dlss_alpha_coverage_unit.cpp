// Pure CPU unit test for DLSS opt-in costume alpha classifier.
// Does not need NVIDIA, Vulkan or SDL.
#include "../../src/render/dlss_alpha_coverage.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
using xyo::render::hasInteriorTranslucency;

int main() {
    constexpr int W = 8, H = 8;
    std::array<uint8_t, W * H * 4> pixels{};
    auto put = [&](int x, int y, uint8_t alpha) {
        pixels[(y * W + x) * 4 + 3] = alpha;
    };
    auto all = [&](uint8_t alpha) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                put(x, y, alpha);
    };
    all(255);
    if (hasInteriorTranslucency(pixels.data(), W, H)) return 1;
    all(0);
    if (hasInteriorTranslucency(pixels.data(), W, H)) return 2;

    // An ordinary opaque shape with a single one-pixel antialias band
    // must not force DLSS to reset temporal history.
    all(0);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x)
            put(x, y, 255);
    for (int x = 1; x <= 6; ++x)
        put(x, 1, 128);
    if (hasInteriorTranslucency(pixels.data(), W, H)) return 3;

    // Flat alpha interiors (as emitted by SVG fill-opacity and RGBA PNG)
    // should trigger, even if surrounded by fully transparent texels.
    all(0);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x)
            put(x, y, 128);
    if (!hasInteriorTranslucency(pixels.data(), W, H)) return 4;
    all(0);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x)
            put(x, y, 32);
    if (!hasInteriorTranslucency(pixels.data(), W, H)) return 5;

    // Too-thin translucent lines have no 3x3 stable interior. This
    // known deliberate false negative prevents opaque SVG AA false alarms.
    all(0);
    for (int x = 1; x <= 6; ++x) put(x, 4, 128);
    if (hasInteriorTranslucency(pixels.data(), W, H)) return 6;

    if (hasInteriorTranslucency(nullptr, W, H)) return 7;
    if (hasInteriorTranslucency(pixels.data(), 1, 1)) return 8;
    std::puts("PASS: opaque/transparent, antialias-only, PNG/SVG-like "
              "flat alpha and narrow-line exclusions");
}
