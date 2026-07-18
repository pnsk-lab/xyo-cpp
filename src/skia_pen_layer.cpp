#include "sjit/skia_pen_layer.hpp"

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace sjit {

struct SkiaPenLayer::Impl {
    int width = 0;
    int height = 0;
    sk_sp<SkSurface> surface;
    GrDirectContext *gpu_context = nullptr;
};

namespace {

bool canConvertToSkScalar(double value) {
    constexpr double kMaxSkScalar =
        static_cast<double>(std::numeric_limits<SkScalar>::max()) / 4.0;
    return std::isfinite(value) && value >= -kMaxSkScalar && value <= kMaxSkScalar;
}

U8CPU colorChannel(int value) {
    return static_cast<U8CPU>(std::clamp(value, 0, 255));
}

} // namespace

SkiaPenLayer::SkiaPenLayer(int width, int height, GrDirectContext *gpu_context)
    : impl_(std::make_unique<Impl>()) {
    if (width <= 0 || height <= 0) {
        return;
    }
    impl_->width = width;
    impl_->height = height;
    impl_->gpu_context = gpu_context;
    if (impl_->gpu_context) {
        impl_->surface = SkSurfaces::RenderTarget(
            impl_->gpu_context,
            skgpu::Budgeted::kNo,
            SkImageInfo::MakeN32Premul(width, height));
    } else {
        impl_->surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    }
    clear();
}

SkiaPenLayer::~SkiaPenLayer() = default;

bool SkiaPenLayer::valid() const noexcept {
    return impl_ && impl_->surface;
}

int SkiaPenLayer::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

int SkiaPenLayer::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

void SkiaPenLayer::clear() {
    if (valid()) {
        impl_->surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
}

bool SkiaPenLayer::drawStroke(
    double x1,
    double y1,
    double x2,
    double y2,
    double width,
    int r,
    int g,
    int b,
    int a) {
    if (!valid() || !canConvertToSkScalar(x1) || !canConvertToSkScalar(y1) ||
        !canConvertToSkScalar(x2) || !canConvertToSkScalar(y2) ||
        !canConvertToSkScalar(width)) {
        return false;
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeCap(SkPaint::kRound_Cap);
    paint.setStrokeJoin(SkPaint::kRound_Join);
    paint.setStrokeWidth(static_cast<SkScalar>(std::max(1.0, width)));
    paint.setARGB(colorChannel(a), colorChannel(r), colorChannel(g), colorChannel(b));
    paint.setBlendMode(SkBlendMode::kSrcOver);

    SkCanvas *canvas = impl_->surface->getCanvas();
    const SkScalar sx1 = static_cast<SkScalar>(x1);
    const SkScalar sy1 = static_cast<SkScalar>(y1);
    if (x1 == x2 && y1 == y2) {
        canvas->drawPoint(sx1, sy1, paint);
    } else {
        canvas->drawLine(
            sx1,
            sy1,
            static_cast<SkScalar>(x2),
            static_cast<SkScalar>(y2),
            paint);
    }
    return true;
}

bool SkiaPenLayer::drawOpaquePoints(const SkiaPenPoint *points, std::size_t count) {
    if (!valid() || !points || count == 0) {
        return count == 0;
    }

    SkCanvas *canvas = impl_->surface->getCanvas();
    /* Scratch pen stamps are ordered. Do not batch all points by color: doing
       so changes the order of overlapping opaque stamps (the old unordered
       map implementation made the result depend on hash iteration order).
       Consecutive points with the same paint can still be batched safely. */
    std::size_t run_start = 0;
    while (run_start < count) {
        const SkiaPenPoint &first = points[run_start];
        if (!canConvertToSkScalar(first.x) || !canConvertToSkScalar(first.y) ||
            !canConvertToSkScalar(first.width)) {
            return false;
        }
        const SkScalar width = static_cast<SkScalar>(std::max(1.0, first.width));
        const SkColor color = SkColorSetRGB(
            colorChannel(first.r), colorChannel(first.g), colorChannel(first.b));
        std::vector<SkPoint> positions;
        positions.reserve(std::min<std::size_t>(count - run_start, 256));
        std::size_t run_end = run_start;
        while (run_end < count) {
            const SkiaPenPoint &point = points[run_end];
            if (!canConvertToSkScalar(point.x) || !canConvertToSkScalar(point.y) ||
                !canConvertToSkScalar(point.width)) {
                return false;
            }
            const SkScalar point_width = static_cast<SkScalar>(std::max(1.0, point.width));
            const SkColor point_color = SkColorSetRGB(
                colorChannel(point.r), colorChannel(point.g), colorChannel(point.b));
            if (point_width != width || point_color != color) {
                break;
            }
            positions.emplace_back(
                static_cast<SkScalar>(point.x), static_cast<SkScalar>(point.y));
            ++run_end;
        }
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeWidth(width);
        paint.setColor(color);
        paint.setBlendMode(SkBlendMode::kSrcOver);
        canvas->drawPoints(
            SkCanvas::kPoints_PointMode,
            SkSpan<const SkPoint>(positions.data(), positions.size()),
            paint);
        run_start = run_end;
    }
    return true;
}

bool SkiaPenLayer::readRgbaPixels(std::uint8_t *pixels, std::size_t row_bytes) {
    if (!valid() || !pixels ||
        row_bytes < static_cast<std::size_t>(impl_->width) * 4u) {
        return false;
    }
    if (impl_->gpu_context) {
        impl_->gpu_context->flushAndSubmit();
    }
    const SkImageInfo destination = SkImageInfo::Make(
        impl_->width,
        impl_->height,
        kRGBA_8888_SkColorType,
        kUnpremul_SkAlphaType);
    return impl_->surface->readPixels(destination, pixels, row_bytes, 0, 0);
}

} // namespace sjit
