// xyo-cpp: 乱数。Math.random() と同じく [0,1) の 53bit 一様 double を生成。
// 分布の一致のみ要求（シードの一致は求めない — design.md §5.3）。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <random>

namespace xyo {

class Rng {
public:
    Rng() { seed(std::random_device{}() * 0x9E3779B97F4A7C15ull + 1); }
    explicit Rng(uint64_t s) { seed(s); }

    void seed(uint64_t s) {
        state_ = 0;
        inc_ = (s << 1) | 1;
        next();
        state_ += s;
        next();
    }

    uint32_t next() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        uint32_t x = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
        uint32_t rot = static_cast<uint32_t>(old >> 59);
        return (x >> rot) | (x << ((-rot) & 31));
    }

    /// [0,1) — V8 Math.random() と同じ 53bit 方式
    double nextDouble() {
        uint64_t hi = static_cast<uint64_t>(next());
        uint64_t lo = static_cast<uint64_t>(next());
        uint64_t bits = (hi << 21) ^ (lo >> 11);   // 53 bits 使用
        return (bits & ((1ull << 53) - 1)) * (1.0 / 9007199254740992.0);
    }

    /// [low, high] 整数（両端含む）
    int64_t nextIntRange(int64_t low, int64_t high) {
        return low + static_cast<int64_t>(nextDouble() * (high + 1 - low));
    }

    /// スレッドローカル RNG — design.md §5.3 でシード一致は不要と
    /// 定義しているため、worker 毎に独立ストリームを持たせる
    /// （並列実行時の共有状態・順序依存を排除）。
    /// XYO_SEED=<n> を設定すると全スレッドが同じ列で開始する —
    /// AOT/JIT/インタプリタの意味論一致検査用（並列実行時は
    /// スケジューリング順で揺れるため XYO_PAR=0 と併用する）。
    static Rng& global() {
        static thread_local Rng r = makeGlobal();
        return r;
    }

private:
    static Rng makeGlobal() {
        if (const char* s = std::getenv("XYO_SEED"))
            return Rng(std::strtoull(s, nullptr, 0));
        return Rng();
    }

    uint64_t state_ = 0, inc_ = 1;
};

} // namespace xyo
