#ifndef SJIT_SKIA_PEN_LAYER_HPP
#define SJIT_SKIA_PEN_LAYER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

class GrDirectContext;

namespace sjit {

struct SkiaPenPoint {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    int r = 0;
    int g = 0;
    int b = 0;
};

class SkiaPenLayer {
public:
    SkiaPenLayer(int width, int height, GrDirectContext *gpu_context = nullptr);
    ~SkiaPenLayer();

    SkiaPenLayer(const SkiaPenLayer &) = delete;
    SkiaPenLayer &operator=(const SkiaPenLayer &) = delete;

    bool valid() const noexcept;
    int width() const noexcept;
    int height() const noexcept;

    void clear();
    bool drawStroke(
        double x1,
        double y1,
        double x2,
        double y2,
        double width,
        int r,
        int g,
        int b,
        int a);
    bool drawOpaquePoints(const SkiaPenPoint *points, std::size_t count);
    bool readRgbaPixels(std::uint8_t *pixels, std::size_t row_bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sjit

#endif
