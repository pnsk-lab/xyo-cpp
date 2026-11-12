// Real JIT/AOT arithmetic list projections with Values not representable in JSON.
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "aot/aot.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace xyo;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    const std::string json{std::istreambuf_iterator<char>(input), {}};
    if (json.empty()) return 2;
    constexpr uint64_t hash = 0x4c4c564d50524f4aull;
    setJitMode(JitMode::Eager);
    aot::BuildResult object;
    std::string error;
    {
        Runtime source;
        if (!loadSb3(source, json, nullptr).ok ||
            !aot::compileProject(source, hash, object, &error) || object.scripts == 0) {
            std::fprintf(stderr, "AOT build failed: %s\n", error.c_str()); return 1;
        }
    }
    unsigned checks = 0;
    for (int kind = 0; kind < 4; ++kind) {
        for (int mode = 0; mode < 3; ++mode) {
            setJitMode(mode == 0 ? JitMode::Off : JitMode::Eager);
            Runtime runtime;
            if (!loadSb3(runtime, json, nullptr).ok) return 1;
            Target* target = runtime.targets.back();
            auto* list = target->lookupVariableById("l_data")->list;
            std::vector<uint64_t> original;
            {
                Gc::DeferCollect defer;
                for (unsigned i = 0; i < 64; ++i) {
                    Value value;
                    if (kind == 0) {
                        value = i % 2 ? Value::undefined() : Value::color(0x12, 0x34, 0x56);
                    } else if (kind == 1) {
                        const uint64_t payloads[] = {val::kNan, 0xfff8000000001234ull,
                            0x7ff0000000000001ull, 0xfff0000000000001ull};
                        value = Value{payloads[i % 4]};
                    } else if (kind == 2) {
                        if (i % 3 == 0) value = Value::boolean(true);
                        else if (i % 3 == 1) value = Value::string(SStr::create(u"0017"));
                        else value = Value::string(SStr::create(u" bad "));
                    } else {
                        const double values[] = {0.0, -0.0, 5e-324, -5e-324,
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()};
                        value = Value::num(values[i % 6]);
                    }
                    list->push(value); original.push_back(value.bits);
                }
            }
            Value expected = Value::num(0);
            for (unsigned i = 1; i <= 129; ++i) {
                const Value value{original[(i * 7919) % 64]};
                expected = Value::numCanonical(cast::toNumber(expected) + cast::toNumber(value));
            }
            if (mode == 2 && !runtime.jit->loadAot(runtime, object.object.data(),
                                                 object.object.size(), hash, &error)) {
                std::fprintf(stderr, "AOT load failed: %s\n", error.c_str()); return 1;
            }
            runtime.greenFlag();
            if (mode && (runtime.threads.empty() || !runtime.threads[0]->isCompiled)) return 1;
            if (mode == 2) {
                JitEngine::ScriptStats stats;
                if (!runtime.jit->scriptStats(runtime.threads[0]->topBlockPtr, &stats) ||
                    !stats.compiled || !stats.aot) return 1;
            }
            runtime.runUntilDone(1000);
            Value got = target->lookupVariableById("v_acc")->value;
            if (got.isNumber() && std::isnan(got.asNumberUnchecked())) got = Value::nan();
            if (!runtime.threads.empty() || got.bits != expected.bits || list->len != original.size()) {
                std::fprintf(stderr, "FAIL kind=%d mode=%d got=%016llx expected=%016llx\n",
                    kind, mode, (unsigned long long)got.bits, (unsigned long long)expected.bits);
                return 1;
            }
            for (unsigned i = 0; i < original.size(); ++i) {
                if (list->get0(i).bits != original[i]) return 1;
                ++checks;
            }
            ++checks;
        }
    }
    std::printf("PASS raw list projections: %u checks, interpreter/JIT/AOT, original tags preserved\n", checks);
}
