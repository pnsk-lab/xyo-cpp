#include "../../src/render/dlss_opaque_coverage.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace xyo::render;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (0)
int main() {
    constexpr int W = 64, H = 64;
    std::vector<uint8_t> pixels(W * H * 4, 0);
    const auto setAlpha = [&](int x, int y, uint8_t a) {
        pixels[(y * W + x) * 4 + 3] = a;
    };
    for (int y=8; y<56; ++y)
        for (int x=8; x<56; ++x) setAlpha(x, y, 255);
    for (int y=25; y<39; ++y)
        for (int x=25; x<39; ++x) setAlpha(x, y, 0);
    const auto result = dlssBuildOpaqueCoverage(pixels, W, H);
    CHECK(dlssCoverageContains(result, 32, 32));
    CHECK(dlssCoverageContains(result, 108, 108));
    CHECK(!dlssCoverageContains(result, 64, 64));
    CHECK(!dlssCoverageContains(result, 5, 5));
    CHECK(!dlssCoverageContains(result, 16, 16));
    CHECK(!dlssCoverageContains(result, -1, 10));
    CHECK(!dlssCoverageContains(result, 128, 64));
    CHECK(dlssCoverageHasOpaqueArea(result));
    for (int y=8; y<56; ++y)
        for (int x=8; x<56; ++x) setAlpha(x,y,128);
    const auto partial = dlssBuildOpaqueCoverage(pixels, W, H);
    CHECK(!dlssCoverageHasOpaqueArea(partial));
    const auto invalid = dlssBuildOpaqueCoverage(
        std::span<const uint8_t>{pixels.data(),10}, W,H);
    CHECK(!dlssCoverageHasOpaqueArea(invalid));
    std::puts("PASS: conservative alpha coverage, transparent hole and safety margin");
}
