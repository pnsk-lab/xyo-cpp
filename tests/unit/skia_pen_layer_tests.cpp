#include "sjit/skia_pen_layer.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const std::uint8_t *pixelAt(
    const std::vector<std::uint8_t> &pixels,
    int width,
    int x,
    int y) {
    return pixels.data() + (static_cast<std::size_t>(y * width + x) * 4u);
}

void testStrokeAndSrcOver() {
    constexpr int width = 24;
    constexpr int height = 16;
    sjit::SkiaPenLayer layer(width, height);
    require(layer.valid(), "Skia creates the raster pen surface");

    require(
        layer.drawStroke(5.0, 8.0, 19.0, 8.0, 4.0, 255, 0, 0, 128),
        "Skia draws a horizontal pen stroke");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));
    require(layer.readRgbaPixels(pixels.data(), width * 4u), "Skia exports RGBA pixels");

    const std::uint8_t *center = pixelAt(pixels, width, 12, 8);
    require(
        center[0] >= 250 && center[1] <= 2 && center[2] <= 2 &&
            center[3] >= 126 && center[3] <= 129,
        "Skia exports unpremultiplied red with the requested alpha");
    require(pixelAt(pixels, width, 1, 1)[3] == 0, "untouched pixels remain transparent");
    const std::uint8_t cap_center_alpha = pixelAt(pixels, width, 4, 8)[3];
    const std::uint8_t cap_corner_alpha = pixelAt(pixels, width, 3, 6)[3];
    require(
        cap_center_alpha > 120 && cap_corner_alpha < cap_center_alpha / 2,
        "pen strokes use round rather than square end caps");

    require(
        layer.drawStroke(12.0, 3.0, 12.0, 13.0, 4.0, 0, 0, 255, 128),
        "Skia draws a crossing pen stroke");
    require(layer.readRgbaPixels(pixels.data(), width * 4u), "Skia exports blended pixels");
    center = pixelAt(pixels, width, 12, 8);
    require(
        center[0] >= 80 && center[0] <= 90 && center[1] <= 2 &&
            center[2] >= 165 && center[2] <= 175 &&
            center[3] >= 190 && center[3] <= 193,
        "Skia applies source-over alpha composition");
}

void testPointAndClear() {
    constexpr int width = 16;
    constexpr int height = 16;
    sjit::SkiaPenLayer layer(width, height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));

    require(
        layer.drawStroke(8.0, 8.0, 8.0, 8.0, 6.0, 0, 255, 0, 255),
        "Skia draws a zero-length stroke as a point");
    require(layer.readRgbaPixels(pixels.data(), width * 4u), "Skia exports point pixels");
    const std::uint8_t *center = pixelAt(pixels, width, 8, 8);
    require(
        center[0] == 0 && center[1] == 255 && center[2] == 0 && center[3] == 255,
        "zero-length strokes preserve pen color");

    layer.clear();
    require(layer.readRgbaPixels(pixels.data(), width * 4u), "Skia exports cleared pixels");
    require(
        std::all_of(pixels.begin(), pixels.end(), [](std::uint8_t value) { return value == 0; }),
        "clearing the Skia pen layer restores transparent pixels");
}

void testOpaquePointOrder() {
    constexpr int width = 16;
    constexpr int height = 16;
    sjit::SkiaPenLayer layer(width, height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));
    const sjit::SkiaPenPoint points[] = {
        {5.0, 8.0, 6.0, 255, 0, 0},
        {11.0, 8.0, 6.0, 0, 0, 255},
        {5.0, 8.0, 6.0, 0, 0, 255},
        {11.0, 8.0, 6.0, 255, 0, 0}};
    require(layer.drawOpaquePoints(points, 4), "Skia draws ordered opaque points");
    require(layer.readRgbaPixels(pixels.data(), width * 4u), "Skia exports ordered points");
    const std::uint8_t *left = pixelAt(pixels, width, 5, 8);
    const std::uint8_t *right = pixelAt(pixels, width, 11, 8);
    require(
        left[0] == 0 && left[1] == 0 && left[2] == 255 && left[3] == 255 &&
            right[0] == 255 && right[1] == 0 && right[2] == 0 && right[3] == 255,
        "each pixel keeps its own latest opaque pen point on top");
}

} // namespace

int main() {
    testStrokeAndSrcOver();
    testPointAndClear();
    testOpaquePointOrder();
    std::cout << "Skia pen layer tests passed\n";
    return 0;
}
