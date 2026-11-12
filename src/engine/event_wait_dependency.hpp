// Fast, conservative variable-dependency guard for pure Scratch wait-until.
// Pure native wait-until dependency watcher (up to four scalars opt-in).
// Enabled by default on native hosts, independently of XYO_EVENT_SCHED.
// XYO_WAIT_DEP_FAST=0 restores ordinary polling. Browser/WASM stays opt-in.
// The global progress-epoch scheduler is otherwise unchanged.
// A cached predicate is safe to skip only while every watched scalar retains
// identical raw bits and source/variable binding generations still match.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "thread.hpp"
#include "../model/target.hpp"

namespace xyo {

inline bool isolatedEventWaitDependencyEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("XYO_WAIT_DEP_FAST");
#if defined(__EMSCRIPTEN__)
        // Browser builds have a different compiler and scheduler surface.
        return e && e[0] == '1';
#else
        return !e || e[0] != '0';
#endif
    }();
    return enabled;
}

inline bool multiVariableEventWaitEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("XYO_WAIT_MULTIVAR");
#if defined(__EMSCRIPTEN__)
        // Never infer native pointer ownership from a browser/AOT snapshot.
        return false;
#else
        return v && v[0] == '1';
#endif
    }();
    return enabled;
}

inline bool eventWaitDependencyEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("XYO_EVENT_SCHED_DEP");
        return (e && e[0] == '1') || isolatedEventWaitDependencyEnabled();
    }();
    return enabled;
}

// site is an opaque native codegen token. Never dereference it directly:
// AOT may retain an address from an unrelated build. Only accept a site if
// its numeric value equals the address of a *currently owned* source Block.
inline void snapshotEventWaitDependency(Thread& thread, uintptr_t site) {
    thread.eventWatchValid = false;
    if (!eventWaitDependencyEnabled() || !thread.blockContainer ||
        !thread.target || !site) return;
    Blocks& blocks = *thread.blockContainer;
    const Block* wait = nullptr;
    for (auto& [_, block] : blocks.allBlocks()) {
        if (reinterpret_cast<uintptr_t>(&block) == site) {
            wait = &block;
            break;
        }
    }
    if (!wait || !blocks.isPureWaitCondition(wait)) return;
    const BlockInput* condition = wait->inputs.find("CONDITION");
    if (!condition) return;
    const std::string& first = condition->block.empty()
        ? condition->shadow : condition->block;
    if (first.empty()) return;

    std::vector<std::string> todo{first};
    std::unordered_set<std::string> seen;
    std::vector<const Variable*> dependents;
    const bool multi = multiVariableEventWaitEnabled();
    while (!todo.empty()) {
        std::string id = std::move(todo.back());
        todo.pop_back();
        if (!seen.insert(id).second || seen.size() > 64) return;
        const Block* block = blocks.getBlock(id);
        if (!block) return;
        if (block->opcode == "data_variable") {
            auto field = block->fields.find("VARIABLE");
            if (field == block->fields.end() || field->second.id.empty())
                return;
            Variable* v = thread.target->lookupVariableById(field->second.id);
            if (!v || v->type != Variable::kScalarType) return;
            bool alreadySeen = false;
            for (const Variable* known : dependents)
                alreadySeen |= known == v;
            if (!alreadySeen) {
                if ((!multi && !dependents.empty()) ||
                    dependents.size() == 4) return;
                dependents.push_back(v);
            }
        }
        for (const auto& [_, input] : block->inputs.items) {
            if (!input.block.empty()) todo.push_back(input.block);
            else if (!input.shadow.empty()) todo.push_back(input.shadow);
        }
    }
    if (dependents.empty()) return;

    const auto stamp = thread.target->variableBindingStamp();
    thread.eventWatchVariable = dependents[0];
    thread.eventWatchBits = dependents[0]->value.bits;
    if (dependents.size() > 1) {
        if (!thread.eventWatchAdditional)
            thread.eventWatchAdditional =
                std::make_unique<Thread::AdditionalWaitDependencies>();
        auto& extra = *thread.eventWatchAdditional;
        extra.count = uint8_t(dependents.size() - 1);
        for (size_t i = 1; i < dependents.size(); ++i) {
            extra.variables[i - 1] = dependents[i];
            extra.bits[i - 1] = dependents[i]->value.bits;
        }
    } else if (thread.eventWatchAdditional) {
        thread.eventWatchAdditional->count = 0;
    }
    thread.eventWatchSourceRevision = blocks.revision();
    thread.eventWatchStage = stamp.stage;
    thread.eventWatchLocalGeneration = stamp.localGeneration;
    thread.eventWatchStageGeneration = stamp.stageGeneration;
    thread.eventWatchValid = true;
}

// Check the ownership/version of snapshot identities without dereferencing
// any saved Variable*. Safe even when an embedder directly erases a variable.
inline bool eventWaitBindingSnapshotCurrent(const Thread& thread) {
    if (!thread.eventWatchValid || !thread.target || !thread.blockContainer ||
        !thread.eventWatchVariable) return false;
    if (thread.blockContainer->revision() != thread.eventWatchSourceRevision)
        return false;
    const auto stamp = thread.target->variableBindingStamp();
    return stamp.stage == thread.eventWatchStage &&
        stamp.localGeneration == thread.eventWatchLocalGeneration &&
        stamp.stageGeneration == thread.eventWatchStageGeneration;
}

inline bool canSkipEventWaitByDependency(const Thread& thread) {
    // Never dereference a saved Variable* until both binding generations and
    // the block source revision are verified.
    if (!eventWaitBindingSnapshotCurrent(thread)) return false;
    if (thread.eventWatchVariable->value.bits != thread.eventWatchBits)
        return false;
    if (thread.eventWatchAdditional) {
        const auto& extra = *thread.eventWatchAdditional;
        for (size_t i = 0; i < extra.count; ++i) {
            if (!extra.variables[i] ||
                extra.variables[i]->value.bits != extra.bits[i])
                return false;
        }
    }
    return true;
}

} // namespace xyo
