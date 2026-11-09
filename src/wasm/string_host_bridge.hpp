// Invocation-scoped immutable UTF-16 literals for the WASM string bridge.
// The compiler stores owned UTF-16 code units, never GC-managed source pointers.
// All invocation allocations are deferred until the staged Value writeback
// commits (or the speculative segment bails out).
#pragma once

#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xyo::wasm {

struct StringLiteralBindings {
    const std::vector<std::u16string>* literals = nullptr;
    std::vector<Value> cached;
    std::vector<uint8_t> valid;
};

inline thread_local StringLiteralBindings* activeStringLiterals = nullptr;

struct ScopedStringLiteralBindings {
    StringLiteralBindings current;
    StringLiteralBindings* previous = nullptr;

    explicit ScopedStringLiteralBindings(
        const std::vector<std::u16string>& literals)
        : previous(activeStringLiterals) {
        current.literals = &literals;
        current.cached.resize(literals.size(), Value::emptyStr());
        current.valid.resize(literals.size(), 0);
        activeStringLiterals = &current;
    }
    ~ScopedStringLiteralBindings() {
        activeStringLiterals = previous;
    }
    ScopedStringLiteralBindings(const ScopedStringLiteralBindings&) = delete;
    ScopedStringLiteralBindings& operator=(const ScopedStringLiteralBindings&) = delete;
};

inline bool loadBoundStringLiteral(void* scratch, uint32_t index) {
    auto* bindings = activeStringLiterals;
    if (!scratch || !bindings || !bindings->literals ||
        index >= bindings->literals->size()) return false;
    if (!bindings->valid[index]) {
        const auto& text = (*bindings->literals)[index];
        bindings->cached[index] = text.empty()
            ? Value::emptyStr()
            : Value::string(SStr::create(text));
        bindings->valid[index] = 1;
    }
    const uint64_t bits = bindings->cached[index].bits;
    std::memcpy(scratch, &bits, sizeof(bits));
    return true;
}

} // namespace xyo::wasm
