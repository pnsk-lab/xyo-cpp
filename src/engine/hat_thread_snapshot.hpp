// A call-local snapshot of existing Scratch hat threads.
//
// Non-restarting hats must ignore newly spawned threads from the current
// startHats() call, but must detect already running copies. A linear scan
// costs O(existing threads * receivers) on a large fanout. Build the index
// lazily only after repeated lookups, preserving the cheap small-project path.
//
// The index borrows topBlock string views from live Thread objects: callers
// must keep the original Thread objects alive throughout the hat dispatch.
#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "thread.hpp"

namespace xyo {

class HatThreadSnapshot {
public:
    explicit HatThreadSnapshot(const std::vector<Thread*>& threads)
        : threads_(threads), initialSize_(threads.size()) {}

    bool contains(const Target* target, std::string_view topBlock) {
        // Building a hash table for one receiver is usually more expensive
        // than scanning the active Thread array.
        if (initialSize_ < 16 || ++lookups_ < 4)
            return containsLinear(target, topBlock);

        if (!indexed_) {
            existing_.reserve(initialSize_);
            for (size_t i = 0; i < initialSize_; ++i) {
                const Thread* thread = threads_[i];
                if (eligible(*thread)) {
                    existing_.insert({thread->target, thread->topBlock});
                }
            }
            indexed_ = true;
        }
        return existing_.contains({target, topBlock});
    }

private:
    struct Key {
        const Target* target;
        std::string_view block;

        bool operator==(const Key& other) const {
            return target == other.target && block == other.block;
        }
    };

    struct Hash {
        size_t operator()(const Key& key) const {
            const size_t a = std::hash<const Target*>{}(key.target);
            const size_t b = std::hash<std::string_view>{}(key.block);
            return a ^ (b + static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (a << 6) + (a >> 2));
        }
    };

    static bool eligible(const Thread& thread) {
        return !thread.stackClick && thread.status != STATUS_DONE;
    }

    bool containsLinear(const Target* target, std::string_view topBlock) const {
        for (size_t i = 0; i < initialSize_; ++i) {
            const Thread* thread = threads_[i];
            if (thread->target == target && thread->topBlock == topBlock &&
                eligible(*thread)) {
                return true;
            }
        }
        return false;
    }

    const std::vector<Thread*>& threads_;
    size_t initialSize_;
    size_t lookups_ = 0;
    bool indexed_ = false;
    std::unordered_set<Key, Hash> existing_;
};

} // namespace xyo
