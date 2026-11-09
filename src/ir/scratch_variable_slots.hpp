// Transactional bridge from live Scratch scalar Variables to portable
// raw NaN-boxed external slots. The owner Target must remain alive/rooted,
// and no scheduler yield or GC pointer relocation is permitted while active.
// Scratch Values are copied, not referenced from the generated machine code.
#pragma once

#include "../model/target.hpp"
#include "scratch_numeric_kernel.hpp"
#include <unordered_set>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace xyo::ir {

class ScratchNumericSlots {
public:
    explicit ScratchNumericSlots(std::vector<Variable*> variables)
        : variables_(std::move(variables)) {
        original_.reserve(variables_.size());
        staged_.reserve(variables_.size());
        std::unordered_set<Variable*> seen;
        for (Variable* variable : variables_) {
            if (!validScalar(variable) || !seen.insert(variable).second) {
                valid_ = false;
                original_.clear();
                staged_.clear();
                return;
            }
            original_.push_back(variable->value.bits);
            staged_.push_back(variable->value.bits);
        }
        valid_ = true;
    }
    ScratchNumericSlots(Target& owner,
                        const std::vector<std::string>& slotIds)
        : ScratchNumericSlots(resolve(owner, slotIds)) {
        owner_ = &owner;
        slotIds_ = slotIds;
        if (valid_) {
            bindingStamp_ = owner.variableBindingStamp();
            bindingStampValid_ = true;
        }
    }

    ScratchNumericSlots(const ScratchNumericSlots&) = delete;
    ScratchNumericSlots& operator=(const ScratchNumericSlots&) = delete;

    // Reuse the same buffers at the next scheduler slice. Rebind IDs before
    // reading any borrowed Variable*, because a Scratch target may have
    // replaced or erased them since the preceding yield.
    bool refresh() {
        if (!owner_ || slotIds_.size() != variables_.size() ||
            original_.size() != variables_.size() ||
            staged_.size() != variables_.size())
            return false;
        valid_ = false;
        committed_ = false;
        const auto stamp = owner_->variableBindingStamp();
        const bool bindingsUnchanged = bindingStampValid_ &&
                                       stamp == bindingStamp_;
        for (size_t i = 0; i < variables_.size(); ++i) {
            // The epoch check MUST happen before the pointer is read:
            // erase+recreate may reuse its old address (ABA).
            Variable* variable = bindingsUnchanged
                ? variables_[i] : owner_->lookupVariableById(slotIds_[i]);
            if (!validScalar(variable)) return false;
            if (!bindingsUnchanged) {
                for (size_t j = 0; j < i; ++j)
                    if (variables_[j] == variable) return false;
                variables_[i] = variable;
            }
            original_[i] = variable->value.bits;
            staged_[i] = original_[i];
        }
        bindingStamp_ = stamp;
        bindingStampValid_ = true;
        valid_ = true;
        return true;
    }

    bool valid() const { return valid_; }
    size_t size() const { return staged_.size(); }
    uint64_t* data() { return valid_ ? staged_.data() : nullptr; }
    const std::vector<uint64_t>& bits() const { return staged_; }

    // All checks precede all writes, so a type change or failed compiled
    // segment never publishes half a transaction.
    bool commit() {
        if (!valid_ || committed_) return false;
        // Invalidate a transaction BEFORE dereferencing any cached pointer
        // when either local variables or a stage-global binding changed.
        // Unrelated structural changes fail conservatively, then refresh
        // rebinds the IDs on the following scheduler slice.
        if (owner_ && (!bindingStampValid_ ||
                       owner_->variableBindingStamp() != bindingStamp_))
            return false;
        for (size_t i = 0; i < variables_.size(); ++i) {
            const auto* variable = variables_[i];
            if (!validScalar(variable) ||
                variable->value.bits != original_[i] ||
                !Value{staged_[i]}.isNumber())
                return false;
        }
        for (size_t i = 0; i < variables_.size(); ++i) {
            auto* variable = variables_[i];
            // Preserve signed zero, infinities and finite doubles, but
            // ensure any NaN is published using the canonical Scratch tag.
            const Value candidate{staged_[i]};
            variable->value = Value::numCanonical(
                candidate.asNumberUnchecked());
            variable->observeValueType(variable->value);
        }
        committed_ = true;
        return true;
    }

private:
    static std::vector<Variable*> resolve(
            Target& owner, const std::vector<std::string>& slotIds) {
        std::vector<Variable*> result;
        result.reserve(slotIds.size());
        for (const auto& id : slotIds)
            result.push_back(id.empty() ? nullptr : owner.lookupVariableById(id));
        return result;
    }
    static bool validScalar(const Variable* variable) {
        return variable && variable->type == Variable::kScalarType &&
               !variable->isCloud && variable->value.isNumber();
    }

    std::vector<Variable*> variables_;
    Target* owner_ = nullptr;
    std::vector<std::string> slotIds_;
    std::vector<uint64_t> original_;
    std::vector<uint64_t> staged_;
    Target::VariableBindingStamp bindingStamp_{};
    bool bindingStampValid_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

// Integration point for either native LLVM (i64* argument) or WASM
// (copy in/out of an i64[] linear-memory snapshot). The callback executes
// synchronously and returns false on bailout; no variable write occurs until
// both the callback and all commit-time guards succeed.
template <typename Runner>
bool invokeNumericKernel(Target& target,
                         const std::vector<std::string>& slotIds,
                         Runner&& runner) {
    ScratchNumericSlots snapshot(target, slotIds);
    if (!snapshot.valid()) return false;
    if (!std::forward<Runner>(runner)(snapshot.data(), snapshot.size()))
        return false;
    return snapshot.commit();
}

// The strongly typed runtime entrypoint rejects any kernel that would
// skip Scratch scheduler checkpoints. It is deliberately separate from the
// lower-level slot-only callback used by numeric ABI unit tests.
template <typename Runner>
bool invokeNumericKernel(Target& target,
                         const ScratchNumericKernel& kernel,
                         Runner&& runner) {
    if (kernel.requiresLoopScheduler ||
        !kernel.executable.yieldBoundaries.empty() ||
        !kernel.executable.externalValueSlots ||
        !verifyExecutable(kernel.executable))
        return false;
    return invokeNumericKernel(target, kernel.slotIds,
                               std::forward<Runner>(runner));
}

} // namespace xyo::ir
