// Shared immutable strings still have mutable parse caches. Exercise cold
// publications and hot readers without relying on a Variable* dependency edge.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

using namespace xyo;

int test_parallel_strings_main() {
    Gc::DeferCollect defer; // Native worker stacks are not GC root providers.
    std::atomic<int> failures{0};
    constexpr unsigned workers = 8;
    constexpr unsigned rounds = 12;
    for (unsigned round = 0; round < rounds; ++round) {
        struct NumberCase { SStr* str; double number; uint32_t state; };
        std::vector<NumberCase> numbers = {
            {SStr::create(u"42"), 42, 1},
            {sstrFromAscii("00042"), 42, 1},
            {sstrConcat(u"3", u".5"), 3.5, 1},
            {sstrConcatAscii(u"0x", "10"), 16, 1},
            {SStr::create(u"not a number"), 0, 2},
            {SStr::create(u"   "), 0, 3},
            {SStr::create(u"Infinity"), INFINITY, 4},
            {SStr::create(u"-Infinity"), -INFINITY, 4},
        };
        SStr* hex = SStr::create(u"#1a2b3c");
        SStr* decimal = sstrFromAscii("16711680");
        // letterOf returns the process-wide ASCII cache, shared even between
        // scripts whose variables and literal strings are otherwise distinct.
        Value ascii = rt::letterOf(Value::num(1),
                                   Value::string(SStr::create(u"7")));
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                auto check = [&](bool ok) {
                    if (!ok) failures.fetch_add(1, std::memory_order_relaxed);
                };
                for (unsigned repeat = 0; repeat < 64; ++repeat) {
                    for (size_t i = 0; i < numbers.size(); ++i) {
                        auto& c = numbers[(i + worker) % numbers.size()];
                        Value value = Value::string(c.str);
                        check(cast::toNumber(value) == c.number);
                        check(c.str->loadNumState() == c.state);
                        // Read the payload too, including NaN's separate state.
                        double cached = c.str->loadNumCache();
                        check(c.state == 2 ? std::isnan(cached)
                                           : cached == c.number);
                        if (c.number == 42) {
                            // A second publisher path must remain atomic too.
                            check(cast::listIndex(value, 100) == 41);
                        }
                    }
                    Rgba h = cast::toRgbColorObject(Value::string(hex));
                    check(h.r == 26 && h.g == 43 && h.b == 60 && h.a == 255);
                    Rgba d = cast::toRgbColorObject(Value::string(decimal));
                    check(d.r == 255 && d.g == 0 && d.b == 0 && d.a == 255);
                    check(cast::toNumber(ascii) == 7);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != workers)
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
    }
    std::printf("parallel strings: %u cold rounds, %d failures\n", rounds,
                failures.load());
    return failures.load();
}
