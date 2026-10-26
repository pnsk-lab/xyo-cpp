// Exact differential coverage for hsvToRgb's achromatic and channel-cast fast paths.
#include "core/color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using xyo::Hsv;
using xyo::Rgb;

// Independent copy of the pre-fast-path formula. Only finite hues are passed:
// converting a nonfinite normalized hue to int has no defined C++ result.
Rgb oldHsvToRgb(Hsv hsv) {
    double h = hsv.h;
    if (!(h >= 0.0 && h < 360.0)) {
        h = std::fmod(h, 360.0);
        if (h < 0) h += 360;
    }
    double s = std::max(0.0, std::min(hsv.s, 1.0));
    double v = std::max(0.0, std::min(hsv.v, 1.0));
    int i = static_cast<int>(h / 60);
    double f = h / 60 - i;
    double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r, g, b;
    switch (i) {
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    default: r = v; g = t; b = p; break;
    }
    return {static_cast<int>(std::floor(r * 255)),
            static_cast<int>(std::floor(g * 255)),
            static_cast<int>(std::floor(b * 255))};
}
} // namespace

int test_color_fast_main() {
    int checks = 0, failures = 0;
    auto check = [&](Hsv hsv) {
        ++checks;
        Rgb expected = oldHsvToRgb(hsv);
        Rgb actual = xyo::color::hsvToRgb(hsv);
        if (expected.r != actual.r || expected.g != actual.g || expected.b != actual.b) {
            if (++failures <= 10)
                std::printf("FAIL hsv fast h=%.17g s=%.17g v=%.17g: (%d,%d,%d) != (%d,%d,%d)\n",
                            hsv.h, hsv.s, hsv.v, actual.r, actual.g, actual.b,
                            expected.r, expected.g, expected.b);
        }
    };
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double hues[] = {
        -std::numeric_limits<double>::max(), -1e20, -1080, -720,
        std::nextafter(-360.0, -inf), -360, std::nextafter(-360.0, inf),
        std::nextafter(-60.0, -inf), -60, std::nextafter(-60.0, inf),
        // Adding 360 to a tiny negative remainder rounds to exactly 360.
        // Its sector is 6, so the old switch's default must remain unchanged.
        -std::numeric_limits<double>::min(), -tiny, -0.0, 0.0, tiny,
        std::nextafter(60.0, -inf), 60, std::nextafter(60.0, inf),
        std::nextafter(120.0, -inf), 120, std::nextafter(120.0, inf),
        std::nextafter(180.0, -inf), 180, std::nextafter(180.0, inf),
        std::nextafter(240.0, -inf), 240, std::nextafter(240.0, inf),
        std::nextafter(300.0, -inf), 300, std::nextafter(300.0, inf),
        std::nextafter(360.0, -inf), 360,
        std::nextafter(360.0, inf), 720, 1080, 1e20,
        std::numeric_limits<double>::max()
    };
    // Every gray RGB and every byte brightness boundary, including both adjacent
    // representable values. Negative/NaN saturation clamps to zero in the old
    // formula; tiny positive saturation intentionally exercises the general path.
    const double sats[] = {0.0, -0.0, -1.0, -tiny, -inf, nan, tiny, 0.5, 1.0};
    for (int gray = 0; gray <= 255; ++gray) {
        Hsv hsv = xyo::color::rgbToHsv({gray, gray, gray});
        check(hsv);
        for (double h : hues) {
            for (double s : sats) {
                check({h, s, hsv.v});
                check({h, s, std::nextafter(hsv.v, -inf)});
                check({h, s, std::nextafter(hsv.v, inf)});
            }
        }
    }
    // Nonfinite s/v are defined by the existing ordered std::min/std::max
    // clamps, unlike nonfinite hue. Preserve that behavior without new policy.
    const double special[] = {-inf, -1, -tiny, -0.0, 0.0, tiny,
                              0.5, 1.0, std::nextafter(1.0, inf), 2, inf, nan};
    for (double h : hues)
        for (double s : special)
            for (double v : special) check({h, s, v});

    // Sweep every byte saturation/brightness pair at chromatic angles in all
    // six sectors. Both adjacent doubles at each s/v quantization boundary are
    // important: a real-arithmetic identity or reassociation can move a channel
    // across an integer after multiplication by 255. Keep the original formula
    // as the oracle rather than assuming an RGB -> HSV -> RGB round trip is exact.
    const double chromaticHues[] = {15, 30, 75, 90, 135, 150,
                                    195, 210, 255, 270, 315, 330};
    for (double h : chromaticHues) {
        for (int sat = 0; sat <= 255; ++sat) {
            double s = sat / 255.0;
            const double adjacentS[] = {std::nextafter(s, -inf), s,
                                         std::nextafter(s, inf)};
            for (int value = 0; value <= 255; ++value) {
                double v = value / 255.0;
                const double adjacentV[] = {std::nextafter(v, -inf), v,
                                             std::nextafter(v, inf)};
                for (double ss : adjacentS)
                    for (double vv : adjacentV) check({h, ss, vv});
            }
        }
    }

    // Also place q/t (not just v) on byte boundaries: at s=v=1 each
    // sector's interpolated channel is f or 1-f. Probe neighboring hue doubles
    // on each side of the derived threshold, including sector endpoints.
    for (int sector = 0; sector < 6; ++sector) {
        for (int channel = 0; channel <= 255; ++channel) {
            double h = sector * 60.0 + (channel / 255.0) * 60.0;
            check({std::nextafter(h, -inf), 1, 1});
            check({h, 1, 1});
            check({std::nextafter(h, inf), 1, 1});
        }
    }

    // Fixed integer generator: reproducible broad finite HSV differential set,
    // with equal representation of achromatic and chromatic saturation values.
    uint64_t state = UINT64_C(0xd1b54a32d192ed03);
    auto next = [&]() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * UINT64_C(2685821657736338717);
    };
    auto unit = [&]() { return double(next() >> 11) * 0x1p-53; };
    for (int i = 0; i < 200000; ++i) {
        double h = (unit() * 2.0 - 1.0) * 1e8;
        double s = i % 2 ? unit() * 3.0 - 1.0 : 0.0;
        double v = unit() * 3.0 - 1.0;
        check({h, s, v});
    }
    // Random finite hues spanning large binary64 exponents exercise
    // fmod with huge arguments; ldexp(unit(), exponent <= 1023) stays finite.
    for (int i = 0; i < 20000; ++i) {
        double mantissa = unit();
        int exponent = static_cast<int>(next() % 1024);
        double h = std::ldexp(mantissa, exponent);
        if (next() & 1) h = -h;
        check({h, unit(), unit()});
    }

    // Optional all-24-bit-RGB differential sweep, using the existing test entry
    // point. Compare against the original formula, not the input RGB: existing
    // floating-point/truncation round-trip losses must not be "fixed" here.
    const char* exhaustive = std::getenv("XYO_TEST_COLOR_EXHAUSTIVE");
    if (exhaustive && exhaustive[0] == '1' && exhaustive[1] == '\0') {
        for (int r = 0; r <= 255; ++r)
            for (int g = 0; g <= 255; ++g)
                for (int b = 0; b <= 255; ++b)
                    check(xyo::color::rgbToHsv({r, g, b}));
    }
    std::printf("color fast: %d checks, %d failures\n", checks, failures);
    return failures;
}
