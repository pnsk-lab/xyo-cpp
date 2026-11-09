#include "list_storage.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace xyo {
namespace {

class DequeStore final : public ListStore {
    std::deque<Value> values_;
public:
    uint32_t size() const override { return uint32_t(values_.size()); }
    Value get(uint32_t i) const override { return values_[i]; }
    void set(uint32_t i, Value v) override { values_[i] = v; }
    void insert(uint32_t i, Value v) override { values_.insert(values_.begin() + i, v); }
    void erase(uint32_t i) override { values_.erase(values_.begin() + i); }
    void clear() override { values_.clear(); }
    void assignFlat(const Value* source, uint32_t count, uint32_t) override {
        values_.assign(source, source + count);
    }
    void copyFlat(Value* destination) const override {
        std::copy(values_.begin(), values_.end(), destination);
    }
    size_t allocatedBytes() const override {
        return values_.size() * sizeof(Value) + (values_.size() / 64 + 1) * 16;
    }
};

// Move a contiguous "hole" to the most recently edited position.
class GapStore final : public ListStore {
    // Value is trivially copyable. A gap's spare slots are never read,
    // so avoid value-initializing them on allocation.
    uint32_t capacity_ = 0;
    std::unique_ptr<Value[]> values_;
    uint32_t gapStart_ = 0;
    uint32_t gapEnd_ = 0;

    uint32_t gapSize() const { return gapEnd_ - gapStart_; }
    void expand() {
        const uint32_t oldCapacity = uint32_t(capacity_);
        if (oldCapacity == UINT32_MAX) std::abort();
        const uint32_t next = uint32_t(std::min<uint64_t>(
            UINT32_MAX, std::max<uint64_t>(8, uint64_t(oldCapacity) * 2)));
        auto grown = std::make_unique_for_overwrite<Value[]>(next);
        const uint32_t suffix = oldCapacity - gapEnd_;
        if (gapStart_)
            std::memcpy(grown.get(), values_.get(),
                        size_t(gapStart_) * sizeof(Value));
        if (suffix)
            std::memcpy(grown.get() + next - suffix, values_.get() + gapEnd_,
                        size_t(suffix) * sizeof(Value));
        values_.swap(grown);
        capacity_ = next;
        gapEnd_ = next - suffix;
    }
    void moveGap(uint32_t pos) {
        if (pos < gapStart_) {
            const uint32_t delta = gapStart_ - pos;
            std::memmove(values_.get() + gapEnd_ - delta,
                         values_.get() + pos, size_t(delta) * sizeof(Value));
            gapStart_ -= delta;
            gapEnd_ -= delta;
        } else if (pos > gapStart_) {
            const uint32_t delta = pos - gapStart_;
            std::memmove(values_.get() + gapStart_,
                         values_.get() + gapEnd_, size_t(delta) * sizeof(Value));
            gapStart_ += delta;
            gapEnd_ += delta;
        }
    }
public:
    explicit GapStore(uint32_t capacity)
        : capacity_(std::max(4u, capacity)),
          values_(std::make_unique_for_overwrite<Value[]>(capacity_)),
          gapEnd_(capacity_) {}

    uint32_t size() const override { return uint32_t(capacity_) - gapSize(); }
    Value get(uint32_t i) const override {
        return values_[i < gapStart_ ? i : i + gapSize()];
    }
    void set(uint32_t i, Value v) override {
        values_[i < gapStart_ ? i : i + gapSize()] = v;
    }
    void insert(uint32_t i, Value v) override {
        if (!gapSize()) expand();
        moveGap(i);
        values_[gapStart_++] = v;
    }
    void erase(uint32_t i) override {
        moveGap(i);
        ++gapEnd_;
    }
    void clear() override {
        gapStart_ = 0;
        gapEnd_ = uint32_t(capacity_);
    }
    void assignFlat(const Value* source, uint32_t count,
                    uint32_t focusIndex) override {
        // Reserve a gap on the *next* edit location. Migration no longer
        // has to fill the entire vector then immediately reallocate it.
        const uint64_t spare = std::max<uint64_t>(8, count / 4);
        const uint64_t capacity = std::max<uint64_t>(
            4, std::min<uint64_t>(UINT32_MAX, uint64_t(count) + spare));
        if (capacity_ != capacity) {
            values_ = std::make_unique_for_overwrite<Value[]>(size_t(capacity));
            capacity_ = uint32_t(capacity);
        }
        gapStart_ = std::min(focusIndex, count);
        gapEnd_ = uint32_t(capacity) - (count - gapStart_);
        if (gapStart_)
            std::memcpy(values_.get(), source,
                        size_t(gapStart_) * sizeof(Value));
        const uint32_t suffix = count - gapStart_;
        if (suffix)
            std::memcpy(values_.get() + gapEnd_, source + gapStart_,
                        size_t(suffix) * sizeof(Value));
    }
    void copyFlat(Value* destination) const override {
        if (gapStart_)
            std::memcpy(destination, values_.get(),
                        size_t(gapStart_) * sizeof(Value));
        const uint32_t suffix = uint32_t(capacity_) - gapEnd_;
        if (suffix)
            std::memcpy(destination + gapStart_, values_.get() + gapEnd_,
                        size_t(suffix) * sizeof(Value));
    }
    size_t allocatedBytes() const override {
        return size_t(capacity_) * sizeof(Value);
    }
};

// Rank-indexed chunks: binary-search prefix totals, then edit one local leaf.
// Each leaf holds up to 128 consecutive Values; middle edits avoid global shifts.
// Directory updates are O(number of chunks), i.e. O(n / 128), not O(n).
class ChunkStore final : public ListStore {
    static constexpr uint32_t kBlockSize = 128;
    struct Block {
        uint32_t len = 0;
        // Only [0, len) is logically initialized and ever inspected.
        std::array<Value, kBlockSize> values;
    };
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<uint32_t> prefix_;
    uint32_t size_ = 0;

    struct Position { size_t block; uint32_t offset; };
    Position locate(uint32_t i) const {
        auto it = std::lower_bound(prefix_.begin(), prefix_.end(), i + 1);
        const size_t b = size_t(it - prefix_.begin());
        assert(b < blocks_.size());
        return {b, i - (b ? prefix_[b - 1] : 0)};
    }
    void reindex(size_t b) {
        uint32_t count = b ? prefix_[b - 1] : 0;
        prefix_.resize(blocks_.size());
        for (; b < blocks_.size(); ++b) {
            count += blocks_[b]->len;
            prefix_[b] = count;
        }
        assert(prefix_.empty() || prefix_.back() == size_);
    }
    void insertBlock(size_t index) {
        blocks_.insert(blocks_.begin() + index, std::make_unique_for_overwrite<Block>());
        prefix_.insert(prefix_.begin() + index, 0);
    }
public:
    uint32_t size() const override { return size_; }
    Value get(uint32_t i) const override {
        const auto [b, off] = locate(i);
        return blocks_[b]->values[off];
    }
    void set(uint32_t i, Value v) override {
        const auto [b, off] = locate(i);
        blocks_[b]->values[off] = v;
    }
    void insert(uint32_t i, Value v) override {
        assert(i <= size_);
        if (i == size_) {
            if (blocks_.empty() || blocks_.back()->len == kBlockSize)
                insertBlock(blocks_.size());
            Block& last = *blocks_.back();
            last.values[last.len++] = v;
            ++size_;
            prefix_.back() = size_;
            return;
        }
        Position pos = locate(i);
        Block* leaf = blocks_[pos.block].get();
        if (leaf->len == kBlockSize) {
            const size_t b = pos.block;
            insertBlock(b + 1);
            // vector insertion moves only unique_ptrs; leaf remains stable.
            Block& right = *blocks_[b + 1];
            const uint32_t middle = kBlockSize / 2;
            right.len = kBlockSize - middle;
            std::memcpy(right.values.data(), leaf->values.data() + middle,
                        right.len * sizeof(Value));
            leaf->len = middle;
            if (pos.offset >= middle) {
                pos = {b + 1, pos.offset - middle};
                leaf = &right;
            }
        }
        std::memmove(leaf->values.data() + pos.offset + 1,
                     leaf->values.data() + pos.offset,
                     (leaf->len - pos.offset) * sizeof(Value));
        leaf->values[pos.offset] = v;
        ++leaf->len;
        ++size_;
        reindex(pos.block ? pos.block - 1 : 0);
    }
    void erase(uint32_t i) override {
        const auto [b, off] = locate(i);
        Block& leaf = *blocks_[b];
        std::memmove(leaf.values.data() + off,
                     leaf.values.data() + off + 1,
                     (leaf.len - off - 1) * sizeof(Value));
        --leaf.len;
        --size_;
        if (!leaf.len) {
            blocks_.erase(blocks_.begin() + b);
            prefix_.erase(prefix_.begin() + b);
        }
        reindex(b ? b - 1 : 0);
    }
    void clear() override {
        blocks_.clear();
        prefix_.clear();
        size_ = 0;
    }
    void assignFlat(const Value* source, uint32_t count, uint32_t) override {
        clear();
        const size_t totalBlocks = (size_t(count) + kBlockSize - 1) / kBlockSize;
        blocks_.reserve(totalBlocks);
        prefix_.reserve(totalBlocks);
        for (uint32_t offset = 0; offset < count;) {
            const uint32_t take = std::min(kBlockSize, count - offset);
            auto block = std::make_unique_for_overwrite<Block>();
            block->len = take;
            std::memcpy(block->values.data(), source + offset,
                        size_t(take) * sizeof(Value));
            blocks_.push_back(std::move(block));
            offset += take;
            prefix_.push_back(offset);
        }
        size_ = count;
    }
    void copyFlat(Value* destination) const override {
        uint32_t offset = 0;
        for (const auto& block : blocks_) {
            if (block->len)
                std::memcpy(destination + offset, block->values.data(),
                            size_t(block->len) * sizeof(Value));
            offset += block->len;
        }
        assert(offset == size_);
    }
    size_t allocatedBytes() const override {
        return blocks_.size() * sizeof(Block) +
               blocks_.capacity() * sizeof(std::unique_ptr<Block>) +
               prefix_.capacity() * sizeof(uint32_t);
    }
};
} // namespace

ListStore* makeListStore(uint32_t mode, uint32_t initialCapacity) {
    switch (mode) {
    case 4: return new DequeStore();
    case 5: return new GapStore(initialCapacity);
    case 6: return new ChunkStore();
    default: return nullptr;
    }
}
} // namespace xyo
