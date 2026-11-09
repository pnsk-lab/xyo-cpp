// Conservative quality gate for NVIDIA Optical Flow costs.
// Cost is an error/confidence indicator, not a calibrated probability.
// High-cost cells suggest that temporal history is unreliable.
#pragma once

#include <cmath>

namespace xyo::render {
struct DlssOpticalCostThreshold {
    float mean = 8.0f;             // Empirical RTX 3070 starting value
    float highFraction = 0.70f;    // Fraction of 4x4 cells with cost >= 8
};

inline bool dlssMotionUnreliable(float meanCost,
                                 float fractionAboveEight,
                                 DlssOpticalCostThreshold threshold = {}) {
    if (!std::isfinite(meanCost) ||
        !std::isfinite(fractionAboveEight) ||
        meanCost < 0.0f || fractionAboveEight < 0.0f ||
        fractionAboveEight > 1.0f)
        return true;
    return meanCost >= threshold.mean &&
           fractionAboveEight >= threshold.highFraction;
}
} // namespace xyo::render
