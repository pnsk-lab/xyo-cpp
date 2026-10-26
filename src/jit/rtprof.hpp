// xyo-cpp: JIT ヘルパ用 rdtsc プロファイラ（XYO_PROF=1 で有効）。
// extern "C" ヘルパの入口で XYO_RT_PROF(slot) を置くだけで、
// 終了時にスロット別の消費時間・呼出回数を stderr へ集計出力する。
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace xyo {

/// 高分解能タイマ。x86 では rdtsc、それ以外（WASM 等）では
/// steady_clock のナノ秒を「サイクル」として扱う（tscHz=1e9 に校正）。
inline uint64_t rtProfNow() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct RTProf {
    enum Slot {
        kReporter = 0, kCompat, kYield, kList, kVar, kCast,
        kArith, kStr, kBlocks, kArg, kProc, kGc, kBcast, kLoopY,
        kPen, kOther, kMax
    };
    static constexpr const char* kNames[kMax] = {
        "reporter", "compat", "yield", "list", "var", "cast",
        "arith", "str", "blocks", "arg", "proc", "gc", "bcast", "loopy",
        "pen", "other",
    };
    std::atomic<uint64_t> cycles[kMax]{};
    std::atomic<uint64_t> calls[kMax]{};
    bool enabled;
    double tscHz = 0;  // キャリブレーション済み TSC 周波数

    RTProf() : enabled(std::getenv("XYO_PROF") != nullptr) {
        if (!enabled) return;
#if defined(__x86_64__) || defined(__i386__)
        auto t0 = std::chrono::steady_clock::now();
        uint64_t c0 = __rdtsc();
        // 軽い busy wait で TSC 周波数を校正（~5ms）
        while (std::chrono::steady_clock::now() - t0 <
               std::chrono::milliseconds(5)) {
        }
        tscHz = (__rdtsc() - c0) /
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
#else
        tscHz = 1e9;   // rtProfNow() はナノ秒を返す
#endif
    }
    ~RTProf() {
        if (!enabled) return;
        std::fprintf(stderr, "[rtprof] tsc=%.2fGHz\n", tscHz / 1e9);
        for (int i = 0; i < kMax; ++i) {
            uint64_t cy = cycles[i].load(), n = calls[i].load();
            if (!n) continue;
            std::fprintf(stderr,
                         "[rtprof] %-8s %8.1fms  %10llu calls  %6.0fns/call\n",
                         kNames[i], cy / tscHz * 1e3,
                         (unsigned long long)n,
                         n ? double(cy) / double(n) / tscHz * 1e9 : 0.0);
        }
    }
    static RTProf& get() {
        static RTProf p;
        return p;
    }
};

struct RTProfScope {
    uint64_t t0;
    int slot;
    explicit RTProfScope(int s) : slot(s) {
        t0 = RTProf::get().enabled ? rtProfNow() : 0;
    }
    ~RTProfScope() {
        if (t0) {
            RTProf::get().cycles[slot]
                .fetch_add(rtProfNow() - t0, std::memory_order_relaxed);
            RTProf::get().calls[slot]
                .fetch_add(1, std::memory_order_relaxed);
        }
    }
};

namespace detail {
/// map dos contadores por nome de função — heap-leak intencional para
/// o lambda do atexit acessar sem captura (static não é capturável).
inline std::mutex& rtProfNameMutex() {
    // Same lifetime as the intentionally leaked map, including atexit output.
    static auto* m = new std::mutex;
    return *m;
}
inline std::unordered_map<const char*, uint64_t>& rtProfNameMap() {
    static auto* m = new std::unordered_map<const char*, uint64_t>;
    return *m;
}
} // namespace detail

/// 関数名別の呼出回数（XYO_PROF 時、atexit で降順表示）。
/// inline — gc.cpp (core) も XYO_RT_PROF を使うため、ヘッダに実装を
/// 置かないと XYO_JIT=OFF ビルドでリンクエラーになる。
inline void rtProfBump(const char* name) {
    static int once = []{
        std::atexit([]{
            std::lock_guard lock(detail::rtProfNameMutex());
            auto& m = detail::rtProfNameMap();
            std::vector<std::pair<const char*, uint64_t>> v(
                m.begin(), m.end());
            std::sort(v.begin(), v.end(),
                      [](auto& a, auto& b){ return a.second > b.second; });
            for (size_t i = 0; i < v.size() && i < 20; ++i)
                std::fprintf(stderr, "[fn] %s %llu\n", v[i].first,
                             (unsigned long long)v[i].second);
        });
        return 0; }();
    (void)once;
    std::lock_guard lock(detail::rtProfNameMutex());
    ++detail::rtProfNameMap()[name];
}

} // namespace xyo

#define XYO_RT_PROF(slot)                                              \
    ::xyo::RTProfScope _rtprof_(::xyo::RTProf::slot);                  \
    do {                                                             \
        if (::xyo::RTProf::get().enabled)                            \
            ::xyo::rtProfBump(__func__);                             \
    } while (0)
