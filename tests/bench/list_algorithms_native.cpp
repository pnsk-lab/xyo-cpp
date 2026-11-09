// Standalone storage benchmark (no GC/JIT), plus deterministic parity checks.
// g++ -O3 -std=c++20 -Isrc src/core/list_storage.cpp tests/bench/list_algorithms_native.cpp -o build/list_algorithms_native
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>
#include "core/list_storage.hpp"

using xyo::Value;
using xyo::ListStore;

template <class F>
double milliseconds(F&& f) {
    const auto a = std::chrono::steady_clock::now();
    f();
    const auto b = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Action {
    bool insert;
    uint32_t index;
    uint64_t value;
};

struct Result {
    double appendMs, readMs, editMs;
    uint64_t checksum;
};

Result run(uint32_t mode, const std::vector<Action>& actions,
           const std::vector<uint32_t>& reads) {
    constexpr uint32_t initial = 20000, appendCount = 200000;
    std::unique_ptr<ListStore> store(mode ? xyo::makeListStore(mode, initial) : nullptr);
    std::vector<Value> flat;
    if (!store) flat.reserve(initial);
    auto size = [&]() -> uint32_t { return store ? store->size() : uint32_t(flat.size()); };
    auto at = [&](uint32_t i) -> Value { return store ? store->get(i) : flat[i]; };
    auto push = [&](Value v) {
        if (store) store->insert(size(), v);
        else flat.push_back(v);
    };
    auto insert = [&](uint32_t i, Value v) {
        if (store) store->insert(i, v);
        else flat.insert(flat.begin() + i, v);
    };
    auto erase = [&](uint32_t i) {
        if (store) store->erase(i);
        else flat.erase(flat.begin() + i);
    };
    for (uint32_t i = 0; i < initial; ++i) push(Value::num(double(i)));
    volatile uint64_t checksum = 0;
    const double readMs = milliseconds([&] {
        uint64_t local = 0;
        for (uint32_t i : reads) local += at(i).bits;
        checksum = local;
    });
    const double editMs = milliseconds([&] {
        for (auto a : actions) {
            if (a.insert) insert(a.index, Value::num(double(a.value)));
            else erase(a.index);
        }
    });
    // Verify the entire edited sequence, not just the later append experiment.
    uint64_t editChecksum = 0;
    for (uint32_t i = 0; i < size(); ++i)
        editChecksum ^= at(i).bits * (uint64_t(i + 1) * 0x9E3779B97F4A7C15ull);
    // Different scenario for append: reset the existing container first.
    if (store) store->clear();
    else flat.clear();
    const double appendMs = milliseconds([&] {
        for (uint32_t i = 0; i < appendCount; ++i)
            push(Value::num(double(i)));
    });
    uint64_t last = 0;
    for (uint32_t i = 0; i < size(); i += 31)
        last += at(i).bits;
    return {appendMs, readMs, editMs, uint64_t(checksum) ^ last ^ editChecksum};
}

int main() {
    constexpr uint32_t initial = 20000;
    constexpr uint32_t editCount = 8000;
    std::mt19937 engine(0xC0FFEE);
    std::vector<Action> actions;
    uint32_t count = initial;
    for (uint32_t i = 0; i < editCount; ++i) {
        const bool insert = (i % 2 == 0);
        const uint32_t index = engine() % (count + (insert ? 1 : 0));
        actions.push_back({insert, index, i + 1});
        count += insert ? 1 : -1;
    }
    std::vector<uint32_t> reads;
    for (uint32_t i = 0; i < 200000; ++i)
        reads.push_back(engine() % initial);
    std::printf("mode    append(ms)  random-get(ms)  middle-edit(ms)  checksum\n");
    uint64_t expected = 0;
    for (uint32_t mode : {0u, 4u, 5u, 6u}) {
        const Result r = run(mode, actions, reads);
        if (mode == 0) expected = r.checksum;
        if (expected != r.checksum) {
            std::fprintf(stderr, "FAIL mode=%u checksum mismatch\n", mode);
            return 1;
        }
        std::printf("%-7s %10.3f %15.3f %16.3f  %llx\n",
                    mode == 0 ? "flat" : mode == 4 ? "deque" :
                    mode == 5 ? "gap" : "chunked", r.appendMs,
                    r.readMs, r.editMs,
                    static_cast<unsigned long long>(r.checksum));
    }
}
