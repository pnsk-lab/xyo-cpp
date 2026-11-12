// Link against the same headless libxyo/header snapshot as the tested JIT.
// The host resumes exactly one Fiber slice and changes inputs only after an
// actual native yield. Both baseline and region must publish/reload exactly.
#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace xyo;

struct Input {
    const char16_t* text;
    double number;
};

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void run(const std::vector<uint8_t>& bytes, bool region) {
    unsetenv("XYO_PROF");
    unsetenv("XYO_BLOCK_COUNT");
    setenv("XYO_DANGER", "0", 1);
    setenv("XYO_JIT_CALLFRAME", "0", 1);
    setenv("XYO_JIT_COMMON_CFG", "0", 1);
    setenv("XYO_JIT_OSR", "0", 1);
    setenv("XYO_JIT_MODE", "eager", 1);
    setenv("XYO_GPU", "0", 1);
    setenv("XYO_PAR", "0", 1);
    setenv("XYO_PARFOR", "0", 1);
    setenv("XYO_JIT_NUMERIC_REGION", region ? "1" : "0", 1);
    Runtime rt;
    rt.warpTimeMs = -1.0; // Force the real deadline poll at each 64-tick phase.
    auto loaded = loadSb3File(rt, bytes.data(), bytes.size());
    require(loaded.ok, "load failed");
    Target* target = rt.getSpriteTargetByName(u"Bench");
    require(target != nullptr, "Bench target missing");
    auto* x = target->lookupVariableById("v_x");
    auto* index = target->lookupVariableById("v_i");
    auto* coefficient = target->lookupVariableById("v_coeff");
    require(x && index && coefficient, "fixture fields missing");
    rt.greenFlag();
    require(rt.threads.size() == 1, "fixture must start one thread");
    Thread* thread = rt.threads.front();
    const Input changes[] = {{u"0", 0}, {nullptr, 3}, {u" 7 ", 7},
        {u"-0", -0.0}, {u"abc", 0}, {nullptr, 2097153}, {nullptr, 0.5},
        {u"0017", 17}};
    double multiplier = 17, expected = 1;
    Input active{u"0017", 17};
    unsigned completed = 0, handoffs = 0;
    for (unsigned slices = 0; slices < 128 && thread->status != STATUS_DONE; ++slices) {
        rt.sequencer->timer.start();
        rt.sequencer->stepThread(*thread);
        const auto now = static_cast<unsigned>(cast::toNumber(index->value));
        require(now >= completed && now <= 1025, "bad iteration counter");
        require(now - completed <= 64, "native slice skipped deadline checkpoint");
        for (unsigned i = completed; i < now; ++i) {
            double value = (std::isnan(expected) ? 0.0 : expected) * multiplier + 23;
            expected = std::fmod(value, 65521.0);
            if (expected / 65521.0 < 0) expected += 65521.0;
        }
        const double actual = cast::toNumberOrNaN(x->value);
        require((std::isnan(expected) && std::isnan(actual)) || actual == expected,
                "published state differs at native checkpoint");
        if (now == completed) continue;
        require(coefficient->value.isString() == (active.text != nullptr),
                "readonly input changed its Value tag");
        if (active.text)
            require(coefficient->value.asStrPtr()->view() == std::u16string_view(active.text),
                    "readonly String spelling was changed");
        completed = now;
        if (completed == 1025) break;
        const auto& next = changes[handoffs % (sizeof(changes) / sizeof(changes[0]))];
        coefficient->value = next.text
            ? Value::string(SStr::create(next.text)) : Value::num(next.number);
        multiplier = next.number;
        active = next;
        ++handoffs;
    }
    require(completed == 1025, "thread did not finish");
    require(handoffs >= 8, "did not exercise enough actual native yields");
    std::cout << "checkpoint region=" << region << " handoffs=" << handoffs
              << " completed=" << completed << " PASS\n";
}

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: checkpoint-smoke fixture.sb3 [...]");
        for (int i = 1; i < argc; ++i) {
            std::ifstream input(argv[i], std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            require(!bytes.empty(), "fixture cannot be read");
            run(bytes, false);
            run(bytes, true);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "checkpoint FAIL: " << error.what() << '\n';
        return 1;
    }
}
