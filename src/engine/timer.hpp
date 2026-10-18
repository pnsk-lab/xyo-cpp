// xyo-cpp: util/timer.js 相当。
// nowObj 差し替え可能（runtime.currentMSecs を使うブロックタイマ用）。
#pragma once

#include <chrono>
#include <functional>

namespace xyo {

class Timer {
public:
    using NowFn = std::function<double()>;

    Timer() = default;
    explicit Timer(NowFn nowFn) : nowFn_(std::move(nowFn)) {}

    double now() const {
        return nowFn_ ? nowFn_() : stdNow();
    }
    void start() { startTime_ = now(); }
    double timeElapsed() const { return now() - startTime_; }
    double startTime() const { return startTime_; }
    /// Clock::resume 用（JS は startTime += dt）
    void setStartTime(double t) { startTime_ = t; }

    static double stdNow() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    NowFn nowFn_;
    double startTime_ = 0;
};

} // namespace xyo
