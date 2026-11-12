// Conservative known-geometry motion for opaque Scratch sprites.
// Completely independent of Vulkan and Scratch runtime implementation.
#pragma once
#include "dlss_opaque_coverage.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace xyo::render {

struct DlssRigidMotionRect {
    int32_t left, top, right, bottom;
    float deltaX, deltaY;       // current pixel -> previous pixel (translation)
    float affine = 0;           // 0 = translation, 1 = full inverse affine
    float reserved = 0;
    // std430 vec4 rows: current screen pixel -> previous screen pixel.
    // The fourth component stores the polygon inset in normalized UV space.
    float previousX[4] = {};
    float previousY[4] = {};
    // Current screen pixel -> normalized image coordinates (0..1).
    float currentU[4] = {};
    float currentV[4] = {};
    // Optional 128x128 conservative opaque coverage (std430 uint[512]).
    DlssOpaqueCoverage opaqueBits{};
};
static_assert(sizeof(DlssRigidMotionRect) == 96 + 4*kDlssCoverageWords);

struct DlssRigidSprite {
    int32_t id = 0;
    uintptr_t costume = 0;  // image/cache identity; not dereferenced
    float centerX = 0, centerY = 0;  // input pixel space
    float left = 0, top = 0, right = 0, bottom = 0;
    float size = 100, direction = 90;
    uint8_t rotationStyle = 0;
    bool opaqueUnrotated = false;
    // Actual SDL_RenderCopyEx destination rectangle and rotation center,
    // in input render pixels, *after SDL integer rounding*.
    float rasterX = 0, rasterY = 0, rasterW = 0, rasterH = 0;
    float pivotX = 0, pivotY = 0; // relative to raster rectangle
    float renderAngle = 0;       // clockwise degrees in SDL y-down space
    const DlssOpaqueCoverage* opaqueBits = nullptr;
};

class DlssRigidMotionTracker {
public:
    static constexpr unsigned kMaxRects = 64;

    std::vector<DlssRigidMotionRect> update(
        std::span<const DlssRigidSprite> current, int w, int h,
        bool affineEnabled = false) {
        std::vector<DlssRigidMotionRect> result;
        result.reserve(std::min(current.size(), size_t(kMaxRects)));
        if (w <= 0 || h <= 0 || w != width_ || h != height_) {
            previous_.clear();
        }
        if (w > 0 && h > 0) {
            for (const auto& now : current) {
                if (result.size() == kMaxRects) break;
                if (!valid(now) || !now.opaqueUnrotated) continue;
                const auto found = previous_.find(now.id);
                if (found == previous_.end()) continue;
                const auto& old = found->second;
                if (!valid(old) || !old.opaqueUnrotated ||
                    now.costume != old.costume ||
                    now.rotationStyle != old.rotationStyle ||
                    (!affineEnabled && (now.size != old.size ||
                                        now.direction != old.direction)))
                    continue;
                if (affineEnabled) {
                    // A static sprite needs no override; leave its zero/OF
                    // motion alone and avoid the per-pixel affine work.
                    if (std::abs(now.centerX-old.centerX) < 0.001f &&
                        std::abs(now.centerY-old.centerY) < 0.001f &&
                        now.rasterW == old.rasterW &&
                        now.rasterH == old.rasterH &&
                        now.rasterX == old.rasterX &&
                        now.rasterY == old.rasterY &&
                        now.renderAngle == old.renderAngle)
                        continue;
                    DlssRigidMotionRect rect{};
                    if (!buildAffine(now, old, w, h, rect)) continue;
                    // Affine pixel displacement must not be used when another
                    // sprite can occlude the current or the previous quad.
                    bool overlaps = false;
                    for (const auto& other : current) {
                        if (other.id != now.id &&
                            overlap(rect.left,rect.top,rect.right,rect.bottom,other)) {
                            overlaps = true; break;
                        }
                    }
                    if (!overlaps) {
                        for (const auto& [id, other] : previous_) {
                            if (id != now.id &&
                                overlap(int(std::floor(old.left)),
                                        int(std::floor(old.top)),
                                        int(std::ceil(old.right)),
                                        int(std::ceil(old.bottom)),other)) {
                                overlaps = true; break;
                            }
                        }
                    }
                    if (!overlaps) result.push_back(rect);
                    continue;
                }
                const float dx = old.centerX - now.centerX;
                const float dy = old.centerY - now.centerY;
                // SDL_RenderCopyEx rasterizes integer destination rects.
                // Fractional center differences may *not* equal the actual
                // rasterized motion; leave them to Optical Flow until a
                // subpixel-accurate geometric rendering path exists.
                if (!std::isfinite(dx) || !std::isfinite(dy) ||
                    (std::abs(dx) < 0.25f && std::abs(dy) < 0.25f) ||
                    std::abs(dx) > 32 || std::abs(dy) > 32 ||
                    std::abs(dx - std::round(dx)) > 0.001f ||
                    std::abs(dy - std::round(dy)) > 0.001f)
                    continue;
                // A 3px inset protects the antialias edge and rounding
                // differences between SDL's integer rect and Scratch coords.
                const int l = std::clamp(int(std::ceil(now.left)) + 3, 0, w);
                const int t = std::clamp(int(std::ceil(now.top)) + 3, 0, h);
                const int r = std::clamp(int(std::floor(now.right)) - 3, 0, w);
                const int b = std::clamp(int(std::floor(now.bottom)) - 3, 0, h);
                if (l >= r || t >= b) continue;

                // Whole-sprite exclusion when any other *visible* sprite can
                // obscure the candidate now OR at its old sample position.
                // Conservative: behind-sprite overlap also disables override.
                bool overlaps = false;
                for (const auto& other : current) {
                    if (other.id == now.id) continue;
                    if (overlap(l,t,r,b,other)) { overlaps = true; break; }
                }
                if (!overlaps) {
                    for (const auto& [id, other] : previous_) {
                        if (id == now.id) continue;
                        if (overlap(l+int(std::floor(dx)),
                                    t+int(std::floor(dy)),
                                    r+int(std::ceil(dx)),
                                    b+int(std::ceil(dy)),other)) {
                            overlaps = true; break;
                        }
                    }
                }
                if (!overlaps) result.push_back({l,t,r,b,dx,dy});
            }
        }
        previous_.clear();
        for (const auto& sample : current)
            if (valid(sample)) previous_[sample.id] = sample;
        width_ = w;
        height_ = h;
        return result;
    }
    void reset() { previous_.clear(); width_ = height_ = 0; }

private:
    struct Matrix {
        double qx, qy, xx, xy, yx, yy;
    };
    static bool transform(const DlssRigidSprite& s, Matrix& out) {
        if (!std::isfinite(s.rasterX) || !std::isfinite(s.rasterY) ||
            !std::isfinite(s.rasterW) || !std::isfinite(s.rasterH) ||
            !std::isfinite(s.pivotX) || !std::isfinite(s.pivotY) ||
            !std::isfinite(s.renderAngle) ||
            s.rasterW < 8 || s.rasterH < 8 ||
            s.rasterW > 8192 || s.rasterH > 8192)
            return false;
        constexpr double pi = 3.14159265358979323846;
        const double a = s.renderAngle * pi / 180.0;
        const double c = std::cos(a), sn = std::sin(a);
        const double centerX = s.rasterX + s.pivotX;
        const double centerY = s.rasterY + s.pivotY;
        out = {
            centerX - c*s.pivotX + sn*s.pivotY,
            centerY - sn*s.pivotX - c*s.pivotY,
            c*s.rasterW, -sn*s.rasterH,
            sn*s.rasterW, c*s.rasterH
        };
        return true;
    }
    static bool buildAffine(const DlssRigidSprite& now,
                            const DlssRigidSprite& old,
                            int width, int height,
                            DlssRigidMotionRect& rect) {
        Matrix c{}, p{};
        if (!transform(now,c) || !transform(old,p)) return false;
        const double determinant = c.xx*c.yy - c.xy*c.yx;
        if (!std::isfinite(determinant) ||
            std::abs(determinant) < 0.01)
            return false;
        const double i00=c.yy/determinant, i01=-c.xy/determinant;
        const double i10=-c.yx/determinant, i11=c.xx/determinant;
        // Inverse transform maps current screen pixels to source image UV.
        const double tx=-i00*c.qx-i01*c.qy;
        const double ty=-i10*c.qx-i11*c.qy;
        const double m00=p.xx*i00+p.xy*i10;
        const double m01=p.xx*i01+p.xy*i11;
        const double m10=p.yx*i00+p.yy*i10;
        const double m11=p.yx*i01+p.yy*i11;
        const double m02=p.qx+p.xx*tx+p.xy*ty;
        const double m12=p.qy+p.yx*tx+p.yy*ty;
        // Reject teleports and large geometry changes: Optical Flow is
        // preferable when temporal correspondence is not trustworthy.
        constexpr double maxMotion = 32.0;
        for (const double x : {double(now.left),double(now.right)}) {
            for (const double y : {double(now.top),double(now.bottom)}) {
                const double vx=(m00-1.0)*x + m01*y + m02;
                const double vy=m10*x + (m11-1.0)*y + m12;
                if (!std::isfinite(vx) || !std::isfinite(vy) ||
                    std::abs(vx)>maxMotion || std::abs(vy)>maxMotion)
                    return false;
            }
        }
        const double scaleX=old.rasterW/now.rasterW;
        const double scaleY=old.rasterH/now.rasterH;
        if (scaleX < 0.75 || scaleX > 1.35 ||
            scaleY < 0.75 || scaleY > 1.35)
            return false;
        const int l=std::clamp(int(std::floor(now.left)),0,width);
        const int t=std::clamp(int(std::floor(now.top)),0,height);
        const int r=std::clamp(int(std::ceil(now.right)),0,width);
        const int b=std::clamp(int(std::ceil(now.bottom)),0,height);
        if (l >= r || t >= b) return false;
        rect.left=l;rect.top=t;rect.right=r;rect.bottom=b;
        rect.affine=1;
        if (now.opaqueBits) {
            rect.opaqueBits = *now.opaqueBits;
            rect.reserved = 1.0f;
        }
        rect.previousX[0]=float(m00);
        rect.previousX[1]=float(m01);
        rect.previousX[2]=float(m02);
        rect.previousX[3]=float(3.0/now.rasterW);
        rect.previousY[0]=float(m10);
        rect.previousY[1]=float(m11);
        rect.previousY[2]=float(m12);
        rect.previousY[3]=float(3.0/now.rasterH);
        rect.currentU[0]=float(i00);
        rect.currentU[1]=float(i01);
        rect.currentU[2]=float(tx);
        rect.currentV[0]=float(i10);
        rect.currentV[1]=float(i11);
        rect.currentV[2]=float(ty);
        return true;
    }
    static bool valid(const DlssRigidSprite& s) {
        return s.costume != 0 && std::isfinite(s.centerX) &&
            std::isfinite(s.centerY) && std::isfinite(s.size) &&
            std::isfinite(s.direction) && std::isfinite(s.left) &&
            std::isfinite(s.top) && std::isfinite(s.right) &&
            std::isfinite(s.bottom) && s.size > 0 &&
            // Keep all floating -> integer rectangle conversions bounded,
            // including pathological Scratch numeric coordinates.
            std::abs(s.centerX) <= 1.0e6f &&
            std::abs(s.centerY) <= 1.0e6f &&
            std::abs(s.left) <= 1.0e6f &&
            std::abs(s.right) <= 1.0e6f &&
            std::abs(s.top) <= 1.0e6f &&
            std::abs(s.bottom) <= 1.0e6f &&
            s.right > s.left && s.bottom > s.top;
    }
    static bool overlap(int l, int t, int r, int b,
                        const DlssRigidSprite& other) {
        return valid(other) && other.left < r && other.right > l &&
            other.top < b && other.bottom > t;
    }
    std::unordered_map<int32_t, DlssRigidSprite> previous_;
    int width_ = 0, height_ = 0;
};
} // namespace xyo::render
