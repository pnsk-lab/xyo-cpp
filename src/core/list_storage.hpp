// Alternate non-contiguous Scratch list stores. Value is trivially copyable.
#pragma once
#include <cstddef>
#include <cstdint>
#include "value.hpp"

namespace xyo {

struct ListStore {
    virtual ~ListStore() = default;
    virtual uint32_t size() const = 0;
    virtual Value get(uint32_t index) const = 0;
    virtual void set(uint32_t index, Value value) = 0;
    virtual void insert(uint32_t index, Value value) = 0;
    virtual void erase(uint32_t index) = 0;
    virtual void clear() = 0;
    // Representation transitions should copy in blocks, rather than
    // dispatching one virtual insert/get per element.
    // focusIndex is the pending edit position; backends may use it to
    // preposition their movable gap.
    virtual void assignFlat(const Value* values, uint32_t count,
                            uint32_t focusIndex) {
        (void)focusIndex;
        clear();
        for (uint32_t i = 0; i < count; ++i)
            insert(i, values[i]);
    }
    virtual void copyFlat(Value* destination) const {
        for (uint32_t i = 0; i < size(); ++i)
            destination[i] = get(i);
    }
    virtual size_t allocatedBytes() const = 0;
};

// 4=std::deque, 5=gap buffer, 6=rank-indexed chunked sequence.
// Each store keeps its own size; SList owns the authoritative public len.
ListStore* makeListStore(uint32_t mode, uint32_t initialCapacity);
} // namespace xyo
