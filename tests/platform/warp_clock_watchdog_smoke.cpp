// Repeated watchdog construction, arming, disarming and joining.
// Compile this standalone with warp_clock.cpp under TSan to detect
// the worker starting before later state members are initialized.
#include "../../src/engine/warp_clock.hpp"

#include <cstdio>
#include <thread>

int main() {
    constexpr int kCycles = 256;
    for (int i = 0; i < kCycles; ++i) {
        xyo::WarpClockWatchdog watchdog((i & 1) != 0);
        if (watchdog.armed()) return 1;
        watchdog.arm(0.0, (i % 3) == 0);
        if (!watchdog.armed()) return 2;
        std::this_thread::yield();
        watchdog.disarm();
        if (watchdog.armed()) return 3;
        watchdog.arm(0.0, (i % 3) != 0);
        watchdog.disarm();
        if (watchdog.armed()) return 4;
        // ~WarpClockWatchdog joins the worker before destroying its state.
    }
    std::printf("PASS %d watchdog init/arm/disarm/join cycles\n", kCycles);
    return 0;
}
