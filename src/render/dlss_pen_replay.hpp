// Optional display-only, jittered rerasterization of Scratch Pen strokes.
// Never replaces the persistent collision/stamp/save pen buffer.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xyo::render {

struct DlssPenReplayStroke {
    std::array<float, 4> rgba{};
    float diameter = 1;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Scratch coordinates
};

constexpr size_t kDlssPenReplayMaxStrokes = 512;

// Preflight GPU geometry without a pixel shader or frame buffer allocation.
// Mirror the conservative CPU sampled-AABB budget.
inline bool dlssPenReplayWithinBudget(
    std::span<const DlssPenReplayStroke> strokes,
    int stageW, int stageH, int width, int height,
    float jitterX, float jitterY) {
    if (stageW <= 0 || stageH <= 0 || width <= 0 || height <= 0 ||
        size_t(width)*size_t(height) > 1024u*1024u ||
        strokes.size() > kDlssPenReplayMaxStrokes ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        std::abs(jitterX) > 0.5f || std::abs(jitterY) > 0.5f)
        return false;
    const double sx=double(width)/stageW, sy=double(height)/stageH;
    const double radiusScale=std::min(sx,sy);
    uint64_t visits=0;
    for (const auto& stroke : strokes) {
        const double d=stroke.diameter;
        if (!std::isfinite(d) || d<=0 || d>4096 ||
            !std::isfinite(stroke.x0) || !std::isfinite(stroke.y0) ||
            !std::isfinite(stroke.x1) || !std::isfinite(stroke.y1) ||
            std::abs(stroke.x0)>1e6 || std::abs(stroke.x1)>1e6 ||
            std::abs(stroke.y0)>1e6 || std::abs(stroke.y1)>1e6)
            return false;
        for (float color : stroke.rgba)
            if (!std::isfinite(color)) return false;
        const double origin=(d==1 || d==3) ? 0.5 : 0.0;
        const double ax=(stageW*0.5+stroke.x0+origin)*sx+jitterX;
        const double bx=(stageW*0.5+stroke.x1+origin)*sx+jitterX;
        const double ay=(stageH*0.5-stroke.y0-origin)*sy+jitterY;
        const double by=(stageH*0.5-stroke.y1-origin)*sy+jitterY;
        const double support=d*radiusScale*0.5+0.5;
        const auto bound=[](double x,int max){
            return std::clamp(int(std::floor(
                std::clamp(x,-1e6,1e6))),0,max);
        };
        const int left=bound(std::min(ax,bx)-support-1,width);
        const int right=bound(std::max(ax,bx)+support+2,width);
        const int top=bound(std::min(ay,by)-support-1,height);
        const int bottom=bound(std::max(ay,by)+support+2,height);
        visits+=uint64_t(right-left)*uint64_t(bottom-top);
        if (visits>1'000'000u) return false;
    }
    return true;
}

inline bool dlssReplayPenStrokes(std::span<const DlssPenReplayStroke> strokes,
                                int stageW, int stageH, int width, int height,
                                float jitterX, float jitterY,
                                std::vector<uint8_t>& out) {
    constexpr size_t kMaxPixels = 1024u * 1024u;
    if (stageW <= 0 || stageH <= 0 || width <= 0 || height <= 0 ||
        size_t(width) * size_t(height) > kMaxPixels ||
        strokes.size() > kDlssPenReplayMaxStrokes ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        std::abs(jitterX) > 0.5f || std::abs(jitterY) > 0.5f)
        return false;

    out.assign(size_t(width) * height * 4, 0);
    const double sx = double(width) / stageW;
    const double sy = double(height) / stageH;
    const double pixelScale = std::min(sx, sy);
    // Prevent pathological line widths / overdraw from monopolizing a
    // rendering frame. We are only a display-quality experiment.
    constexpr uint64_t kMaxSampleVisits = 1'000'000;
    uint64_t sampleVisits = 0;

    for (const auto& stroke : strokes) {
        const double diameter = stroke.diameter;
        if (!std::isfinite(diameter) || diameter <= 0 || diameter > 4096 ||
            !std::isfinite(stroke.x0) || !std::isfinite(stroke.x1) ||
            !std::isfinite(stroke.y0) || !std::isfinite(stroke.y1) ||
            std::abs(stroke.x0) > 1e6 || std::abs(stroke.x1) > 1e6 ||
            std::abs(stroke.y0) > 1e6 || std::abs(stroke.y1) > 1e6)
            return false;
        for (float color : stroke.rgba)
            if (!std::isfinite(color)) return false;
        // Scratch Pen gives diameter 1 and 3 a half logical-pixel offset.
        const double origin = (diameter == 1 || diameter == 3) ? 0.5 : 0.0;
        const double ax = (stageW * 0.5 + stroke.x0 + origin) * sx + jitterX;
        const double ay = (stageH * 0.5 - stroke.y0 - origin) * sy + jitterY;
        const double bx = (stageW * 0.5 + stroke.x1 + origin) * sx + jitterX;
        const double by = (stageH * 0.5 - stroke.y1 - origin) * sy + jitterY;
        const double dx = bx - ax, dy = by - ay;
        const double length2 = dx*dx + dy*dy;
        const double radius = diameter * pixelScale * 0.5;
        const double support = radius + 0.5;
        const int minX = std::clamp(
            int(std::floor(std::max(-1e6, std::min(ax,bx)-support-1.0))),0,width);
        const int maxX = std::clamp(
            int(std::ceil(std::min(1e6, std::max(ax,bx)+support+1.0))),0,width);
        const int minY = std::clamp(
            int(std::floor(std::max(-1e6, std::min(ay,by)-support-1.0))),0,height);
        const int maxY = std::clamp(
            int(std::ceil(std::min(1e6, std::max(ay,by)+support+1.0))),0,height);
        sampleVisits += uint64_t(maxX-minX) * uint64_t(maxY-minY);
        if (sampleVisits > kMaxSampleVisits) return false;
        const double alpha = std::clamp(double(stroke.rgba[3]),0.0,1.0);
        for (int y=minY; y<maxY; ++y) {
            for (int x=minX; x<maxX; ++x) {
                const double vx=x+0.5-ax, vy=y+0.5-ay;
                const double t=length2>1e-12
                    ? std::clamp((vx*dx+vy*dy)/length2,0.0,1.0):0.0;
                const double dist=std::hypot(vx-t*dx,vy-t*dy);
                const double sa=alpha*std::clamp(support-dist,0.0,1.0);
                if (sa<=0.0) continue;
                uint8_t* dest=&out[(size_t(y)*width+x)*4];
                const double previousAlpha=dest[3]/255.0;
                const double nextAlpha=sa+previousAlpha*(1.0-sa);
                for (int channel=0; channel<3; ++channel) {
                    // Store straight RGBA: SDL_BLENDMODE_BLEND applies the
                    // coverage once at the final stage composition.
                    const double oldPremul=(dest[channel]/255.0)*previousAlpha;
                    const double source=std::clamp(double(stroke.rgba[channel]),0.0,1.0);
                    const double nextPremul=source*sa+oldPremul*(1.0-sa);
                    dest[channel]=static_cast<uint8_t>(std::lround(
                        255.0*nextPremul/nextAlpha));
                }
                dest[3]=static_cast<uint8_t>(std::lround(255.0*nextAlpha));
            }
        }
    }
    return true;
}

} // namespace xyo::render
