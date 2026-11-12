// Standalone: link against the SAME isolated headless headers/libxyo as the
// tested JIT, like numeric_region_checkpoint_smoke.cpp. Run with fixture.sb3.
// This is a checkpoint correctness test, not a throughput benchmark.
//
// Host changes happen only after a real suspended native Fiber slice. Neither
// the SList object/Variable binding nor source Blocks are replaced. Alternative
// storage is legal through public SList APIs; a fast raw read must recheck extra,
// length and items after each handoff. numericOnly is NOT an item type proof.
#include "core/cast.hpp"
#include "core/slist.hpp"
#include "engine/fiber.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xyo;

namespace {
constexpr unsigned kIterations = 1025;

enum class Profile { Base, List, Finite };

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

const char* profileName(Profile profile) {
    switch (profile) {
    case Profile::Base: return "BASE";
    case Profile::List: return "LIST";
    case Profile::Finite: return "FINITE";
    }
    return "unknown";
}

void configure(Profile profile, uint32_t mode) {
    for (const char* name : {"XYO_PROF", "XYO_BLOCK_COUNT", "XYO_TOPPROF",
                            "XYO_TOPTRACE", "XYO_JIT_DISABLE"}) unsetenv(name);
    for (const char* name : {"XYO_DANGER", "XYO_JIT_CALLFRAME",
                            "XYO_JIT_COMMON_CFG", "XYO_JIT_OSR", "XYO_GPU",
                            "XYO_PAR", "XYO_PARFOR", "XYO_JIT_ASYNC_WARMUP_MS",
                            "XYO_JIT_NUMERIC_REGION_COST_SELECT"})
        setenv(name, "0", 1);
    setenv("XYO_JIT_MODE", "eager", 1);
    setenv("XYO_JIT_CONDITIONAL_SSA", "1", 1);
    setenv("XYO_JIT_NUMERIC_REPEAT_SSA", "1", 1);
    const char* enabled = profile == Profile::Base ? "0" : "1";
    setenv("XYO_JIT_NUMERIC_REGION", enabled, 1);
    setenv("XYO_JIT_NUMERIC_REGION_MIXED", enabled, 1);
    setenv("XYO_JIT_NUMERIC_REGION_FACT_GUARDS", enabled, 1);
    setenv("XYO_JIT_NUMERIC_REGION_READ_LIST", enabled, 1);
    setenv("XYO_JIT_NUMERIC_REGION_LIST_FINITE",
           profile == Profile::Finite ? "1" : "0", 1);
    setenv("XYO_LIST_LIMIT", "0", 1);
    setenv("XYO_LIST_STORAGE", mode == 0 ? "flat" : mode == 4 ? "deque" : "adaptive", 1);
    // The focused-test global is authoritative before Runtime/load allocations.
    xyo_list_storage_mode = mode;
    xyo_list_limit = 0;
}

Value string(const char16_t* text) { return Value::string(SStr::create(text)); }

Value mixedValue(uint32_t index) {
    switch (index % 16) {
    case 0: return Value::num(3);
    case 1: return string(u"0017");
    case 2: return Value::nan();
    case 3: return Value::num(std::numeric_limits<double>::infinity());
    case 4: return Value::num(-std::numeric_limits<double>::infinity());
    case 5: return Value::num(-0.0);
    case 6: return string(u" 7 ");
    case 7: return string(u"abc");
    case 8: return Value::emptyStr();
    case 9: return Value::boolean(true);
    case 10: return string(u"Infinity");
    case 11: return string(u"-Infinity");
    case 12: return string(u"NaN");
    case 13: return string(u" ");
    case 14: return Value::boolean(false);
    default: return string(u"-0");
    }
}

void refill(SList& list, uint32_t length, unsigned kind) {
    list.clear();
    for (uint32_t i = 0; i < length; ++i) {
        Value value;
        switch (kind) {
        case 0: value = Value::num(double(i % 64 + 1)); break;
        case 1: value = mixedValue(i); break;
        case 2: value = string(i % 2 ? u"0017" : u" 7 "); break;
        case 3: value = Value::num(-0.0); break;
        case 4: value = Value::nan(); break;
        default: value = Value::num(-double(i % 31)); break;
        }
        list.push(value);
    }
    require(list.len == length, "host refill was truncated");
}

struct Coverage {
    unsigned directStrings = 0;
    unsigned capacityGrowths = 0;
    unsigned epochChanges = 0;
    unsigned adaptivePromotions = 0;
    unsigned extraSlices = 0;
};

void promoteAdaptive(SList& list, Coverage& coverage) {
    require(list.len >= 256, "adaptive phase needs a sufficiently long list");
    // Insert/erase in pairs preserves all original logical values. The repeated
    // local middle edits cross the public adaptive promotion threshold (24).
    for (unsigned i = 0; i < 24; ++i) {
        list.insert0(128, Value::num(99));
        list.erase0(128);
    }
    require(list.extra != nullptr, "adaptive edits did not promote storage");
    ++coverage.adaptivePromotions;
}

void mutate(SList& list, uint32_t mode, unsigned phase, Coverage& coverage) {
    const auto oldCap = list.cap;
    const auto oldEpoch = list.storageEpoch;
    switch (phase % 16) {
    case 0: refill(list, 0, 0); break;
    case 1: refill(list, 8, 0); break;
    case 2:
        refill(list, 64, 0);
        if (!list.extra) {
            require(list.numericOnly, "expected unsullied numericOnly feedback before direct writes");
            // Deliberately bypass numericOnly feedback through the public flat
            // ABI. A backend trusting the list-wide flag will misread Strings.
            for (uint32_t i = 0; i < list.len; ++i)
                list.items[i] = string(i % 2 ? u"0017" : u" 7 ");
            require(list.numericOnly, "direct write unexpectedly changed numericOnly");
            ++coverage.directStrings;
        }
        break;
    case 3: refill(list, 1024, 0); break;
    case 4: refill(list, 64, 1); break;
    case 5: refill(list, 64, 1); break;
    case 6: refill(list, 0, 0); break;
    case 7: refill(list, 1024, 1); break;
    case 8: refill(list, 8, 2); break;
    case 9: refill(list, 64, 3); break;
    case 10: refill(list, 1024, 0); break;
    case 11: refill(list, 64, 1); break;
    case 12: refill(list, 0, 0); break;
    case 13: refill(list, 64, 2); break;
    case 14: refill(list, 1024, 5); break;
    case 15: refill(list, 8, 4); break;
    }
    if (list.cap > oldCap) ++coverage.capacityGrowths;
    if (list.storageEpoch != oldEpoch) ++coverage.epochChanges;
    if (mode == 7 && list.len == 1024) promoteAdaptive(list, coverage);
}

std::vector<uint64_t> snapshot(const SList& list) {
    std::vector<uint64_t> result;
    result.reserve(list.len);
    for (uint32_t i = 0; i < list.len; ++i) result.push_back(list.get0(i).bits);
    return result;
}

bool sameNumber(double expected, double actual) {
    if (std::isnan(expected)) return std::isnan(actual);
    if (expected != actual) return false;
    return expected != 0.0 || std::signbit(expected) == std::signbit(actual);
}

void run(const std::vector<uint8_t>& bytes, Profile profile, uint32_t mode) {
    configure(profile, mode);
    const std::string label = std::string(profileName(profile)) + "/mode=" + std::to_string(mode);
    Runtime rt;
    rt.warpTimeMs = -1.0; // Poll MUST hand off at each original 64-tick boundary.
    rt.dangerMode = false;
    rt.blockCountEnabled = false;
    auto loaded = loadSb3File(rt, bytes.data(), bytes.size());
    require(loaded.ok, label + ": fixture load failed");
    auto* target = rt.getSpriteTargetByName(u"Bench");
    require(target != nullptr, label + ": Bench target missing");
    auto* index = target->lookupVariableById("v_i");
    auto* accumulator = target->lookupVariableById("v_acc");
    auto* variable = target->lookupVariableById("l_data");
    require(index && accumulator && variable && variable->type == Variable::kListType &&
            variable->list, label + ": fixture fields missing");
    SList* const list = variable->list;
    require(list->len == 64, label + ": initial list length must be 64");
    for (uint32_t i = 0; i < 64; ++i)
        require(list->get0(i).isNumber() && cast::toNumber(list->get0(i)) == double(i + 1),
                label + ": initial list must contain Numbers 1..64");
    rt.greenFlag();
    require(rt.threads.size() == 1, label + ": expected one thread");
    auto* thread = rt.threads.front();
    Coverage coverage;
    unsigned completed = 0, handoffs = 0, mutations = 0;
    double expected = 0;
    for (unsigned slices = 0; slices < 128 && thread->status != STATUS_DONE; ++slices) {
        const auto before = snapshot(*list);
        if (list->extra) ++coverage.extraSlices;
        rt.sequencer->timer.start();
        rt.sequencer->stepThread(*thread); // Exactly one native Fiber resume.
        require(thread->isCompiled, label + ": test did not use native JIT");
        require(variable->list == list, label + ": fixture rebound its SList");
        require(list->len == before.size(), label + ": readonly loop changed list length");
        for (uint32_t i = 0; i < list->len; ++i)
            require(list->get0(i).bits == before[i],
                    label + ": readonly loop changed item bits/String spelling");
        const double rawIndex = cast::toNumberOrNaN(index->value);
        require(std::isfinite(rawIndex) && rawIndex >= completed && rawIndex <= kIterations &&
                std::trunc(rawIndex) == rawIndex, label + ": invalid iteration counter");
        const auto now = static_cast<unsigned>(rawIndex);
        require(now - completed <= 64, label + ": native slice skipped a deadline checkpoint");
        // No list writes occur inside the body. Its current logical contents
        // equal the snapshot for ALL iterations completed in this single slice.
        // get0 deliberately supports deque/adaptive; it may acquire benign
        // storage feedback in future implementations, never change values.
        for (unsigned step = completed + 1; step <= now; ++step) {
            const uint32_t offset = step % 64; // Scratch (i % 64) + 1, zero-based.
            const double element = offset < list->len ? cast::toNumber(list->get0(offset)) : 0.0;
            expected = (std::isnan(expected) ? 0.0 : expected) + element;
        }
        const double actual = cast::toNumberOrNaN(accumulator->value);
        require(accumulator->value.isNumber() && sameNumber(expected, actual),
                label + ": accumulator mismatch at completed=" + std::to_string(now));
        const bool advanced = now > completed;
        completed = now;
        if (thread->status == STATUS_DONE) break;
        // Released Fiber has no public started() accessor. Native execution,
        // observed counter progress and an unfinished Fiber establish handoff.
        require(thread->fiber && !thread->fiber->finished(),
                label + ": return was not an actual suspended native Fiber");
        ++handoffs;
        if (advanced && completed < kIterations) mutate(*list, mode, mutations++, coverage);
    }
    require(thread->status == STATUS_DONE && completed == kIterations,
            label + ": thread did not finish exactly 1025 iterations");
    require(handoffs >= 8 && mutations >= 8, label + ": insufficient actual native handoffs");
    require(mutations >= 16, label + ": mutation schedule was not fully exercised");
    if (mode == 0 || mode == 7) {
        require(coverage.directStrings > 0, label + ": no stale-numericOnly challenge");
        require(coverage.capacityGrowths > 0 && coverage.epochChanges > 0,
                label + ": did not change flat capacity/storage epoch");
    }
    if (mode == 4 || mode == 7)
        require(coverage.extraSlices > 0, label + ": did not resume with alternative storage");
    if (mode == 7)
        require(coverage.adaptivePromotions > 0, label + ": no adaptive promotion");
    std::cout << "list-checkpoint profile=" << profileName(profile) << " mode=" << mode
              << " handoffs=" << handoffs << " mutations=" << mutations
              << " growths=" << coverage.capacityGrowths << " epoch_changes=" << coverage.epochChanges
              << " direct_strings=" << coverage.directStrings
              << " adaptive_promotions=" << coverage.adaptivePromotions
              << " extra_slices=" << coverage.extraSlices
              << " completed=" << completed << " PASS\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: numeric-region-list-checkpoint-smoke fixture.sb3 [...]");
        for (int i = 1; i < argc; ++i) {
            std::ifstream input(argv[i], std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            require(!bytes.empty(), std::string("cannot read fixture: ") + argv[i]);
            for (auto profile : {Profile::Base, Profile::List, Profile::Finite})
                for (uint32_t mode : {0u, 4u, 7u}) run(bytes, profile, mode);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "list-checkpoint FAIL: " << error.what() << '\n';
        return 1;
    }
}
