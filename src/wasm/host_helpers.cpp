// Shared host math operations imported by generated WASM HotSpot code.
// Operand buffers live in the Emscripten linear memory.
#include "../rt/runtime_helpers.hpp"
#include "../ir/host_effect_abi.hpp"
#include "string_number_bridge.hpp"
#include "list_host_bridge.hpp"
#include "string_host_bridge.hpp"
#include <cstdint>
#include <cstring>

#if defined(__EMSCRIPTEN__)
namespace xyo { struct JitCtx; }
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
