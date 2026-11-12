// Structural generation tracking for Scratch variable bindings.
//
// Unlike a value update, an insertion/erase/replacement can invalidate a
// cached Variable* or change whether a local ID shadows a stage ID.
// This map is intentionally API-compatible with the subset of unordered_map
// used by loaders, the editor, clones and the public Target::variables API.
// All structural mutation entrypoints bump a *globally unique* generation,
// so even a destroyed and recreated Target cannot produce an ABA match.
//
// The map must not be mutated through a downcast to Base. No concurrent
// mutation is permitted; the existing Scratch runtime is single-threaded
// across scheduler slices.
#pragma once

#include "variable.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace xyo {

class VariableMap : public std::unordered_map<std::string, Variable> {
    using Base = std::unordered_map<std::string, Variable>;
public:
    using Base::Base;
    using typename Base::const_iterator;
    using typename Base::iterator;
    using typename Base::key_type;
    using typename Base::size_type;
    using typename Base::mapped_type;

    VariableMap() = default;
    VariableMap(const VariableMap& rhs)
        : Base(static_cast<const Base&>(rhs)) {}
    VariableMap(VariableMap&& rhs) noexcept
        : Base(std::move(static_cast<Base&>(rhs))) {
        rhs.touch();
    }
    VariableMap(const Base& rhs) : Base(rhs) {}
    VariableMap(Base&& rhs) : Base(std::move(rhs)) {}

    VariableMap& operator=(const VariableMap& rhs) {
        if (this != &rhs) {
            Base::operator=(static_cast<const Base&>(rhs));
            touch();
        }
        return *this;
    }
    VariableMap& operator=(VariableMap&& rhs) noexcept {
        if (this != &rhs) {
            Base::operator=(std::move(static_cast<Base&>(rhs)));
            touch();
            rhs.touch();
        }
        return *this;
    }
    VariableMap& operator=(const Base& rhs) {
        Base::operator=(rhs);
        touch();
        return *this;
    }
    VariableMap& operator=(Base&& rhs) {
        Base::operator=(std::move(rhs));
        touch();
        return *this;
    }
    VariableMap& operator=(std::initializer_list<Base::value_type> rhs) {
        Base::operator=(rhs);
        touch();
        return *this;
    }

    uint64_t generation() const noexcept { return generation_; }
    // Serial-only native LLVM guard reads this stable address as a
    // *volatile* i64 on every access; it never owns the VariableMap.
    // Releasing/replacing a Target invalidates the address. The JIT
    // must verify cached owner identity before dereferencing it.
    const uint64_t* generationAddress() const noexcept {
        return &generation_;
    }

    template <class... Args>
    auto emplace(Args&&... args) {
        auto inserted = Base::emplace(std::forward<Args>(args)...);
        if (inserted.second) touch();
        return inserted;
    }

    template <class... Args>
    iterator emplace_hint(const_iterator hint, Args&&... args) {
        // Conservative invalidation: emplace_hint doesn't report insertion.
        auto inserted = Base::emplace_hint(hint, std::forward<Args>(args)...);
        touch();
        return inserted;
    }

    template <class... Args>
    auto try_emplace(const key_type& key, Args&&... args) {
        auto inserted = Base::try_emplace(
            key, std::forward<Args>(args)...);
        if (inserted.second) touch();
        return inserted;
    }
    template <class... Args>
    auto try_emplace(key_type&& key, Args&&... args) {
        auto inserted = Base::try_emplace(
            std::move(key), std::forward<Args>(args)...);
        if (inserted.second) touch();
        return inserted;
    }
    template <class... Args>
    iterator try_emplace(const_iterator hint, const key_type& key,
                         Args&&... args) {
        auto it = Base::try_emplace(hint, key,
                                    std::forward<Args>(args)...);
        touch(); // possible insertion; conservative on existing key
        return it;
    }
    template <class... Args>
    iterator try_emplace(const_iterator hint, key_type&& key,
                         Args&&... args) {
        auto it = Base::try_emplace(hint, std::move(key),
                                    std::forward<Args>(args)...);
        touch();
        return it;
    }

    template <class Key, class ValueArg>
    auto insert_or_assign(Key&& key, ValueArg&& value) {
        auto result = Base::insert_or_assign(
            std::forward<Key>(key), std::forward<ValueArg>(value));
        touch();
        return result;
    }
    template <class Key, class ValueArg>
    iterator insert_or_assign(const_iterator hint, Key&& key,
                              ValueArg&& value) {
        auto result = Base::insert_or_assign(
            hint, std::forward<Key>(key),
            std::forward<ValueArg>(value));
        touch();
        return result;
    }

    template <class Pair>
    auto insert(Pair&& value) {
        auto result = Base::insert(std::forward<Pair>(value));
        if (result.second) touch();
        return result;
    }
    iterator insert(const_iterator hint, const Base::value_type& value) {
        auto it = Base::insert(hint, value);
        touch();
        return it;
    }
    iterator insert(const_iterator hint, Base::value_type&& value) {
        auto it = Base::insert(hint, std::move(value));
        touch();
        return it;
    }
    template <class InputIt>
    void insert(InputIt first, InputIt last) {
        if (first == last) return;
        Base::insert(first, last);
        touch();
    }
    void insert(std::initializer_list<Base::value_type> values) {
        if (values.size() == 0) return;
        Base::insert(values);
        touch();
    }

    mapped_type& operator[](const key_type& key) {
        touch(); // operator[] may insert or replace an existing object
        return Base::operator[](key);
    }
    mapped_type& operator[](key_type&& key) {
        touch();
        return Base::operator[](std::move(key));
    }

    size_type erase(const key_type& key) {
        const size_type n = Base::erase(key);
        if (n) touch();
        return n;
    }
    iterator erase(const_iterator it) {
        auto next = Base::erase(it);
        touch();
        return next;
    }
    iterator erase(const_iterator first, const_iterator last) {
        const bool changed = first != last;
        auto next = Base::erase(first, last);
        if (changed) touch();
        return next;
    }
    void clear() noexcept {
        if (!Base::empty()) {
            Base::clear();
            touch();
        }
    }
    void swap(VariableMap& rhs) noexcept {
        if (this == &rhs) return;
        Base::swap(static_cast<Base&>(rhs));
        touch();
        rhs.touch();
    }

    // Prevent mutation through uncovered APIs without generation updates.
    template <class... Args>
    void extract(Args&&...) = delete;
    template <class... Args>
    void merge(Args&&...) = delete;

private:
    void touch() noexcept {
        generation_ = nextGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
    inline static std::atomic<uint64_t> nextGeneration_{2};
    uint64_t generation_ = nextGeneration_.fetch_add(
        1, std::memory_order_relaxed);
};

} // namespace xyo
