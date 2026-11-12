// Host-owned tagged Scratch list bridge for transactional WASM execution.
//
// Do not pass SList* or Value* into a generated module: those pointers are
// neither safe GC roots nor stable across resize/collection. This temporary
// invocation table holds Variable* borrowed from a live Target and is only
// active while its synchronous segment executes on the owning thread.
#pragma once

#include "../ir/list_access.hpp"
#include "../model/variable.hpp"
#include "../rt/runtime_helpers.hpp"
#include "../core/cast.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <utility>
#include <unordered_map>
#include <vector>

namespace xyo::wasm {

// The segment's speculative Scratch lists are copied on first structural mutation.
// No SList write is visible until commit. Reads use the staged copy, so a
// write/read pair in one WASM segment has normal Scratch order.
struct NumericListTransaction {
    struct Stage {
        Variable* owner = nullptr;
        // Only a delete needs a full materialized vector (index shifting).
        // Append/replace stay in a sparse journal until commit.
        std::vector<Value> items;
        std::vector<Value> appended;
        std::unordered_map<uint32_t, Value> replacements;
        bool modified = false;
        bool everNonNumeric = false;
        bool materialized = false;

        uint32_t length() const {
            return materialized ? uint32_t(items.size()) :
                uint32_t(owner->list->len + appended.size());
        }
    };
    std::vector<Stage> stages;
    static constexpr size_t kMaxItems = 65536;

    explicit NumericListTransaction(const std::vector<Variable*>& variables) {
        stages.reserve(variables.size());
        for (Variable* v : variables) stages.push_back({v});
    }
    bool validList(uint32_t index) const {
        if (index >= stages.size()) return false;
        const Variable* v = stages[index].owner;
        return v && v->type == Variable::kListType && v->list;
    }
    bool valid(uint32_t index) const {
        return validList(index) &&
            stages[index].owner->list->len <= kMaxItems;
    }
    // Length is safe for mixed-type lists, even if a numeric specialization
    // cannot read the values. It always sees this segment's speculative edits.
    bool query(uint32_t binding, double needle, uint8_t op,
               double& result) const {
        if (!validList(binding)) return false;
        const Stage& s = stages[binding];
        if (op == 0) {
            result = static_cast<double>(s.length());
            return true;
        }
        // Keep the existing numeric-only search fast path; typed/mixed
        // searches below use Scratch's full equality semantics.
        if (!valid(binding) || !s.owner->list->numericOnly ||
            std::isnan(needle)) return false;
        for (uint32_t index = 0; index < s.length(); ++index) {
            uint64_t bits = 0;
            if (!read(binding, double(index) + 1, bits)) return false;
            const double value = Value{bits}.asNumberUnchecked();
            if (value == needle) {
                result = op == 1 ? 1.0 : double(index + 1);
                return true;
            }
        }
        result = 0.0;
        return true;
    }
    // Interpret a dynamic Scratch index without touching the global RNG:
    // "random"/"any" are handed back to the interpreter before consuming
    // entropy, so a subsequent deopt cannot double-consume a random draw.
    static bool randomIndex(Value index) {
        if (!index.isString() || !index.asStrPtr()) return false;
        const auto text = index.asStrPtr()->view();
        return text == u"random" || text == u"any";
    }
    bool readIndexed(uint32_t binding, Value index, Value& result) const {
        if (!valid(binding) || randomIndex(index)) return false;
        const Stage& s = stages[binding];
        const int32_t zeroBased = cast::listIndex(index, s.length());
        if (zeroBased < 0) return false;
        return readValue(binding, double(zeroBased) + 1, result);
    }

    // Read the current raw Value, including uncommitted journal writes.
    // The authoritative SList and every original item remain rooted through
    // the owning Target. Generated strings are protected by DeferCollect.
    bool readValue(uint32_t binding, double index, Value& result) const {
        if (!valid(binding)) return false;
        const Stage& s = stages[binding];
        uint32_t offset = 0;
        if (!ir::numericListIndex(index, s.length(), offset)) return false;
        if (s.materialized) {
            result = s.items[offset];
        } else if (offset >= s.owner->list->len) {
            result = s.appended[offset - s.owner->list->len];
        } else {
            const auto it = s.replacements.find(offset);
            result = it == s.replacements.end()
                ? s.owner->list->get0(offset) : it->second;
        }
        return true;
    }

    // The interpreter's two list reporters have intentionally different
    // comparisons: contains uses SameValueZero OR Scratch coercive equality;
    // indexOf uses Scratch equality only. Do not rewrite as IEEE ==.
    bool queryValue(uint32_t binding, Value needle, uint8_t op,
                    double& result) const {
        if (!valid(binding) || (op != 1 && op != 2)) return false;
        const Stage& s = stages[binding];
        const bool needleNumeric = needle.isNumber();
        const double needleNumber = needleNumeric
            ? needle.asNumberUnchecked() : 0.0;
        for (uint32_t index = 0; index < s.length(); ++index) {
            Value entry;
            if (!readValue(binding, double(index) + 1, entry))
                return false;
            const bool equal = rt::compareEqual(entry, needle);
            bool sameValueZero = false;
            if (op == 1) {
                if (entry.isNumber() && needleNumeric) {
                    const double value = entry.asNumberUnchecked();
                    sameValueZero = value == needleNumber ||
                        (std::isnan(value) && std::isnan(needleNumber));
                } else if (entry.isString() && needle.isString()) {
                    const auto* a = entry.asStrPtr();
                    const auto* b = needle.asStrPtr();
                    sameValueZero = a && b
                        ? a->view() == b->view() : a == b;
                } else {
                    sameValueZero = entry.bits == needle.bits;
                }
            }
            if (equal || sameValueZero) {
                result = op == 1 ? 1.0 : double(index + 1);
                return true;
            }
        }
        result = 0.0;
        return true;
    }

    bool materialize(uint32_t binding) {
        if (!valid(binding)) return false;
        Stage& s = stages[binding];
        if (s.materialized) return true;
        const SList* list = s.owner->list;
        // Copy only for deletion, which shifts the logical indices.
        s.items.clear();
        s.items.reserve(list->len);
        if (list->isContiguous())
            s.items.assign(list->items, list->items + list->len);
        else
            for (uint32_t i = 0; i < list->len; ++i)
                s.items.push_back(list->get0(i));
        for (const auto& [index, value] : s.replacements)
            s.items[index] = value;
        s.items.insert(s.items.end(), s.appended.begin(), s.appended.end());
        s.appended.clear();
        s.replacements.clear();
        s.materialized = true;
        return true;
    }
    bool read(uint32_t binding, double index, uint64_t& result) const {
        Value value;
        if (!readValue(binding, index, value) || !value.isNumber())
            return false;
        result = value.bits;
        return true;
    }
    enum class Edit : uint8_t { Append, Replace, Delete, DeleteAll, Insert };
    bool write(Edit op, uint32_t binding, double index, uint64_t raw) {
        return writeTyped(op, binding, Value::num(index), raw);
    }
    bool writeTyped(Edit op, uint32_t binding, Value index, uint64_t raw) {
        if (op == Edit::DeleteAll) {
            // Clearing is safe even for a mixed-type list: no values are
            // dereferenced or stored and a bailout leaves the owner intact.
            if (!validList(binding)) return false;
            Stage& s = stages[binding];
            s.items.clear();
            s.appended.clear();
            s.replacements.clear();
            s.materialized = true;
            s.modified = true;
            return true;
        }
        if (!valid(binding)) return false;
        const Value value{raw};
        // Store exact Scratch Values, including strings, booleans and colors.
        // Any speculative string allocation remains rooted on commit because
        // collection is deferred until its list's Target becomes authoritative.
        Stage& s = stages[binding];
        if (op != Edit::Delete && !value.isNumber())
            s.everNonNumeric = true;
        uint32_t offset = 0;
        if (op != Edit::Append) {
            // Preserve exact cast::listIndex and toListIndexD behavior for
            // "last", numeric strings, and decimal fractional indices.
            // No random index is safe inside a speculative segment because
            // rolling back list state cannot roll back global RNG state.
            if (randomIndex(index)) return false;
            if (op == Edit::Delete) {
                const double parsed = cast::toListIndexD(
                    index, s.length(), true);
                if (parsed == double(cast::LIST_ALL))
                    return writeTyped(Edit::DeleteAll, binding,
                                      Value::num(0), 0);
                if (!std::isfinite(parsed) || parsed < 1 ||
                    parsed > s.length()) return false;
                offset = uint32_t(parsed) - 1;
            } else {
                const int32_t parsed = cast::listIndex(index,
                    s.length() + uint32_t(op == Edit::Insert));
                if (parsed < 0) return false;
                offset = static_cast<uint32_t>(parsed);
            }
        }
        if (op == Edit::Append || op == Edit::Insert) {
            if (xyo_list_limit && s.length() >= xyo_list_limit) return true;
            if (s.length() >= kMaxItems) return false;
        }
        if (op == Edit::Append) {
            if (s.materialized) s.items.push_back(value);
            else s.appended.push_back(value);
        } else if (op == Edit::Replace) {
            if (s.materialized) s.items[offset] = value;
            else if (offset >= s.owner->list->len)
                s.appended[offset - s.owner->list->len] = value;
            else
                s.replacements.insert_or_assign(offset, value);
        } else if (op == Edit::Insert) {
            if (!materialize(binding)) return false;
            s.items.insert(s.items.begin() + offset, value);
        } else {
            if (!materialize(binding)) return false;
            s.items.erase(s.items.begin() + offset);
        }
        s.modified = true;
        return true;
    }
    void commit() {
        // Reserve capacity for ALL touched lists before publishing writes.
        // A deferred-GC scope covers this commit; the journal may contain
        // strings newly created by a nested WASM host expression.
        for (auto& s : stages)
            if (s.modified) s.owner->list->ensure(s.length());
        for (auto& s : stages) {
            if (!s.modified) continue;
            SList* list = s.owner->list;
            // Raw contiguous item/len writes below bypass SList::write0.
            // A commit is a single Scratch transaction, conservatively
            // bracketed for the process-wide read-observation epoch.
            ir::ScopedSourceListContentWrite write;
            if (!list->isContiguous()) {
                if (s.materialized) {
                    list->clear();
                    for (const Value item : s.items) list->push(item);
                } else {
                    for (const auto& [index, value] : s.replacements)
                        list->write0(index, value);
                    for (const Value item : s.appended) list->push(item);
                }
            } else if (s.materialized) {
                for (size_t i = 0; i < s.items.size(); ++i)
                    list->items[i] = s.items[i];
                list->len = uint32_t(s.items.size());
            } else {
                for (const auto& [index, value] : s.replacements)
                    list->items[index] = value;
                for (size_t i = 0; i < s.appended.size(); ++i)
                    list->items[list->len + i] = s.appended[i];
                list->len += uint32_t(s.appended.size());
            }
            // SList::numericOnly is monotone; a single nonnumeric Value
            // invalidates the numeric specialization for future readers.
            // Even if a later deletion removes the Value, leaving false is
            // conservative and matches the runtime's own flag handling.
            list->numericOnly &= !s.everNonNumeric;
            if (s.materialized) {
                for (const Value value : s.items)
                    list->numericOnly &= value.isNumber();
            } else {
                for (const auto& [index, value] : s.replacements)
                    list->numericOnly &= value.isNumber();
                for (const Value value : s.appended)
                    list->numericOnly &= value.isNumber();
            }
            list->monitorDirty = true;
        }
    }
};

struct ActiveListBindings {
    NumericListTransaction* transaction = nullptr;
};
inline thread_local ActiveListBindings activeListBindings;

struct ScopedListBindings {
    explicit ScopedListBindings(NumericListTransaction& txn)
        : previous_(activeListBindings) {
        activeListBindings.transaction = &txn;
    }
    ~ScopedListBindings() { activeListBindings = previous_; }
    ScopedListBindings(const ScopedListBindings&) = delete;
    ScopedListBindings& operator=(const ScopedListBindings&) = delete;
private:
    ActiveListBindings previous_;
};

// scratch[0] = numeric f64 index; result = exact tagged Value.
inline bool loadBoundTaggedListItem(void* scratch, uint32_t binding) {
    const auto* txn = activeListBindings.transaction;
    if (!scratch || !txn) return false;
    uint64_t indexBits = 0;
    std::memcpy(&indexBits, scratch, sizeof(indexBits));
    Value result;
    if (!txn->readIndexed(binding, Value{indexBits}, result))
        return false;
    std::memcpy(scratch, &result.bits, sizeof(result.bits));
    return true;
}

// scratch[0] = tagged needle; result = f64 0/1 or 1-based index.
inline bool queryBoundTaggedList(void* scratch, uint32_t binding,
                                 uint8_t op) {
    const auto* txn = activeListBindings.transaction;
    if (!scratch || !txn) return false;
    uint64_t bits = 0;
    std::memcpy(&bits, scratch, sizeof(bits));
    double result = 0;
    if (!txn->queryValue(binding, Value{bits}, op, result))
        return false;
    std::memcpy(scratch, &result, sizeof(result));
    return true;
}

// Format the *speculative* list with the same runtime formatter used by
// the interpreter. We materialize a temporary GC-managed SList so that
// rt::listContents can select the exact Scratch separator and UTF-16
// conversion. The enclosing WASM segment defers GC collection, and neither
// the original SList nor its monitor state is modified by this reporter.
inline bool getBoundListContents(void* scratch, uint32_t binding) {
    auto* txn = activeListBindings.transaction;
    if (!scratch || !txn || !txn->valid(binding)) return false;
    const uint32_t count = txn->stages[binding].length();
    if (xyo_list_limit && count > xyo_list_limit)
        return false; // avoid silently truncating a preexisting oversized list
    SList* temp = SList::create(std::max(4u, count));
    for (uint32_t i = 0; i < count; ++i) {
        Value value;
        if (!txn->readValue(binding, double(i) + 1, value))
            return false;
        temp->push(value);
    }
    const Value output = rt::listContents(temp);
    std::memcpy(scratch, &output.bits, sizeof(output.bits));
    return true;
}

inline bool loadBoundNumericListItem(void* scratch, uint32_t binding) {
    const auto* txn = activeListBindings.transaction;
    if (!scratch || !txn) return false;
    double index = 0;
    std::memcpy(&index, scratch, sizeof(index));
    uint64_t value = 0;
    if (!txn->read(binding, index, value)) return false;
    std::memcpy(scratch, &value, sizeof(value));
    return true;
}

// Scratch scratch[0] is both the numeric search key and result f64.
inline bool queryBoundNumericList(void* scratch, uint32_t binding,
                                  uint8_t op) {
    const auto* txn = activeListBindings.transaction;
    if (!scratch || !txn) return false;
    double needle = 0, result = 0;
    std::memcpy(&needle, scratch, sizeof(needle));
    if (!txn->query(binding, needle, op, result)) return false;
    std::memcpy(scratch, &result, sizeof(result));
    return true;
}

// Scratch scratch[0]=index f64; scratch[1]=raw tagged numeric Value.
// For append the index is ignored. No failure path mutates a live SList.
inline bool editBoundNumericList(void* scratch, uint32_t binding,
                                 NumericListTransaction::Edit operation) {
    auto* txn = activeListBindings.transaction;
    if (!scratch || !txn) return false;
    uint64_t indexBits = 0, bits = 0;
    std::memcpy(&indexBits, scratch, sizeof(indexBits));
    std::memcpy(&bits, static_cast<const uint8_t*>(scratch) + 8,
                sizeof(bits));
    return txn->writeTyped(operation, binding, Value{indexBits}, bits);
}

} // namespace xyo::wasm
