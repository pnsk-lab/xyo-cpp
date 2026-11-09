// xyo-cpp: SList — GC-managed Scratch list.
// Flat variants keep the existing contiguous ABI. Alternate representations
// must be accessed via get0/write0 or guarded JIT helper calls.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "list_storage.hpp"
#include "gc.hpp"
#include "value.hpp"

namespace xyo {

/// 0 = unlimited. See --list-limit / XYO_LIST_LIMIT.
extern "C" uint32_t xyo_list_limit;

/// XYO_LIST_STORAGE: flat (default), sbo4, sbo8, flat15, deque, gap, chunked, adaptive.
/// Read on startup. Also writable in focused tests before allocating lists.
/// Existing SList ABI offsets (len/cap/flags/items) must remain unchanged.
extern "C" uint32_t xyo_list_storage_mode;

struct SList {
    uint32_t len;
    uint32_t cap;
    bool monitorDirty;
    // Monotonic negative feedback; once false, remains false.
    bool numericOnly;
    Value* items;
    ListStore* extra; // nullptr while JIT can directly access items
    uint32_t adaptiveEdits;
    uint32_t adaptiveLocalEdits;
    uint32_t adaptiveLastIndex;
    uint32_t adaptiveReads;
    uint32_t adaptiveBackend; // 0=flat, 5=gap, 6=chunked (mode 7 only)
    // Increment whenever items or the physical representation can change.
    // LLVM's loop-local list pointer cache validates this before dereference.
    uint64_t storageEpoch;

    // A flexible tail is allocated by GC only for a list whose initial
    // capacity fits the chosen SBO size. No extra fields or allocation are
    // added to ordinary flat lists.
    Value* inlineItems() {
        return reinterpret_cast<Value*>(this + 1);
    }
    const Value* inlineItems() const {
        return reinterpret_cast<const Value*>(this + 1);
    }
    bool usesInline() const { return !extra && items == inlineItems(); }
    bool isContiguous() const { return !extra; }
    // Restore contiguous storage when the operation mix becomes read-heavy.
    // Called only on the helper path, never during an inlined JIT load.
    void noteAdaptiveRead(uint32_t weight = 1) {
        if (xyo_list_storage_mode != 7 || !extra) return;
        adaptiveReads += weight;
        if (adaptiveReads >= 64) makeContiguous();
    }
    void makeContiguous() {
        if (!extra) return;
        const uint32_t nextCap = std::max(4u, len);
        Value* flat = static_cast<Value*>(std::malloc(size_t(nextCap) * sizeof(Value)));
        if (!flat) std::abort();
        extra->copyFlat(flat);
        delete extra;
        extra = nullptr;
        items = flat;
        cap = nextCap;
        ++storageEpoch;
        resetAdaptiveFeedback();
    }
    void resetAdaptiveFeedback() {
        adaptiveEdits = adaptiveLocalEdits = adaptiveLastIndex = 0;
        adaptiveReads = adaptiveBackend = 0;
    }
    void noteAdaptiveEdit(uint32_t index) {
        if (xyo_list_storage_mode != 7) return;
        if (extra) { adaptiveReads = 0; return; }
        if (len < 256 || index < 32 || len - index <= 32) return;
        if (adaptiveEdits &&
            (index >= adaptiveLastIndex
                 ? index - adaptiveLastIndex <= 32
                 : adaptiveLastIndex - index <= 32))
            ++adaptiveLocalEdits;
        adaptiveLastIndex = index;
        if (++adaptiveEdits < 24) return;
        const uint32_t selected = adaptiveLocalEdits >= 16 ? 5 : 6;
        ListStore* backend = makeListStore(selected, len);
        if (!backend) std::abort();
        backend->assignFlat(items, len, index);
        std::free(items); // Adaptive starts on malloc-backed flat storage.
        items = nullptr;
        extra = backend;
        cap = 0;
        ++storageEpoch;
        adaptiveBackend = selected;
        adaptiveReads = 0;
    }
    Value get0(uint32_t i) const { return extra ? extra->get(i) : items[i]; }
    void write0(uint32_t i, Value v) {
        if (extra) extra->set(i, v);
        else items[i] = v;
    }
    bool equalsBits(const uint64_t* raw, uint32_t count) const {
        if (count != len) return false;
        if (!extra) return !std::memcmp(items, raw, size_t(count) * sizeof(Value));
        for (uint32_t i = 0; i < count; ++i)
            if (extra->get(i).bits != raw[i]) return false;
        return true;
    }
    size_t externalBytes() const {
        if (extra) return extra->allocatedBytes();
        return usesInline() ? 0 : size_t(cap) * sizeof(Value);
    }
    void destroyStorage() {
        if (extra) { delete extra; extra = nullptr; }
        else if (!usesInline()) std::free(items);
    }

    static SList* create(uint32_t initialCap = 4) {
        const bool alternative = xyo_list_storage_mode >= 4 &&
                                 xyo_list_storage_mode <= 6;
        const uint32_t inlineCap = xyo_list_storage_mode == 1 ? 4u :
                                   xyo_list_storage_mode == 2 ? 8u : 0u;
        const bool inlineStorage = inlineCap && initialCap <= inlineCap;
        const uint32_t capacity = inlineStorage ? inlineCap : initialCap;
        auto* l = static_cast<SList*>(Gc::get().alloc(
            GcType::List, uint32_t(sizeof(SList) +
                (inlineStorage ? size_t(inlineCap) * sizeof(Value) : 0))));
        l->len = 0;
        l->cap = capacity;
        l->monitorDirty = false;
        l->numericOnly = true;
        l->storageEpoch = 1;
        l->resetAdaptiveFeedback();
        l->extra = alternative
            ? makeListStore(xyo_list_storage_mode, initialCap) : nullptr;
        l->items = alternative ? nullptr : inlineStorage ? l->inlineItems() :
            static_cast<Value*>(std::malloc(std::max(1u, capacity) * sizeof(Value)));
        if (!alternative && !l->items) std::abort();
        return l;
    }

    void ensure(uint32_t need) {
        if (extra) { cap = std::max(cap, need); return; }
        if (need <= cap) return;
        uint32_t next;
        if (xyo_list_storage_mode == 3) {
            // 1.5x heap capacity: compare lower memory usage vs resize cost.
            const uint64_t growth = uint64_t(cap) + std::max(1u, cap / 2);
            next = uint32_t(std::min<uint64_t>(
                std::max<uint64_t>(growth, need), UINT32_MAX));
        } else {
            const uint64_t growth = uint64_t(cap) * 2;
            next = uint32_t(std::min<uint64_t>(
                std::max<uint64_t>(growth, need), UINT32_MAX));
        }
        Value* grown;
        if (usesInline()) {
            grown = static_cast<Value*>(std::malloc(size_t(next) * sizeof(Value)));
            if (!grown) std::abort();
            std::memcpy(grown, items, size_t(len) * sizeof(Value));
        } else {
            grown = static_cast<Value*>(std::realloc(
                items, size_t(next) * sizeof(Value)));
            if (!grown) std::abort();  // Preserve original allocation on OOM.
        }
        items = grown;
        cap = next;
        ++storageEpoch;
    }

    void push(Value v) {
        if (xyo_list_limit && len >= xyo_list_limit) return;
        ensure(len + 1);
        numericOnly &= v.isNumber();
        if (extra) extra->insert(len++, v);
        else items[len++] = v;
    }
    void insert0(uint32_t i, Value v) {
        if (i > len) return;
        noteAdaptiveEdit(i);
        ensure(len + 1);
        if (!extra)
            std::memmove(items + i + 1, items + i, (len - i) * sizeof(Value));
        numericOnly &= v.isNumber();
        if (extra) extra->insert(i, v);
        else items[i] = v;
        ++len;
        if (xyo_list_limit && len > xyo_list_limit) {
            --len;
            if (extra) extra->erase(len);
        }
    }
    void replace0(uint32_t i, Value v) {
        if (i >= len) return;
        noteAdaptiveRead();
        numericOnly &= v.isNumber();
        write0(i, v);
    }
    void erase0(uint32_t i) {
        if (i >= len) return;
        noteAdaptiveEdit(i);
        if (extra) extra->erase(i);
        else std::memmove(items + i, items + i + 1, (len - i - 1) * sizeof(Value));
        --len;
    }
    void clear() {
        if (xyo_list_storage_mode == 7 && extra) {
            delete extra;
            extra = nullptr;
            items = static_cast<Value*>(std::malloc(4 * sizeof(Value)));
            if (!items) std::abort();
            cap = 4;
            ++storageEpoch;
        } else if (extra) {
            extra->clear();
        }
        len = 0;
        if (xyo_list_storage_mode == 7) resetAdaptiveFeedback();
    }
    SList* clone() const {
        // Adaptive non-flat storage has cap=0; allocate a sufficiently
        // large flat destination and materialize in a single linear copy.
        const bool adaptiveCopy = extra && xyo_list_storage_mode == 7;
        SList* c = create(adaptiveCopy ? std::max(4u, len) : cap);
        if (adaptiveCopy) {
            c->len = len;
            extra->copyFlat(c->items);
        } else if (extra) {
            for (uint32_t i = 0; i < len; ++i) c->push(get0(i));
        } else {
            c->len = len;
            std::memcpy(c->items, items, len * sizeof(Value));
        }
        c->monitorDirty = monitorDirty;
        c->numericOnly = numericOnly;
        return c;
    }
};

} // namespace xyo
