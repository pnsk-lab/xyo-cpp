// Portable, GC-rooted Scratch Value handle table.
//
// A generated LLVM/WASM continuation may retain the 64-bit handle token
// across yield, but must NEVER retain the raw GC-pointer payload in its
// untraced SSA frame. The runtime owns this table and keeps every live
// Value marked through Gc's exact root-provider API.
//
// This table is thread-confined. Registration, mutation, collection and
// destruction must occur at scheduler safe points, without concurrent GC.
// An active session must outlive every compiled continuation that uses it.
#pragma once

#include "../core/gc.hpp"
#include "../core/value.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace xyo::ir {

struct RootedValueHandle {
    // 0 is invalid. Globally monotonically allocated tokens also reject
    // accidentally resolving stale handles against another arena instance.
    uint64_t token = 0;

    constexpr explicit operator bool() const { return token != 0; }
    constexpr bool operator==(const RootedValueHandle&) const = default;
};

// Owns exact GC roots for boxed Scratch Values. List identities are NOT
// boxed Value payloads; lists must still be rooted through their Target or a
// separate list-binding owner. The table retains strings and all immediates.
class GcValueHandles final {
public:
    static constexpr std::size_t kMaxHandles = 1u << 16;

    explicit GcValueHandles(Gc& gc = Gc::get()) : gc_(gc) {
        gc_.addRootProvider(this, [this](Gc& gc) {
            for (const auto& [token, value] : values_) {
                (void)token;
                gc.markValue(value);
            }
        });
    }

    GcValueHandles(const GcValueHandles&) = delete;
    GcValueHandles& operator=(const GcValueHandles&) = delete;
    GcValueHandles(GcValueHandles&&) = delete;
    GcValueHandles& operator=(GcValueHandles&&) = delete;

    ~GcValueHandles() {
        gc_.removeRootProvider(this);
    }

    // Nullopt means capacity exhaustion or monotonically allocated token
    // exhaustion. No handle is inserted in that case.
    std::optional<RootedValueHandle> retain(Value value) {
        if (values_.size() >= kMaxHandles) return std::nullopt;
        // Saturating issuance: fetch_add would wrap after UINT64_MAX and
        // eventually reissue a token that previously named a GC Value.
        uint64_t token = nextToken_.load(std::memory_order_relaxed);
        while (true) {
            if (!token || token == std::numeric_limits<uint64_t>::max())
                return std::nullopt;
            if (nextToken_.compare_exchange_weak(
                    token, token + 1, std::memory_order_relaxed))
                break;
        }
        values_.emplace(token, value);
        return RootedValueHandle{token};
    }

    std::optional<Value> resolve(RootedValueHandle handle) const {
        if (!handle) return std::nullopt;
        const auto it = values_.find(handle.token);
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    // Replace a slot in place without changing its stable token.
    // A live owner must retain both its previous and new values if a
    // transactional branch needs rollback; in that case, retain a second
    // handle rather than calling update before commit.
    bool update(RootedValueHandle handle, Value value) {
        if (!handle) return false;
        const auto it = values_.find(handle.token);
        if (it == values_.end()) return false;
        it->second = value;
        return true;
    }

    // Releases exactly one rooted Value. A stale/duplicate release is false.
    bool release(RootedValueHandle handle) {
        return handle && values_.erase(handle.token) == 1;
    }

    std::size_t size() const { return values_.size(); }

private:
    Gc& gc_;
    std::unordered_map<uint64_t, Value> values_;
    inline static std::atomic<uint64_t> nextToken_{1};
};

// RAII lease for lexical effect invocations. Long-lived continuations own
// explicit handles in their scheduler-side frames instead of lexical leases.
class ScopedValueHandle final {
public:
    ScopedValueHandle(GcValueHandles& handles, RootedValueHandle handle)
        : handles_(&handles), handle_(handle) {}

    ScopedValueHandle(const ScopedValueHandle&) = delete;
    ScopedValueHandle& operator=(const ScopedValueHandle&) = delete;

    ScopedValueHandle(ScopedValueHandle&& other) noexcept
        : handles_(other.handles_), handle_(other.handle_) {
        other.handles_ = nullptr;
        other.handle_ = {};
    }

    ScopedValueHandle& operator=(ScopedValueHandle&&) = delete;

    ~ScopedValueHandle() { release(); }

    RootedValueHandle get() const { return handle_; }

    RootedValueHandle detach() {
        handles_ = nullptr;
        return std::exchange(handle_, {});
    }

    void release() {
        if (!handles_) return;
        handles_->release(handle_);
        handles_ = nullptr;
        handle_ = {};
    }

private:
    GcValueHandles* handles_ = nullptr;
    RootedValueHandle handle_;
};

} // namespace xyo::ir
