// Opt-in temporal antialiasing jitter in low-resolution input pixels.
// Halton points are bounded by [-0.5, 0.5); no RNG or mutable state.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_map>

namespace xyo::render {

struct DlssJitterSample {
    float x = 0.0f, y = 0.0f;
};

constexpr float dlssHalton(uint32_t index, uint32_t base) noexcept {
    float value = 0, factor = 1;
    while (index != 0) {
        factor /= float(base);
        value += factor * float(index % base);
        index /= base;
    }
    return value;
}

constexpr DlssJitterSample dlssJitter(uint64_t frame) noexcept {
    // Eight unique subpixel offsets, then repeat; skip Halton index 0.
    const uint32_t index = uint32_t(frame % 8) + 1;
    return {dlssHalton(index, 2) - 0.5f,
            dlssHalton(index, 3) - 0.5f};
}

// Additional opt-in policy: turn off subpixel jitter on fast object
// translation or abrupt costume changes. Scene-level, conservative policy;
// it must not alter the engine's logical frame/sprite coordinates.
struct DlssJitterSubject {
    int id = 0;
    float x = 0, y = 0; // current center in input-render pixels
    float size = 100;
    float direction = 90;
    int costume = 0;
};

class DlssJitterMotionGate {
public:
    // Returns whether raster jitter is safe on this frame.
    bool update(std::span<const DlssJitterSubject> visible, int width, int height,
                float maxTranslation = 3.0f) {
        if (width < 1 || height < 1 || width != width_ || height != height_)
            reset();
        width_ = width;
        height_ = height;
        const bool first = !initialized_;
        const float limit = std::clamp(maxTranslation, 0.1f, 32.0f);
        // There is no motion estimate on the first observed frame.
        // Do not inject a one-frame jitter pulse immediately before
        // fast movement can be detected.
        bool safe = !first;
        if (!first && previous_.size() != visible.size())
            safe = false;
        std::unordered_map<int, DlssJitterSubject> next;
        next.reserve(visible.size());
        for (const auto& subject : visible) {
            if (!std::isfinite(subject.x) || !std::isfinite(subject.y) ||
                !std::isfinite(subject.size) ||
                !std::isfinite(subject.direction)) {
                safe = false;
                continue;
            }
            const auto previous = previous_.find(subject.id);
            if (!first && (previous == previous_.end() ||
                std::abs(subject.x - previous->second.x) > limit ||
                std::abs(subject.y - previous->second.y) > limit ||
                std::abs(subject.size - previous->second.size) > 20 ||
                std::abs(subject.direction - previous->second.direction) > 30 ||
                subject.costume != previous->second.costume))
                safe = false;
            if (!next.emplace(subject.id, subject).second)
                safe = false;
        }
        previous_.swap(next);
        initialized_ = true;
        return safe;
    }

    void reset() {
        previous_.clear();
        initialized_ = false;
        width_ = height_ = 0;
    }
private:
    std::unordered_map<int, DlssJitterSubject> previous_;
    int width_ = 0, height_ = 0;
    bool initialized_ = false;
};

} // namespace xyo::render
