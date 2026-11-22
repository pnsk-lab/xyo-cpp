// Shared host math operations imported by generated WASM HotSpot code.
// Operand buffers live in the Emscripten linear memory.
#include "../rt/runtime_helpers.hpp"
#include "../ir/host_effect_abi.hpp"
#include "string_number_bridge.hpp"
#include "list_host_bridge.hpp"
#include "string_host_bridge.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__EMSCRIPTEN__)
#include "../core/sstr.hpp"
#include "../blocks/pen_api.hpp"
#include "../engine/runtime.hpp"
namespace xyo { struct JitCtx; }
// Frozen at the start of each Runtime::step (the interpreter's project timer
// is constant during a step in the deterministic headless scheduler). The
// module-side sensing_timer import (id 17) reads this snapshot.
static thread_local double g_hostProjectTimer = 0.0;
static thread_local void* g_hostRuntime = nullptr;
static thread_local void* g_hostTarget = nullptr;
extern "C" void xyo_wasm_set_host_project_timer(double seconds) {
    g_hostProjectTimer = seconds;
}
// The Runtime is alive for the whole step; modules cannot hold a C++
// pointer themselves, so the engine publishes the active instance here.
extern "C" void xyo_wasm_set_host_runtime(void* runtime) {
    g_hostRuntime = runtime;
}
static xyo::Runtime* wasmRuntime() {
    return static_cast<xyo::Runtime*>(g_hostRuntime);
}
extern "C" void* xyo_wasm_get_host_runtime() { return g_hostRuntime; }
extern "C" void xyo_wasm_set_host_target(void* target) {
    g_hostTarget = target;
}
static xyo::Target* wasmTarget() {
    return static_cast<xyo::Target*>(g_hostTarget);
}
// This helper contains no LLVM dependency and uses Scratch's exact math
// implementation from the JIT runtime (also linked by backend_wasm).
extern "C" double xyo_mathop(xyo::JitCtx*, int32_t, double);

extern "C" int xyo_wasm_h(int id, int a, int b) {
    switch (id) {
    case 1: return a + b;  // legacy bridge selftest
    case 2: return a * b;  // legacy bridge selftest
    case 3: {
        // a points to two contiguous f64 operands in linear memory.
        // The result overwrites the first operand. Use memcpy to avoid
        // alignment/strict-aliasing assumptions across the WASM boundary.
        auto* base = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        double lhs = 0, rhs = 0;
        std::memcpy(&lhs, base, sizeof(lhs));
        std::memcpy(&rhs, base + sizeof(lhs), sizeof(rhs));
        const double result = xyo::rt::mod(lhs, rhs);
        std::memcpy(base, &result, sizeof(result));
        return 0;
    }
    case 4: {
        // b = Scratch mathop opcode 0..13, a = single f64 scratch slot.
        if (b < 0 || b > 13) return -1;
        auto* base = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        double value = 0;
        std::memcpy(&value, base, sizeof(value));
        const double result = xyo_mathop(nullptr, b, value);
        std::memcpy(base, &result, sizeof(result));
        return 0;
    }
    case 5: {
        auto* base = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::convertHeapStringNumberSlot(base) ? 0 : -1;
    }
    case 6: {
        // b is a host-bound list ID index, not a raw C++ pointer. The only
        // address crossing the import boundary is a host-owned scratch cell.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::loadBoundNumericListItem(
            scratch, static_cast<uint32_t>(b)) ? 0 : -1;
    }
    case 15: {
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        if (!scratch) return -1;
        const double now = xyo::rt::daysSince2000();
        std::memcpy(scratch, &now, sizeof(now));
        return 0;
    }
    case 27: {
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        if (!scratch) return -1;
        xyo::Runtime* runtime = wasmRuntime();
        const double now = runtime
            ? runtime->ioDevices.clock.projectTimer() : g_hostProjectTimer;
        std::memcpy(scratch, &now, sizeof(now));
        return 0;
    }
    case 28:
    case 29: {
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        xyo::Runtime* runtime = wasmRuntime();
        if (!scratch || !runtime) return -1;
        const double value = id == 28
            ? runtime->ioDevices.mouse.getScratchX()
            : runtime->ioDevices.mouse.getScratchY();
        std::memcpy(scratch, &value, sizeof(value));
        return 0;
    }
    case 30: {
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        xyo::Runtime* runtime = wasmRuntime();
        if (!scratch || !runtime) return -1;
        const uint64_t bits = runtime->ioDevices.mouse.getIsDown()
            ? xyo::val::kTrue : xyo::val::kFalse;
        std::memcpy(scratch, &bits, sizeof(bits));
        return 0;
    }
    case 40: {
        // Commit-only motion effect: b selects setx/sety/changex/changey.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        xyo::Target* target = wasmTarget();
        if (!scratch || !target) return -1;
        if (std::getenv("XYO_WASM_EFFECT_TRACE"))
            std::fprintf(stderr, "[wasm-effect] effect mode=%d\n", b);
        double value = 0;
        std::memcpy(&value, scratch, sizeof(value));
        // Scratch's ToNumber (num() at the primitive boundary) maps NaN to
        // 0; an arithmetic 0/0 result must not turn into a NaN target state.
        if (std::isnan(value)) value = 0.0;
        if (b >= 78 && b <= 80) {
            if (b == 78) target->setDirection(value);
            else if (b == 79)
                target->setDirection(target->direction + value);
            else
                target->setDirection(target->direction - value);
            return 0;
        }
        if (b >= 50) {
            xyo::Runtime* runtime = wasmRuntime();
            if (!runtime) return -1;
            static const char* const names[] = {
                "color", "fisheye", "whirl", "pixelate",
                "mosaic", "brightness", "ghost"};
            const auto clampEffect = [](std::string_view name, double v) {
                if (name == "ghost")
                    return std::min(std::max(v, 0.0), 100.0);
                if (name == "brightness")
                    return std::min(std::max(v, -100.0), 100.0);
                return v;
            };
            if (b >= 50 && b <= 56) {
                const std::string_view name = names[b - 50];
                target->setEffect(std::string(name),
                                  clampEffect(name, value));
            } else if (b >= 60 && b <= 66) {
                const std::string_view name = names[b - 60];
                double next = target->effects.get(name) + value;
                target->setEffect(std::string(name),
                                  clampEffect(name, next));
            } else if (b == 70) {
                target->clearEffects();
            } else if (b == 71) {
                target->setVisible(true);
            } else if (b == 72) {
                target->setVisible(false);
            } else if (b == 73) {
                xyo::penrt::stamp(target, runtime);
            } else if (b == 74) {
                uint64_t bits = 0;
                std::memcpy(&bits, scratch, sizeof(bits));
                xyo::penrt::setColorToColor(target, xyo::Value{bits});
            } else {
                return -1;
            }
            return 0;
        }
        if (b >= 9) {
            xyo::Runtime* runtime = wasmRuntime();
            if (!runtime) return -1;
            if (b == 9) xyo::penrt::clear(runtime);
            else if (b == 10) xyo::penrt::down(target, runtime);
            else if (b == 11) xyo::penrt::up(target, runtime);
            else if (b == 12 || b == 13)
                xyo::penrt::size(target, runtime, xyo::Value::num(value),
                                 b == 13);
            else if (b >= 20 && b <= 23)
                xyo::penrt::colorParamNum(target, b - 20, value, false);
            else if (b >= 30 && b <= 33)
                xyo::penrt::colorParamNum(target, b - 30, value, true);
            else return -1;
            return 0;
        }
        switch (b) {
        case 1: target->setXY(value, target->y); break;
        case 2: target->setXY(target->x, value); break;
        case 3: target->setXY(target->x + value, target->y); break;
        case 4: target->setXY(target->x, target->y + value); break;
        case 5: target->setSize(value); break;
        case 6: target->setSize(target->size + value); break;
        case 7:
            target->volume = std::min(std::max(value, 0.0), 100.0);
            break;
        case 8: {
            double volume = target->volume + value;
            target->volume = std::min(std::max(volume, 0.0), 100.0);
            break;
        }
        default: return -1;
        }
        return 0;
    }
    case 32: {
        // sensing_username: the compiled module may hold the tagged value
        // across further host calls, so the SStr is owned and marked by the
        // Runtime root provider instead of being a transient allocation.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        xyo::Runtime* runtime = wasmRuntime();
        if (!scratch || !runtime) return -1;
        const std::u16string& name = runtime->ioDevices.userData.getUsername();
        uint64_t bits = xyo::val::kEmptyStr;
        if (!name.empty()) {
            if (!runtime->usernameRoot ||
                runtime->usernameRoot->view() != name)
                runtime->usernameRoot = xyo::sstrCreate(name);
            bits = xyo::Value::string(runtime->usernameRoot).bits;
        }
        std::memcpy(scratch, &bits, sizeof(bits));
        if (std::getenv("XYO_WASM_EFFECT_TRACE"))
            std::fprintf(stderr, "[wasm-effect] username\n");
        return 0;
    }
    case 31: {
        // Same caching contract as the interpreter's sensing_loudness:
        // -1 without an audio engine; otherwise at most one device read per
        // currentStepTime window.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        xyo::Runtime* runtime = wasmRuntime();
        if (!scratch || !runtime) return -1;
        double level = -1.0;
        if (runtime->audioEngine) {
            const double since = runtime->loudnessTimer.timeElapsed() -
                runtime->cachedLoudnessTimestamp;
            if (since < runtime->currentStepTime) {
                level = runtime->cachedLoudness;
            } else {
                runtime->cachedLoudnessTimestamp =
                    runtime->loudnessTimer.timeElapsed();
                runtime->cachedLoudness = runtime->audioEngine->getLoudness();
                level = runtime->cachedLoudness;
            }
        }
        std::memcpy(scratch, &level, sizeof(level));
        return 0;
    }
    case 16: // string length (f64)
    case 22: { // toBoolean (f64)
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        if (!scratch) return -1;
        uint64_t lhs = 0, result = 0;
        std::memcpy(&lhs, scratch, sizeof(lhs));
        const auto op = id == 16
            ? xyo::ir::HostEffectOp::StringLength
            : xyo::ir::HostEffectOp::ToBoolean;
        if (!xyo::ir::invokeHostEffect(op, lhs, 0, 0, result))
            return -1;
        const xyo::Value value{result};
        const double converted = value.isBool()
            ? double(value.asBool()) : value.asNumberUnchecked();
        std::memcpy(scratch, &converted, sizeof(converted));
        return 0;
    }
    case 26: {
        // Produce Scratch's exact join separator for speculative list data.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::getBoundListContents(
            scratch, static_cast<uint32_t>(b)) ? 0 : -1;
    }
    case 23: {
        // Numeric f64 index -> original tagged Scratch Value.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::loadBoundTaggedListItem(
            scratch, static_cast<uint32_t>(b)) ? 0 : -1;
    }
    case 24:
    case 25: {
        // Typed contains / index of, honoring Scratch list equality.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::queryBoundTaggedList(
            scratch, static_cast<uint32_t>(b),
            static_cast<uint8_t>(id - 23)) ? 0 : -1;
    }
    case 21: {
        // UTF-16 literal from the current immutable code object.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::loadBoundStringLiteral(
            scratch, static_cast<uint32_t>(b)) ? 0 : -1;
    }
    case 18: // join, tagged output
    case 19: // letter of, tagged output
    case 20: // contains, f64 output
    case 17: { // tagged compare, f64 output
        if (id == 17 && (b < 0 || b > 2)) return -1;
        auto* scratch = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        if (!scratch) return -1;
        uint64_t lhs = 0, rhs = 0, result = 0;
        std::memcpy(&lhs, scratch, sizeof(lhs));
        std::memcpy(&rhs, scratch + sizeof(lhs), sizeof(rhs));
        const auto op = static_cast<xyo::ir::HostEffectOp>(id);
        if (!xyo::ir::invokeHostEffect(op, lhs, rhs, b, result))
            return -1;
        if (id == 20 || id == 17) {
            const double converted = double(xyo::Value{result}.asBool());
            std::memcpy(scratch, &converted, sizeof(converted));
        } else {
            std::memcpy(scratch, &result, sizeof(result));
        }
        return 0;
    }

    case 10:
    case 11:
    case 12: {
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        return xyo::wasm::queryBoundNumericList(
            scratch, static_cast<uint32_t>(b),
            static_cast<uint8_t>(id - 10)) ? 0 : -1;
    }
    case 7:
    case 8:
    case 9:
    case 13:
    case 14: {
        // Append / replace / delete. Only the host-owned transaction is
        // mutated; a late WASM bailout simply drops the staging buffers.
        auto* scratch = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(a)));
        using Edit = xyo::wasm::NumericListTransaction::Edit;
        const Edit operation = id == 7 ? Edit::Append :
                               id == 8 ? Edit::Replace :
                               id == 9 ? Edit::Delete :
                               id == 13 ? Edit::DeleteAll : Edit::Insert;
        return xyo::wasm::editBoundNumericList(
            scratch, static_cast<uint32_t>(b), operation) ? 0 : -1;
    }
    default: return -1;
    }
}
#endif
