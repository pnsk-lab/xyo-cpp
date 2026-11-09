// A/B: old per-slot ID lookup at refresh+commit vs generation-guarded
// borrowed binding pointers. Both reuse fixed-size buffers and perform
// the same value-type and optimistic transaction validation.
//
// This measures HOST snapshot management only, not JIT execution time.
// Compile explicitly: cmake --build build-ir-nogpu --target
//                     xyo-numeric-binding-cache -j4
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/scratch_variable_slots.hpp"
#include "../../src/engine/runtime.hpp"
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace xyo;
using SteadyClock = std::chrono::steady_clock;

struct Fixture {
    Runtime runtime;
    Target* stage = nullptr;
    Target* sprite = nullptr;
    std::vector<std::string> ids;

    explicit Fixture(int slots) {
        stage = new Target(&runtime);
        stage->isStage = true;
        runtime.addTarget(stage);
        sprite = new Target(&runtime);
        runtime.addTarget(sprite);
        ids.reserve(slots);
        for (int i = 0; i < slots; ++i) {
            std::string id = "var-" + std::to_string(i);
            Target* owner = (i & 1) ? stage : sprite;
            owner->variables.emplace(
                id, Variable::create(id, u"v",
                                      Variable::kScalarType, false));
            ids.push_back(std::move(id));
        }
    }
};

static bool isScalar(const Variable* v) {
    return v && v->type == Variable::kScalarType &&
           !v->isCloud && v->value.isNumber();
}

// Emulates the old refresh/commit path but avoids allocations in the A/B
// benchmark, isolating the price of TWO ID resolutions per live slot.
static void oldLookup(Fixture& f, int iterations) {
    const size_t n = f.ids.size();
    std::vector<Variable*> bound(n);
    std::vector<uint64_t> oldBits(n), staged(n);
    for (int tick = 0; tick < iterations; ++tick) {
        for (size_t i = 0; i < n; ++i) {
            bound[i] = f.sprite->lookupVariableById(f.ids[i]);
            assert(isScalar(bound[i]));
            oldBits[i] = bound[i]->value.bits;
            staged[i] = oldBits[i];
        }
        staged[0] = Value::num(double(tick + 1)).bits;
        for (size_t i = 0; i < n; ++i) {
            auto* v = f.sprite->lookupVariableById(f.ids[i]);
            assert(v == bound[i] && isScalar(v));
            assert(v->value.bits == oldBits[i]);
            assert(Value{staged[i]}.isNumber());
        }
        for (size_t i = 0; i < n; ++i) {
            bound[i]->value =
                Value::numCanonical(Value{staged[i]}.asNumberUnchecked());
            bound[i]->observeValueType(bound[i]->value);
        }
    }
}

static void cachedLookup(Fixture& f, int iterations) {
    ir::ScratchNumericSlots slots(*f.sprite, f.ids);
    assert(slots.valid());
    for (int tick = 0; tick < iterations; ++tick) {
        assert(slots.refresh());
        slots.data()[0] = Value::num(double(tick + 1)).bits;
        assert(slots.commit());
    }
}

template <typename Runner>
static double measure(int slots, int iterations, Runner&& runner) {
    double best = std::numeric_limits<double>::infinity();
    for (int rep = 0; rep < 5; ++rep) {
        Fixture fixture(slots);
        const auto start = SteadyClock::now();
        runner(fixture, iterations);
        const auto end = SteadyClock::now();
        assert(fixture.sprite->variables.at("var-0").value.bits ==
               Value::num(double(iterations)).bits);
        const auto ns = std::chrono::duration<double, std::nano>(
            end - start).count() / double(iterations);
        if (ns < best) best = ns;
    }
    return best;
}

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::stoi(argv[1]) : 100000;
    assert(iterations > 0 && iterations < 10000000);
    std::cout << "numeric binding snapshot benchmark: "
              << iterations << " slices, 5 runs (best)\n";
    for (int slots : {1, 8, 32}) {
        const double baseline = measure(slots, iterations, oldLookup);
        const double cached = measure(slots, iterations, cachedLookup);
        std::cout << std::fixed << std::setprecision(1)
                  << slots << " vars: per-ID lookup " << baseline
                  << " ns/slice, generation-cache " << cached
                  << " ns/slice, " << baseline / cached << "x\n";
    }
    return 0;
}
