// xyo-cpp: engine/runtime.js 移植。
#include "runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../core/cast.hpp"
#include "../rt/runtime_helpers.hpp"
#include "fiber.hpp"
#include "hat_thread_snapshot.hpp"
#include "sequencer.hpp"
#include "interpreter_osr_snapshot.hpp"
#include "../jit/jit.hpp"
#include "../jit/nested_warp_callframe_projection.hpp"
#include "../ir/commit_once_effects.hpp"

namespace xyo {

/// コアブロックパッケージのプリミティブ/ハットを登録（blocks/ 側で定義）
void registerCoreBlocks(Runtime& rt);

bool dangerModeFromEnv() {
    const char* v = std::getenv("XYO_DANGER");
    return v != nullptr && *v != 0 && std::strcmp(v, "0") != 0;
}

bool blockCountEnabledFromEnv() {
    const char* v = std::getenv("XYO_BLOCK_COUNT");
    return v && *v && std::strcmp(v, "0") != 0;
}

Runtime::Runtime()
    : sequencer(std::make_unique<Sequencer>(*this)),
      // XYO_JIT=OFF では nojit_stub.cpp の空実装が入る（常時非 null に
      // して ->jit 参照の null チェックを不要にする）
      jit(std::make_unique<JitEngine>()),
      ioDevices(this),
      monitorBlocks(std::make_unique<Blocks>(this)),
      flyoutBlocks(std::make_unique<Blocks>(this)) {
    updateCurrentMSecs();
    registerCoreBlocks(*this);
    Gc::get().addRootProvider(this, [this](Gc& g) { markRoots(g); });
    // This opt-in is deliberately absent from the ordinary Scratch runtime.
    // The native JIT non-waiting broadcast path has a synchronous host
    // commit boundary; the WASM speculative lowering still falls back.
    if (const char* opt = std::getenv("XYO_EFFECT_JOURNAL");
        opt && opt[0] == '1')
        observableJournal = std::make_unique<ir::CommitOnceEffects>();
}

Runtime::~Runtime() {
    if (blockCountEnabled) {
        finishBlockCount();
        const auto elapsed = blockCountEndedAt - blockCountStartedAt;
        const double seconds = blockCountStartedAt == std::chrono::steady_clock::time_point{}
            ? 0.0 : std::chrono::duration<double>(elapsed).count();
        const uint64_t count = executedBlocks.load(std::memory_order_relaxed);
        const double rate = seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
        std::fprintf(stderr,
                     "[block-count] executed=%llu elapsed=%.9fs blocks/s=%.2f "
                     "(Scratch interpreter/native JIT)\n",
                     static_cast<unsigned long long>(count), seconds, rate);
    }
    if (observableJournal && std::getenv("XYO_EFFECT_JOURNAL_STATS"))
        std::fprintf(stderr,
                     "[effect-journal] committed_broadcasts=%llu "
                     "immediate_broadcasts=%llu pending=%zu rooted=%zu "
                     "committed_effects=%llu immediate_effects=%llu\n",
                     static_cast<unsigned long long>(observableCommittedBroadcasts),
                     static_cast<unsigned long long>(observableImmediateBroadcasts),
                     observableJournal->pendingCount(),
                     observableJournal->rootedPayloads(),
                     static_cast<unsigned long long>(observableCommittedEffects),
                     static_cast<unsigned long long>(observableImmediateEffects));
    // Remove journal-owned GC root provider before tearing down targets.
    observableJournal.reset();
    Gc::get().removeRootProvider(this);
    // targets は所有ポインタ（disposeTarget/stopAll でも delete 済み要素は
    // リストから除去されている）。残りをここで回収する。
    for (Target* t : targets) delete t;
    targets.clear();
}

void Runtime::finishBlockCount() {
    if (blockCountEnabled &&
        blockCountStartedAt != std::chrono::steady_clock::time_point{} &&
        blockCountEndedAt == std::chrono::steady_clock::time_point{})
        blockCountEndedAt = std::chrono::steady_clock::now();
}

// ------------------------------------------------------------- スレッド --

Thread* Runtime::pushThread(const std::string& id, Target* target,
                            bool stackClick, bool updateMonitor,
                            const Block* topHint) {
#if !defined(XYO_DISABLE_RT_PROF)
    struct PProf {
        bool enabled = std::getenv("XYO_PROF") != nullptr;
        double allocNs = 0, mapNs = 0, compileNs = 0; uint64_t n = 0;
        ~PProf() {
            if (enabled)
                fprintf(stderr,
                    "[prof] pushThread alloc=%.1f map=%.1f compile=%.1f ms(%llu)\n",
                    allocNs/1e6, mapNs/1e6, compileNs/1e6,
                    (unsigned long long)n);
        }
    };
    static PProf pp;
    auto now = []{ return std::chrono::steady_clock::now(); };
    std::chrono::steady_clock::time_point t0;
    if (pp.enabled) { t0 = now(); }
#endif
    static const bool recycleEnabled = [] {
        const char* env = std::getenv("XYO_THREAD_REUSE");
        return !env || env[0] != '0';
    }();
    std::unique_ptr<Thread> thread;
    if (recycleEnabled && !reusableThreads.empty()) {
        thread = std::move(reusableThreads.back());
        reusableThreads.pop_back();
        // Reconstruct rather than partially resetting: the old target,
        // variable slots, frame owners, profile hints, and ID cache must not
        // leak into a new logical Scratch script.
        std::string newId(id);
        std::destroy_at(thread.get());
        std::construct_at(thread.get(), std::move(newId));
    } else {
        thread = std::make_unique<Thread>(id);
    }
    Thread* t = thread.get();
    t->target = target;
    t->stackClick = stackClick;
    t->updateMonitor = updateMonitor;
    t->blockContainer = updateMonitor ? monitorBlocks.get() : target->blocks;
    t->runtimeIndex = threads.size();
    threads.push_back(t);
    ownedThreads.push_back(std::move(thread));
    if (!t->stackClick && !t->updateMonitor) {
        threadMap[t->getId()] = t;
    }
#if !defined(XYO_DISABLE_RT_PROF)
    if (pp.enabled) { pp.allocNs += std::chrono::duration<double,std::nano>(now()-t0).count(); t0 = now(); }
#endif
    if (!updateMonitor) t->tryCompile(topHint);
    // A short synchronous native entry never needs the interpreter's root
    // StackFrame. Defer its heap allocation until a frame is first requested.
    // Every fallback/non-JIT script retains the eager root frame invariant.
    static const bool lazyRootFrame = [] {
        const char* env = std::getenv("XYO_LAZY_ROOT_FRAME");
        return !env || env[0] != '0';
    }();
    const auto* compiled = t->isCompiled
        ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
    if (lazyRootFrame && compiled && compiled->shortSync)
        t->stack.push_back(id);
    else
        t->pushStack(id);
#if !defined(XYO_DISABLE_RT_PROF)
    if (pp.enabled) { pp.compileNs += std::chrono::duration<double,std::nano>(now()-t0).count(); ++pp.n; }
#endif
    return t;
}

void Runtime::stopThread(Thread* thread) {
    thread->isKilled = true;
    sequencer->retireThread(*thread);
}

Thread* Runtime::restartThread(Thread* thread, const Block* topHint) {
    auto nt = std::make_unique<Thread>(thread->topBlock);
    Thread* n = nt.get();
    n->target = thread->target;
    n->stackClick = thread->stackClick;
    n->updateMonitor = thread->updateMonitor;
    n->blockContainer = thread->blockContainer;
    if (thread->isCompiled) {
        // A restarted script has exactly the same entry block and compiled
        // code. The CompiledScript is owned by JitEngine, not by the Thread.
        // Avoid repeating the JIT cache lookup and compilation decision for
        // every broadcast while preserving HotSpot promotion for cold scripts.
        n->triedToCompile = true;
        n->topBlockPtr = thread->topBlockPtr;
        n->compiled = thread->compiled;
        n->isCompiled = true;
    } else if (thread->triedToCompile) {
        n->tryCompile(topHint);
    }
    const auto* compiled = n->isCompiled
        ? static_cast<const CompiledScript*>(n->compiled) : nullptr;
    static const bool lazyRootFrame = [] {
        const char* env = std::getenv("XYO_LAZY_ROOT_FRAME");
        return !env || env[0] != '0';
    }();
    if (lazyRootFrame && compiled && compiled->shortSync)
        n->stack.push_back(thread->topBlock);
    else
        n->pushStack(thread->topBlock);
    if (!n->stackClick && !n->updateMonitor) {
        threadMap[n->getId()] = n;
    }
    // A restartable broadcast may replace hundreds of receivers in a
    // single scheduler pass. Search-by-identity costs O(receiver-count²).
    // The stored index is a hint, not an invariant: callers can modify the
    // public threads vector. Validate it and fall back to the exact scan.
    size_t index = thread->runtimeIndex;
    if (index >= threads.size() || threads[index] != thread) {
        const auto found = std::find(threads.begin(), threads.end(), thread);
        index = found == threads.end() ? threads.size()
                                       : size_t(found - threads.begin());
    }
    if (index < threads.size()) {
            n->runtimeIndex = index;
            threads[index] = n;
            ownedThreads.push_back(std::move(nt));
            // The replaced thread will never be scheduled again. Release
            // suspended fibers and their 1 MiB stack mappings immediately.
            // If the old thread is currently executing inside its own fiber,
            // retireThread defers release to jitStepThread's return path.
            // Scratch allows a receiver to re-broadcast its own message.
            // Although the replacement occupies its scheduling slot, the
            // currently executing old thread runs until its next yield.
            // Retire it at the sequencer boundary instead of mid-block.
            if (thread != sequencer->activeThread)
                sequencer->retireThread(*thread);
            return n;
    }
    n->runtimeIndex = threads.size();
    threads.push_back(n);
    ownedThreads.push_back(std::move(nt));
    sequencer->retireThread(*thread);
    return n;
}

bool Runtime::containsThread(Thread* thread) const {
    const size_t index = thread->runtimeIndex;
    if (index < threads.size() && threads[index] == thread) return true;
    // Validate the hint rather than trusting it: restart replaces an identity,
    // stopAll detaches the list, and embedders can edit the public vector.
    for (size_t i = 0; i < threads.size(); ++i) {
        if (threads[i] == thread) {
            thread->runtimeIndex = i;
            return true;
        }
    }
    return false;
}

bool Runtime::isActiveThread(Thread* thread) const {
    if (thread->stack.empty() || thread->status == STATUS_DONE) return false;
    return containsThread(thread);
}

bool Runtime::isWaitingThread(Thread* thread) const {
    return thread->status == STATUS_PROMISE_WAIT ||
           thread->status == STATUS_YIELD_TICK ||
           !isActiveThread(thread);
}

void Runtime::toggleScript(const std::string& topBlockId, Target* target,
                           bool stackClick) {
    for (Thread* t : threads) {
        if (t->target == target && t->topBlock == topBlockId &&
            t->status != STATUS_DONE) {
            const Block* block = target->blocks->getBlock(topBlockId);
            std::string opcode = block ? block->opcode : std::string{};
            if (getIsEdgeActivatedHat(opcode) && t->stackClick != stackClick) {
                continue;
            }
            stopThread(t);
            return;
        }
    }
    pushThread(topBlockId, target, stackClick, false);
}

void Runtime::addMonitorScript(const std::string& topBlockId, Target* optTarget) {
    if (!optTarget) optTarget = editingTarget;
    if (!optTarget) return;
    for (Thread* t : threads) {
        if (t->topBlock == topBlockId && t->status != STATUS_DONE &&
            t->updateMonitor) {
            return;
        }
    }
    pushThread(topBlockId, optTarget, false, true);
}

void Runtime::updateThreadMap() {
    threadMap.clear();
    for (Thread* t : threads) {
        if (!t->stackClick && !t->updateMonitor) {
            threadMap[t->getId()] = t;
        }
    }
}

// ---------------------------------------------------------------- startHats --

std::vector<Thread*> Runtime::startHats(
    const std::string& requestedHatOpcode,
    const std::map<std::string, std::u16string>* optMatchFields,
    Target* optTarget) {
    std::vector<Thread*> newThreads;
    auto hit = hats.find(requestedHatOpcode);
    if (hit == hats.end()) return newThreads;
    const HatMeta& hatMeta = hit->second;

    std::map<std::string, std::u16string> matchFields;
    if (optMatchFields) {
        for (auto& [k, v] : *optMatchFields) matchFields[k] = u16ToUpper(v);
    }

    HatThreadSnapshot existing(threads);
    const auto spawnMatched = [&](const std::string& topBlockId,
                                  const Block* top, Target* target) {
        if (hatMeta.restartExistingThreads) {
            static thread_local std::string lookupKey;
            lookupKey.assign(target->id);
            lookupKey.push_back('&');
            lookupKey.append(topBlockId);
            auto it = threadMap.find(lookupKey);
            if (it != threadMap.end()) {
                newThreads.push_back(restartThread(it->second, top));
                return;
            }
        } else if (existing.contains(target, topBlockId)) {
            return;
        }
        newThreads.push_back(
            pushThread(topBlockId, target, false, false, top));
    };
    // Broadcast-and-wait must keep its returned Thread* list (unlike the
    // fire-and-forget path), but it can use exactly the same subscriber index.
    // Other hats and explicit target selections retain generic matching.
    const bool indexedBroadcast =
        requestedHatOpcode == "event_whenbroadcastreceived" &&
        optTarget == nullptr && matchFields.size() == 1 &&
        matchFields.begin()->first == "BROADCAST_OPTION";
    if (indexedBroadcast) {
        const auto receivers = broadcastReceivers(matchFields.begin()->second);
        if (receivers) {
            for (const BroadcastReceiver& receiver : *receivers)
                spawnMatched(receiver.blockId, receiver.top, receiver.target);
        }
    } else {
        forEachScriptByOpcode(requestedHatOpcode,
            [&](const RuntimeScriptCache& script, Target* target) {
                for (const auto& [field, expected] : matchFields) {
                    auto it = script.fieldsOfInputs.find(field);
                    if (it == script.fieldsOfInputs.end()) return;
                    const Value& value = it->second;
                    SStr* str = value.isString() ? value.asStrPtr() : nullptr;
                    if (!str || str->view() != expected) return;
                }
                spawnMatched(script.blockId, script.block, target);
            }, optTarget);
    }

    // Scratch 2 互換: 新スレッドのハット即時評価
    for (Thread* t : newThreads) {
        if (t->isCompiled) {
            // JIT 側の処理（後で）
        } else {
            execute(*sequencer, *t);
            t->goToNextBlock();
        }
    }
    return newThreads;
}

bool Runtime::stageBroadcastNamesAsciiOnly() {
    const Target* stage = getTargetForStage();
    if (asciiBroadcastCacheStep == stepSerial &&
        asciiBroadcastCacheStage == stage)
        return asciiBroadcastCacheValue;

    bool ascii = stage != nullptr;
    if (stage) {
        for (const auto& [id, variable] : stage->variables) {
            if (variable.type != Variable::kBroadcastType) continue;
            if (std::any_of(variable.name.begin(), variable.name.end(),
                            [](char16_t c) { return c >= 128; })) {
                ascii = false;
                break;
            }
        }
        if (ascii) {
            for (const auto& [id, variable] : stage->collidingBroadcasts) {
                if (std::any_of(variable.name.begin(), variable.name.end(),
                                [](char16_t c) { return c >= 128; })) {
                    ascii = false;
                    break;
                }
            }
        }
    }
    asciiBroadcastCacheStage = stage;
    asciiBroadcastCacheValue = ascii;
    asciiBroadcastCacheStep = stepSerial;
    return ascii;
}

std::shared_ptr<const std::vector<Runtime::BroadcastReceiver>>
Runtime::broadcastReceivers(std::u16string_view name) {
    static const bool lastLookupCache = [] {
        const char* setting = std::getenv("XYO_BCAST_RECEIVER_CACHE");
        return !setting || setting[0] != '0';
    }();
    if (lastLookupCache && !broadcastIndexDirty &&
        lastBroadcastLookupValid && lastBroadcastLookupName == name)
        return lastBroadcastLookupReceivers;

    if (broadcastIndexDirty) {
        lastBroadcastLookupValid = false;
        lastBroadcastLookupReceivers.reset();
        // Collect subscriptions once in Scratch execution order. Clones
        // share Blocks, but retain independent Thread identities and their
        // insertion position in executableTargets.
        std::unordered_map<std::u16string, std::vector<BroadcastReceiver>> groups;
        static const std::string opcode = "event_whenbroadcastreceived";
        static const std::string fieldName = "BROADCAST_OPTION";
        for (auto it = executableTargets.rbegin();
             it != executableTargets.rend(); ++it) {
            Target* target = *it;
            for (const auto& script : target->blocks->getRuntimeScripts(opcode)) {
                const auto field = script.fieldsOfInputs.find(fieldName);
                if (field == script.fieldsOfInputs.end() ||
                    !field->second.isString()) continue;
                SStr* str = field->second.asStrPtr();
                if (!str) continue;
                groups[std::u16string(str->view())].push_back(
                    {target, script.blockId, script.block});
            }
        }
        broadcastIndex.clear();
        for (auto& [key, list] : groups) {
            broadcastIndex.emplace(
                std::move(key),
                std::make_shared<const std::vector<BroadcastReceiver>>(
                    std::move(list)));
        }
        broadcastIndexDirty = false;
    }
    const auto it = broadcastIndex.find(u16ToUpper(name));
    const auto receivers = it == broadcastIndex.end() ? nullptr : it->second;
    if (lastLookupCache) {
        lastBroadcastLookupName.assign(name);
        lastBroadcastLookupReceivers = receivers;
        lastBroadcastLookupValid = true;
    }
    return receivers;
}

// Only the non-waiting broadcast path uses this API. Broadcast-and-wait
// must preserve the exact returned Thread* set for its join semantics.
void Runtime::startBroadcast(std::u16string_view name) {
    // Preserve the original JIT fast path with no indirect callback when
    // the experimental journal is disabled (the default).
    if (!observableJournal) {
        startBroadcastImmediate(name);
        return;
    }
    journalBroadcast(name, [](void* context, std::u16string_view message) noexcept {
        static_cast<Runtime*>(context)->startBroadcastImmediate(message);
    }, this);
}

void Runtime::journalBroadcast(std::u16string_view name,
                               BroadcastDelivery deliver, void* context) {
    if (!deliver) return;
    if (!observableJournal) {
        deliver(context, name);
        return;
    }
    // A committed broadcast can synchronously start a receiver that
    // broadcasts again. Deliver it immediately to preserve Scratch's
    // receiver start order rather than queue it behind the outer drain.
    if (observableJournal->draining() ||
        observableJournal->committing() ||
        observableSliceId == UINT64_MAX) {
        ++observableImmediateBroadcasts;
        // The borrowed name may point to GC-managed SStr memory. Nested
        // delivery can collect that source; copy it before invoking hooks.
        const std::u16string owned(name);
        deliver(context, owned);
        return;
    }
    // Own the UTF-16 name before any GC-capable operation, since the
    // caller may have borrowed a temporary Scratch SStr reporter.
    const std::u16string owned(name);
    const uint64_t slice = ++observableSliceId;
    auto pending = observableJournal->begin(slice);
    bool staged = false;
    {
        Gc::DeferCollect defer; // only until the journal has an exact root
        const Value payload = owned.empty() ? Value::emptyStr()
            : Value::string(SStr::create(owned));
        staged = pending.stage(ir::ObservableOp::Broadcast,
                               "runtime.broadcast", payload);
    }
    // This path runs at a non-speculative synchronous host boundary;
    // if journaling is unavailable, execute the original immediate
    // primitive instead. No pending event was ever published on failure.
    if (!staged || !pending.commit([]() noexcept { return true; })) {
        ++observableImmediateBroadcasts;
        deliver(context, owned);
        return;
    }
    const auto result = observableJournal->drain(
        [deliver, context](ir::ObservableOp op, const std::string&,
                           Value retained) noexcept {
            if (op != ir::ObservableOp::Broadcast) return;
            // The Value remains rooted through any nested callback or GC.
            const std::u16string message(retained.isEmptyString()
                ? std::u16string_view{} : retained.asStrPtr()->view());
            deliver(context, message);
        });
    observableCommittedBroadcasts += result.delivered;
}

// Indexed broadcast-and-wait uses the same receiver snapshot and Scratch
// ordering as startHats, but skips construction of a one-entry std::map,
// matching/normalizing fields for every broadcast, and the generic hat walk.
// Unlike fire-and-forget startBroadcast, it must preserve distinct Thread*
// join handles until the caller has observed their completion.
static std::vector<Thread*> startBroadcastAndWaitIndexed(
    Runtime& rt, std::u16string_view name) {
    static const std::string opcode = "event_whenbroadcastreceived";
    const auto hat = rt.hats.find(opcode);
    if (hat == rt.hats.end()) return {};
    const HatMeta meta = hat->second;
    const auto receivers = rt.broadcastReceivers(name);
    if (!receivers) return {};

    std::vector<Thread*> started;
    started.reserve(receivers->size());
    HatThreadSnapshot existing(rt.threads);
    static thread_local std::string key;
    for (const Runtime::BroadcastReceiver& receiver : *receivers) {
        Thread* thread = nullptr;
        if (meta.restartExistingThreads) {
            key.assign(receiver.target->id);
            key.push_back('&');
            key.append(receiver.blockId);
            const auto it = rt.threadMap.find(key);
            if (it != rt.threadMap.end())
                thread = rt.restartThread(it->second, receiver.top);
        } else if (existing.contains(receiver.target, receiver.blockId)) {
            continue;
        }
        if (!thread)
            thread = rt.pushThread(receiver.blockId, receiver.target,
                                   false, false, receiver.top);
        started.push_back(thread);
    }
    for (Thread* thread : started) {
        if (!thread->isCompiled) {
            execute(*rt.sequencer, *thread);
            thread->goToNextBlock();
        }
    }
    return started;
}

std::vector<Thread*> Runtime::startBroadcastAndWait(
    std::u16string_view name) {
    // Keep the opt-in journal path's original synchronous commit semantics.
    // Allow an A/B comparison using identical native binaries. The legacy
    // path is also useful as a conservative fallback for diagnosis.
    static const bool indexed = [] {
        const char* env = std::getenv("XYO_BROADCAST_WAIT_INDEXED");
        return !env || *env != '0';
    }();
    if (!observableJournal) {
        if (indexed)
            return startBroadcastAndWaitIndexed(*this, name);
        std::map<std::string, std::u16string> fields{
            {"BROADCAST_OPTION", std::u16string(name)}};
        return startHats("event_whenbroadcastreceived", &fields, nullptr);
    }
    struct Delivery {
        Runtime* runtime;
        std::vector<Thread*> started;
    } delivery{this, {}};
    journalBroadcast(name,
        [](void* state, std::u16string_view message) noexcept {
            auto& ctx = *static_cast<Delivery*>(state);
            std::map<std::string, std::u16string> fields{
                {"BROADCAST_OPTION", std::u16string(message)}};
            ctx.started = ctx.runtime->startHats(
                "event_whenbroadcastreceived", &fields, nullptr);
        }, &delivery);
    return std::move(delivery.started);
}

void Runtime::journalObservable(ir::ObservableOp op, Value payload,
                                ObservableDelivery deliver, void* context) {
    if (!deliver) return;
    if (!observableJournal) {
        deliver(context, payload);
        return;
    }
    // Scratch receiver events can synchronously trigger new host effects.
    // Maintain original depth-first dispatch order, even under reentrancy.
    // The usual committed host boundary never has an undrained slice.
    if (observableJournal->committing() || observableJournal->draining() ||
        observableJournal->pendingCount() != 0 ||
        observableSliceId == UINT64_MAX) {
        ++observableImmediateEffects;
        // Reentrant effects stay depth-first, but their payload must remain
        // an exact GC root throughout callbacks that can themselves collect.
        // Never suppress collection for the entire nested callback.
        ir::GcValueHandles roots;
        std::optional<ir::RootedValueHandle> root;
        {
            Gc::DeferCollect defer;
            root = roots.retain(payload);
        }
        if (root) {
            deliver(context, *roots.resolve(*root));
        } else {
            // Token exhaustion: retain original host semantics, disabling
            // collection only when we cannot establish an exact root.
            Gc::DeferCollect defer;
            deliver(context, payload);
        }
        return;
    }

    auto slice = observableJournal->begin(++observableSliceId);
    bool staged = false;
    {
        Gc::DeferCollect defer;
        staged = slice.stage(op, "runtime.observable", payload);
    }
    if (!staged || !slice.commit([]() noexcept { return true; })) {
        ++observableImmediateEffects;
        Gc::DeferCollect defer;
        deliver(context, payload);
        return;
    }
    const auto result = observableJournal->drain(
        [deliver, context, op](ir::ObservableOp actual, const std::string&,
                               Value exactValue) noexcept {
            // This synchronous host boundary may have only its one
            // freshly committed record. If future scheduler async slices
            // are added, use typed dispatch rather than dropping effects.
            if (actual == op) deliver(context, exactValue);
        });
    observableCommittedEffects += result.delivered;
}

void Runtime::startBroadcastImmediate(std::u16string_view name) {
    static const std::string opcode = "event_whenbroadcastreceived";
    const auto hat = hats.find(opcode);
    if (hat == hats.end()) return;
    const HatMeta meta = hat->second;

    const auto receivers = broadcastReceivers(name);
    if (!receivers) return;

    HatThreadSnapshot existing(threads);
    std::vector<Thread*> interpreted;
    for (const BroadcastReceiver& receiver : *receivers) {
            Target* target = receiver.target;
            const std::string& top = receiver.blockId;
            Thread* next = nullptr;
            if (meta.restartExistingThreads) {
                static thread_local std::string key;
                key.assign(target->id);
                key.push_back('&');
                key.append(top);
                const auto it = threadMap.find(key);
                if (it != threadMap.end()) {
                    Thread* previous = it->second;
                    // A JIT broadcast without wait never exposes its receiver
                    // to a join. Reuse a compiled receiver's storage instead
                    // of allocating and retiring another Thread per broadcast.
                    // A wait-created receiver, the active sender, or any
                    // currently executing fiber must retain its old identity.
                    if (previous->reusableBroadcastReceiver &&
                        previous->isCompiled && !previous->isKilled &&
                        previous != sequencer->activeThread &&
                        (!previous->fiber || previous->fiber != currentFiber())) {
                        sequencer->retireThread(*previous);
                        previous->releaseFinished();
                        previous->compatibilityStackFrame = ExecContext{};
                        previous->status = STATUS_RUNNING;
                        previous->isKilled = false;
                        previous->requestScriptGlowInFrame = false;
                        previous->hasWarpTimer = false;
                        previous->pushStack(top);
                        next = previous;
                    } else {
                        next = restartThread(previous, receiver.top);
                    }
                }
            } else if (existing.contains(target, top)) {
                // Ignore only this receiver. Other matching hats still start.
                continue;
            }
            if (!next) next = pushThread(top, target, false, false, receiver.top);
            next->reusableBroadcastReceiver = next->isCompiled;
            if (!next->isCompiled) interpreted.push_back(next);
    }
    for (Thread* t : interpreted) {
        execute(*sequencer, *t);
        t->goToNextBlock();
    }
}

// --------------------------------------------------------------- stop 系 --

void Runtime::stopForTarget(Target* target, Thread* optException) {
    emitStopForTarget(target, optException);
    for (Thread* t : threads) {
        if (t == optException) continue;
        if (t->target == target) stopThread(t);
    }
}

void Runtime::stopAll() {
    invalidateBroadcastIndex();
    emitProjectStopAll();
    // クローンを廃棄（dispose が executableTargets/sprite.clones から除去）
    std::vector<Target*> keep;
    for (Target* t : targets) {
        t->onStopAll();
        if (!t->isOriginal) {
            if (onTargetRemoved) onTargetRemoved(t);
            t->dispose();
            delete t;
        } else {
            keep.push_back(t);
        }
    }
    targets = std::move(keep);
    if (sequencer->activeThread) stopThread(sequencer->activeThread);
    // JS では this.threads = [] に留めスレッドオブジェクトは GC 任せ。
    // 実行中プリミティブ（control_stop 等）が自スレッドの参照を保持する
    // ため、ownedThreads からは ~Runtime まで解放しない。
    // また JS は新しい配列に「置き換える」ため、シーケンサが捕まえた
    // 旧配列はそのパス中は実行が継続される — ここでも detach で再現する。
    if (sequencer->stepping == &threads) {
        auto old = std::make_unique<std::vector<Thread*>>();
        old->swap(threads);                  // old: 旧中身, threads: 空
        sequencer->stepping = old.get();
        sequencer->detachedLists.push_back(std::move(old));
    } else {
        threads.clear();
    }
    threadMap.clear();
}

void Runtime::greenFlag() {
    stopAll();
    if (blockCountEnabled) {
        executedBlocks.store(0, std::memory_order_relaxed);
        blockCountStartedAt = std::chrono::steady_clock::now();
        blockCountEndedAt = {};
    }
    emitProjectStart();
    updateCurrentMSecs();
    ioDevices.clock.resetProjectTimer();
    for (Target* t : targets) t->clearEdgeActivatedValues();
    for (Target* t : targets) t->onGreenFlag();
    // Opt-in background prefetch never delays or reorders Scratch threads.
    // It gives one-shot warp broadcast handlers a chance to compile before
    // their first invocation, where ordinary top-level OSR is unavailable.
    if (jit) jit->prefetchWarpBroadcasts(*this);
    // The explicitly blocking experiment may spend a few hundred ms in
    // LLVM before green flag scripts begin. Do not charge that startup time
    // against Scratch's project timer.
    if (const char* prefetch = std::getenv("XYO_JIT_ASYNC_PREFETCH");
        prefetch && prefetch[0] == '2')
        ioDevices.clock.resetProjectTimer();
    startHats("event_whenflagclicked", nullptr, nullptr);
    if (onGreenFlag) onGreenFlag();
}

// ------------------------------------------------------------------ step --

void Runtime::reclaimThreads() {
    // Keep handles exposed by runtime execution state, including detached
    // scheduler passes and completed receivers held by a join. Moving the
    // unique_ptrs never changes Thread addresses; recycling an observed identity
    // would, however, introduce an ABA bug in broadcast-and-wait.
    static const bool stampedRefs = [] {
        const char* env = std::getenv("XYO_THREAD_RECLAIM_STAMP");
        return env && env[0] == '1';
    }();
    uint64_t epoch = 0;
    if (stampedRefs) {
        epoch = ++reclaimRetentionEpoch;
        if (epoch == 0) {
            // This rollover is practically unreachable, but never allow
            // an ancient mark to be mistaken for a current live reference.
            for (auto& up : ownedThreads)
                if (up) up->reclaimRetentionEpoch = 0;
            epoch = ++reclaimRetentionEpoch;
        }
    }
    std::unordered_set<const Thread*> refs;
    std::vector<Thread*> pending;
    const auto retain = [&](Thread* t) {
        if (!t) return;
        if (stampedRefs) {
            if (t->reclaimRetentionEpoch == epoch) return;
            t->reclaimRetentionEpoch = epoch;
            pending.push_back(t);
        } else if (refs.insert(t).second) {
            pending.push_back(t);
        }
    };
    for (Thread* t : threads) retain(t);
    for (Thread* t : lastStepDoneThreads) retain(t);
    for (const auto& entry : waitQueue) retain(entry.thread);
    for (const auto& [id, t] : threadMap) retain(t);
    retain(sequencer->activeThread);
    if (sequencer->stepping)
        for (Thread* t : *sequencer->stepping) retain(t);
    for (const auto& list : sequencer->detachedLists)
        for (Thread* t : *list) retain(t);
    for (const auto& up : ownedThreads) {
        if (up && (up->status != STATUS_DONE || up->fiber || up->jitCtx))
            retain(up.get());
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        Thread* t = pending[i];
        const auto retainFrames = [&](const auto& frames) {
            for (const auto& f : frames) {
                if (!f->executionContext) continue;
                for (Thread* s : f->executionContext->startedThreads) retain(s);
            }
        };
        retainFrames(t->stackFrames);
        retainFrames(t->trashFrames);
        for (Thread* s : t->compatibilityStackFrame.startedThreads) retain(s);
        if (t->jitCtx && t->jitCtx->joinRefs)
            for (Thread* s : *t->jitCtx->joinRefs) retain(s);
    }
    // Compact ownership, not just Thread payloads. Threads no longer
    // reachable through *any* join, runtime, frame or detached-pass reference
    // can donate their storage. Crucially this check occurs before recycling,
    // not opportunistically inside pushThread(), which cannot prove liveness.
    static const bool recycleEnabled = [] {
        const char* env = std::getenv("XYO_THREAD_REUSE");
        return !env || env[0] != '0';
    }();
    constexpr size_t kMaxReusableThreads = 2048;
    std::erase_if(ownedThreads, [&](auto& up) {
        if (!up) return true;
        if (stampedRefs ? up->reclaimRetentionEpoch == epoch
                        : refs.contains(up.get()))
            return false;
        if (recycleEnabled && reusableThreads.size() < kMaxReusableThreads) {
            up->releaseFinished();
            reusableThreads.push_back(std::move(up));
        }
        return true;
    });
    reclaimAt = ownedThreads.size() + ownedThreads.size() / 4 + 1024;
}

void Runtime::step() {
    ++stepSerial;
    static const bool tracePhases = std::getenv("XYO_STEP_PHASES") != nullptr;
    using ProfileClock = std::chrono::steady_clock;
    const auto phaseStart = tracePhases ? ProfileClock::now()
                                        : ProfileClock::time_point{};
    if (jit) {
        jit->pollAsync();
        // A pending thread can become native before its first instruction.
        // Started interpreter threads need a dedicated OSR continuation:
        // restarting the original entry would replay earlier side effects.
        if (jitEnabled()) {
            static const bool osrEnabled = [] {
                const char* enabled = std::getenv("XYO_JIT_OSR");
                return enabled && enabled[0] == '1';
            }();
            for (Thread* t : threads) {
                if (!t || t->isCompiled || !t->triedToCompile ||
                    t->isKilled || !t->topBlockPtr ||
                    (t->interpreterStarted && !osrEnabled))
                    continue;
                const CompiledScript* ready =
                    jit->readyScript(t->topBlockPtr);
                if (!ready) continue;
                const auto& nestedCalls = ready->nestedOsrCallPath;
                const auto& nestedFrames = ready->nestedOsrFramePath;
                const auto& nestedArms = ready->nestedOsrBranchArms;
                const size_t nestedDepth = nestedFrames.size() + 1;
                // Only exact immutable source callsites may be adopted.
                // All procedure frames stay rooted in the interpreter.
                bool nestedPathMatches = ready->nestedOsrEntry &&
                    t->blockContainer && ready->nestedOsrSourceRevision != 0 &&
                    t->blockContainer->revision() ==
                        ready->nestedOsrSourceRevision &&
                    !nestedCalls.empty() && nestedCalls.size() <= 8 &&
                    nestedFrames.size() >= nestedCalls.size() &&
                    nestedFrames.size() <= 14 &&
                    nestedArms.size() == nestedFrames.size() &&
                    ready->nestedOsrArgFrameIndex < nestedFrames.size() &&
                    nestedArms[ready->nestedOsrArgFrameIndex] == 0 &&
                    nestedFrames[ready->nestedOsrArgFrameIndex] ==
                        nestedCalls.back() &&
                    ready->nestedOsrCallId == nestedCalls.back() &&
                    t->stack.size() == nestedDepth &&
                    t->stackFrames.size() == nestedDepth &&
                    t->stack.back() == ready->nestedOsrLoopId;
                if (nestedPathMatches) {
                    for (size_t i = 0; i < nestedFrames.size(); ++i) {
                        if (t->stack[i] != nestedFrames[i]) {
                            nestedPathMatches = false;
                            break;
                        }
                    }
                }
                if (nestedPathMatches && !t->nestedOsrActive &&
                    t->interpreterStarted &&
                    !t->stackClick && !t->updateMonitor &&
                    (t->status == STATUS_RUNNING ||
                     t->status == STATUS_YIELD ||
                     t->status == STATUS_YIELD_TICK)) {
                    const StackFrame* call =
                        t->stackFrames[ready->nestedOsrArgFrameIndex].get();
                    const StackFrame* loop =
                        t->stackFrames[nestedDepth - 1].get();
                    const ExecContext* ec = loop && loop->executionContext
                        ? loop->executionContext.get() : nullptr;
                    // Everything executed so far (including any side
                    // effects and procedure prefix) stays committed. Only
                    // the private remaining count enters the native region.
                    // In particular no procedure args/reporter temporaries
                    // are manufactured or re-evaluated after this point.
                    // The callsite and source lexical ancestry must be
                    // unchanged. The argument names exactly match the
                    // immutable procedure signature, without duplicates or
                    // missing defaults. The original interpreter call frame
                    // remains alive and GC-rooted throughout native work.
                    const auto checkpoint = ir::InterpreterOsrSnapshot::capture(
                        *t, stepSerial);
                    const auto lexical = checkpoint
                        ? checkpoint->sourcePath() : std::nullopt;
                    // Validate each live caller independently, including
                    // warp signature, argument map and executed callsite.
                    // Snapshot::sourcePath() additionally proves every
                    // lexical edge at the current source revision/epoch.
                    bool argumentsValid = t->blockContainer != nullptr;
                    size_t innerArguments = 0;
                    size_t innerExpected = 0;
                    size_t verifiedCalls = 0;
                    for (size_t i = 0;
                         argumentsValid && i < nestedFrames.size(); ++i) {
                        const StackFrame* frame = t->stackFrames[i].get();
                        const Block* source =
                            t->blockContainer->getBlock(nestedFrames[i]);
                        const uint8_t arm = nestedArms[i];
                        if (!frame || !source || frame->waitingReporter ||
                            frame->hasReported || frame->suspendLast ||
                            !frame->reporting.empty() ||
                            !frame->reported.empty()) {
                            argumentsValid = false;
                            break;
                        }
                        if (arm == 3) {
                            // Parent repeat TIMES was evaluated by the
                            // original interpreter. Preserve its actual
                            // counter rather than recomputing it.
                            const ExecContext* context =
                                frame->executionContext.get();
                            argumentsValid =
                                source->opcode == "control_repeat" &&
                                !frame->hasParams && frame->isLoop &&
                                context && context->hasLoopCounter &&
                                !context->timer &&
                                std::isfinite(context->loopCounter) &&
                                context->loopCounter >= 0.0 &&
                                context->loopCounter <= 1099511627776.0 &&
                                std::floor(context->loopCounter) ==
                                    context->loopCounter;
                            continue;
                        }
                        if (arm != 0) {
                            // Exact, already-entered IF branch. Its source
                            // arm is checked against sourcePath below.
                            argumentsValid =
                                !frame->hasParams && !frame->isLoop &&
                                (source->opcode == "control_if" ||
                                 source->opcode == "control_if_else") &&
                                (arm == 1 ||
                                 (arm == 2 &&
                                  source->opcode == "control_if_else"));
                            continue;
                        }
                        if (verifiedCalls >= nestedCalls.size() ||
                            nestedCalls[verifiedCalls] != source->id) {
                            argumentsValid = false;
                            break;
                        }
                        ++verifiedCalls;
                        const Mutation* signature = source->mutation
                            ? t->blockContainer->getProcedurePrototype(
                                  source->mutation->attr("proccode"))
                            : nullptr;
                        argumentsValid =
                            source->opcode == "procedures_call" &&
                            source->mutation &&
                            !source->mutation->isReporterCall() &&
                            frame->hasParams && frame->executionContext &&
                            frame->executionContext->executed &&
                            signature && signature->warp() &&
                            signature->argumentNames.size() <= 16 &&
                            signature->argumentNames.size() ==
                                signature->argumentIds.size() &&
                            frame->params.size() ==
                                signature->argumentNames.size();
                        if (!argumentsValid) break;
                        for (const auto& name : signature->argumentNames) {
                            if (!frame->params.contains(name)) {
                                argumentsValid = false;
                                break;
                            }
                        }
                        if (i == ready->nestedOsrArgFrameIndex) {
                            innerArguments = frame->params.size();
                            innerExpected = signature->argumentNames.size();
                        }
                    }
                    argumentsValid = argumentsValid &&
                        verifiedCalls == nestedCalls.size();
                    if (const char* trace =
                            std::getenv("XYO_JIT_OSR_NESTED_TRACE");
                        trace && trace[0] == '1')
                        std::fprintf(stderr,
                            "[jit-osr-nested] checkpoint top=%s "
                            "frames=%zu snapshot=%d lexical=%d argvalid=%d "
                            "args=%zu expected=%zu loop=%d remaining=%.0f\n",
                            t->topBlock.c_str(), t->stack.size(),
                            int(checkpoint.has_value()),
                            int(lexical.has_value()), int(argumentsValid),
                            innerArguments, innerExpected,
                            int(loop && loop->isLoop),
                            ec && ec->hasLoopCounter ? ec->loopCounter : -1.);
                    bool lexicalMatches = lexical &&
                        lexical->size() == nestedDepth;
                    if (lexicalMatches) {
                        for (size_t i = 0; i < nestedFrames.size(); ++i) {
                            const auto& source = (*lexical)[i];
                            if (source.sourceBlockId != nestedFrames[i]) {
                                lexicalMatches = false;
                                break;
                            }
                            if (nestedArms[i] == 3) {
                                if (source.branchArm != 0 ||
                                    !source.returnBlockId.empty()) {
                                    lexicalMatches = false;
                                    break;
                                }
                            } else if (nestedArms[i] != 0) {
                                if (source.branchArm != nestedArms[i] ||
                                    !source.returnBlockId.empty()) {
                                    lexicalMatches = false;
                                    break;
                                }
                            } else {
                                const Block* callsite =
                                    t->blockContainer->getBlock(
                                        nestedFrames[i]);
                                if (!callsite ||
                                    source.branchArm != 0 ||
                                    source.returnBlockId != callsite->next) {
                                    lexicalMatches = false;
                                    break;
                                }
                            }
                        }
                    }
                    // The optional IR-001 interoperability proof
                    // independently reconstructs canonical source PCs,
                    // verified return sites and typed live loop counters.
                    // This is readonly: actual execution remains owned by
                    // the existing Interpreter->LLVM inner-loop handoff.
                    std::optional<jit::NestedWarpSourceWitness> sourceProof;
                    bool sourceProofValid =
                        !ready->nestedOsrSourceProofRequired;
                    if (ready->nestedOsrSourceProofRequired && checkpoint &&
                        ready->nestedOsrSourceSymbols &&
                        checkpoint->validFor(*t, stepSerial)) {
                        sourceProof = jit::projectNestedWarpSource(
                            *checkpoint, *ready->nestedOsrSourceSymbols,
                            t->topBlock, nestedFrames, nestedArms,
                            ready->nestedOsrLoopId);
                        sourceProofValid = sourceProof.has_value() &&
                            sourceProof->calls.size() == nestedCalls.size();
                        if (sourceProofValid) {
                            // Revision alone is not enough: tools and
                            // extensions can directly edit Block payloads
                            // without calling resetCache(). Reconstruct the
                            // same immutable source digest used by IR-001.
                            // This O(source graph) check is opt-in and runs
                            // only at a *candidate* native adoption point.
                            const Block* root =
                                t->blockContainer->getBlock(t->topBlock);
                            const auto fresh = root
                                ? ir::ScratchCompilationUnit::capture(
                                      *t->blockContainer, root)
                                : ir::ScratchCompilationUnit{};
                            sourceProofValid = fresh.valid() &&
                                fresh.sourceRevision ==
                                    ready->nestedOsrSourceSymbols->sourceRevision &&
                                ir::scratchSourceDigest(fresh) ==
                                    ready->nestedOsrSourceSymbols->sourceDigest;
                        }
                    }
                    if (checkpoint && lexicalMatches && argumentsValid &&
                        sourceProofValid &&
                        checkpoint->validFor(*t, stepSerial) &&
                        checkpoint->hasCountedLoopCheckpoint() &&
                        (*lexical)[nestedDepth - 1].sourceBlockId ==
                            ready->nestedOsrLoopId &&
                        call && call->hasParams &&
                        !call->waitingReporter && !call->hasReported &&
                        call->reporting.empty() && call->reported.empty() &&
                        loop && loop->isLoop && loop->warpMode &&
                        !loop->hasParams && !loop->waitingReporter &&
                        !loop->hasReported && loop->reporting.empty() &&
                        loop->reported.empty() && !loop->suspendLast &&
                        ec && ec->hasLoopCounter &&
                        !ec->timer && !ec->hasStartedThreads &&
                        ec->vals.empty() &&
                        std::isfinite(ec->loopCounter) &&
                        ec->loopCounter >= 0.0 &&
                        ec->loopCounter <= 1099511627776.0 &&
                        std::floor(ec->loopCounter) == ec->loopCounter) {
                        t->osrRemaining = ec->loopCounter;
                        t->nestedOsrActive = true;
                        t->osrActive = true;
                        t->compiled = ready;
                        t->isCompiled = true;
                        if (sourceProof &&
                            std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
                            std::fprintf(stderr,
                                "[jit-osr-source-proof] calls=%zu "
                                "parent-loops=%zu branches=%zu leaf-pc=%llu "
                                "revision=%llu\n",
                                sourceProof->calls.size(),
                                sourceProof->parentLoops.size(),
                                sourceProof->branches.size(),
                                static_cast<unsigned long long>(
                                    sourceProof->innerLoop.sourcePc),
                                static_cast<unsigned long long>(
                                    sourceProof->sourceRevision));
                        if (std::getenv("XYO_JIT_OSR_TIMING"))
                            std::fprintf(stderr,
                                "[jit-osr-nested] promoted loop=%s "
                                "remaining=%.0f depth=%zu\n",
                                ready->nestedOsrLoopId.c_str(),
                                t->osrRemaining, t->stack.size());
                        continue;
                    }
                }
                if (!t->interpreterStarted) {
                    t->compiled = ready;
                    t->isCompiled = true;
                    continue;
                }
                // OSR is legal only at a top-level counted-loop boundary.
                // The interpreter already ran earlier statements; the
                // dedicated native entry runs precisely the remaining
                // iterations and the suffix after this loop.
                if (!ready->osrEntry || ready->osrLoopId.empty() ||
                    t->stack.size() != 1 ||
                    t->stack.back() != ready->osrLoopId ||
                    t->stackFrames.size() != 1 ||
                    t->stackClick || t->updateMonitor)
                    continue;
                StackFrame* frame = t->stackFrames.back().get();
                if (!frame || !frame->isLoop || frame->warpMode ||
                    frame->waitingReporter || frame->hasReported ||
                    frame->suspendLast || frame->hasParams ||
                    !frame->reporting.empty() ||
                    !frame->reported.empty() ||
                    (frame->executionContext &&
                     (frame->executionContext->timer ||
                      frame->executionContext->hasStartedThreads ||
                      !frame->executionContext->vals.empty())) ||
                    (t->status != STATUS_RUNNING &&
                     t->status != STATUS_YIELD &&
                     t->status != STATUS_YIELD_TICK))
                    continue;
                const Block* loop = t->blockContainer
                    ? t->blockContainer->getBlock(ready->osrLoopId)
                    : nullptr;
                if (!loop) continue;
                double remaining = 0.0;
                if (loop->opcode == "control_repeat") {
                    if (!frame->executionContext ||
                        !frame->executionContext->hasLoopCounter)
                        continue;
                    remaining = frame->executionContext->loopCounter;
                    if (!std::isfinite(remaining) ||
                        remaining < 0 || remaining > 1099511627776.0 ||
                        std::floor(remaining) != remaining)
                        continue;
                } else if (loop->opcode != "control_while" &&
                           loop->opcode != "control_repeat_until") {
                    continue;
                }
                t->osrActive = true;
                t->osrRemaining = remaining;
                t->compiled = ready;
                t->isCompiled = true;
                // A fresh compiled root frame is required by compat
                // helpers. Previous interpreter frames no longer contain
                // live reporter state and must not run twice.
                t->stack.clear();
                t->stackFrames.clear();
                t->pushStack(t->topBlock);
                if (std::getenv("XYO_JIT_OSR_TIMING"))
                    std::fprintf(stderr,
                        "[jit-osr] promoted loop=%s remaining=%.0f\n",
                        ready->osrLoopId.c_str(), remaining);
            }
        }
    }
    const auto phaseJit = tracePhases ? ProfileClock::now()
                                       : ProfileClock::time_point{};
    // 停止指示のあったスレッドを除去
    std::vector<Thread*> alive;
    alive.reserve(threads.size());
    for (Thread* t : threads) {
        if (!t->isKilled) {
            t->runtimeIndex = alive.size();
            alive.push_back(t);
        }
    }
    threads = std::move(alive);
    // killed スレッドは ownedThreads に残す: broadcastandwait 等の
    // startedThreads が生ポインタを保持するため（解放すると ABA になる）
    updateThreadMap();
    if (ownedThreads.size() >= reclaimAt) {
        lastStepDoneThreads.clear();   // 保持は 1 ステップだけ（読み手なし）
        reclaimThreads();
    }

    // エッジ起動ハットを毎フレーム評価
    for (auto& [hatType, meta] : hats) {
        if (meta.edgeActivated) startHats(hatType, nullptr, nullptr);
    }
    redrawRequested = false;
    pushMonitors();
    processWaitQueue();
    const auto phasePrepare = tracePhases ? ProfileClock::now()
                                          : ProfileClock::time_point{};
    std::vector<Thread*> doneThreads = sequencer->stepThreads();
    // A passive, source-versioned, GC-rooted capture of the interpreter's
    // live nested warp frames. It does not attach a compiled continuation;
    // the current LLVM ABI cannot adopt an arbitrary deep call stack.
    static const bool interpOsrCapture = [] {
        const char* enabled = std::getenv("XYO_INTERP_OSR_SNAPSHOT");
        return enabled && enabled[0] == '1';
    }();
    if (interpOsrCapture && stepSerial <= 20) {
        for (const Thread* t : threads) {
            if (!t || !t->interpreterStarted || t->isCompiled ||
                t->stack.size() < 3)
                continue;
            const auto snapshot = ir::InterpreterOsrSnapshot::capture(
                *t, stepSerial);
            if (!snapshot) {
                std::fprintf(stderr,
                    "[interp-osr-snapshot] frame=%llu script=%s rejected\n",
                    static_cast<unsigned long long>(stepSerial - 1),
                    t->topBlock.c_str());
                continue;
            }
            const auto lexical = snapshot->sourcePath();
            std::fprintf(stderr,
                "[interp-osr-snapshot] frame=%llu script=%s depth=%zu "
                "roots=%zu counted_loop=%d valid=%d lexical=%d\n",
                static_cast<unsigned long long>(stepSerial - 1),
                t->topBlock.c_str(), snapshot->depth(),
                snapshot->rootedArguments(),
                int(snapshot->hasCountedLoopCheckpoint()),
                int(snapshot->validFor(*t, stepSerial)),
                int(lexical.has_value()));
        }
    }
    const auto phaseExecute = tracePhases ? ProfileClock::now()
                                          : ProfileClock::time_point{};
    emitProjectRunStatus(
        int(threads.size() + doneThreads.size() -
            getMonitorThreadCount(threads) -
            getMonitorThreadCount(doneThreads)));
    lastStepDoneThreads = std::move(doneThreads);
    if (renderer) renderer->draw();
    if (tracePhases) {
        const auto phaseDraw = ProfileClock::now();
        const auto ms = [](ProfileClock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        if (ms(phaseDraw - phaseStart) >= 20.0)
            std::fprintf(stderr,
                "[step-phase] frame=%llu jit=%.3f prep=%.3f scripts=%.3f draw=%.3f total=%.3fms\n",
                static_cast<unsigned long long>(stepSerial - 1),
                ms(phaseJit - phaseStart), ms(phasePrepare - phaseJit),
                ms(phaseExecute - phasePrepare), ms(phaseDraw - phaseExecute),
                ms(phaseDraw - phaseStart));
    }
    if (refreshTargets) {
        emitTargetsUpdate();
        refreshTargets = false;
    }
    if (monitorState.dirty) {
        if (onMonitorsUpdate) onMonitorsUpdate(monitorState);
        monitorState.dirty = false;
    }
}

int Runtime::runUntilDone(int maxSteps) {
    int ran = 0;
    while (ran < maxSteps) {
        bool anyActive = false;
        for (Thread* t : threads) {
            if (!t->stack.empty() && t->status != STATUS_DONE) { anyActive = true; break; }
        }
        if (!anyActive) break;
        step();
        ++ran;
    }
    return ran;
}

void Runtime::updateCurrentMSecs() {
    // scratch-vm: runtime.currentMSecs = Date.now() (1 ms 精度)。
    // Timer::stdNow() はサブ ms を持つため、Scratch の timer 値
    // (整数 ms / 1000) に合わせて切り捨てる。ヘッドレスの step は
    // 実時間より速く回るので、フレームごとに最低 1ms 進める
    // (scratch-vm の 30fps では Date.now() が毎フレーム進む)。
    double now = std::floor(Timer::stdNow());
    currentMSecs = std::max(now, currentMSecs + 1.0);
}

void Runtime::processWaitQueue() {
    if (waitQueue.empty()) return;
    for (auto it = waitQueue.begin(); it != waitQueue.end();) {
        WaitEntry& w = *it;
        if (!w.nextTick && w.deadlineMs > currentMSecs) { ++it; continue; }
        if (w.bubbleTarget) {
            // sayforsecs: usageId が変わっていなければバブルをクリア
            if (w.bubbleTarget->bubbleUsageId == w.bubbleUsageId) {
                w.bubbleTarget->bubbleText.clear();
                w.bubbleTarget->bubbleType = w.bubbleType;
                if (onSay) onSay(w.bubbleTarget, w.bubbleType, u"");
            }
        }
        if (w.thread && w.thread->status == STATUS_PROMISE_WAIT) {
            resolveWait(*sequencer, *w.thread, w.resolveValue);
        }
        it = waitQueue.erase(it);
    }
}

// --------------------------------------------------------------- targets --

void Runtime::addTarget(Target* target) {
    invalidateBroadcastIndex();
    targets.push_back(target);
    executableTargets.push_back(target);
}

void Runtime::moveExecutable(Target* target, double delta) {
    invalidateBroadcastIndex();
    auto it = std::find(executableTargets.begin(), executableTargets.end(), target);
    if (it == executableTargets.end()) return;
    int oldIndex = int(it - executableTargets.begin());
    executableTargets.erase(it);
    // Scratch splices at ToInteger(oldIndex + delta), not at oldIndex + ToInteger(delta).
    // This distinction matters for negative fractional layer changes.
    const double dest = std::isnan(delta) ? oldIndex
        : std::clamp(double(oldIndex) + delta, 0.0, double(executableTargets.size()));
    int newIndex = static_cast<int>(dest);
    if (newIndex > int(executableTargets.size()))
        newIndex = int(executableTargets.size());
    if (newIndex <= 0) {
        if (!executableTargets.empty() && executableTargets[0]->isStage) newIndex = 1;
        else newIndex = 0;
    }
    executableTargets.insert(executableTargets.begin() + newIndex, target);
}

void Runtime::setExecutablePosition(Target* target, int newIndex) {
    // Integer layer placement already knows the destination. Calling the
    // relative move routine searches the same vector twice and converts
    // the index through floating point. Reorder directly, preserving its
    // clamp and stage-first rules (including out-of-range positions).
    static const bool popLastFast = [] {
        const char* env = std::getenv("XYO_EXEC_ORDER_POP_LAST");
        return env && env[0] == '1';
    }();
    // Newly created clones are appended by addTarget() and then
    // moved behind their source. Avoid an O(target-count) search
    // and erase when the element to move is already last.
    if (popLastFast && !executableTargets.empty() &&
        executableTargets.back() == target) {
        invalidateBroadcastIndex();
        executableTargets.pop_back();
    } else {
        auto it = std::find(executableTargets.begin(),
                            executableTargets.end(), target);
        if (it == executableTargets.end()) return;
        invalidateBroadcastIndex();
        executableTargets.erase(it);
    }
    int dest = std::clamp(newIndex, 0, int(executableTargets.size()));
    if (dest == 0 && !executableTargets.empty() &&
        executableTargets.front()->isStage)
        dest = 1;
    executableTargets.insert(executableTargets.begin() + dest, target);
}

void Runtime::removeExecutable(Target* target) {
    invalidateBroadcastIndex();
    if (!executableTargets.empty() && executableTargets.back() == target) {
        executableTargets.pop_back();
        return;
    }
    const auto it = std::find(executableTargets.begin(), executableTargets.end(), target);
    if (it != executableTargets.end()) executableTargets.erase(it);
}

void Runtime::disposeTarget(Target* target) {
    // The audio callback can run concurrently with the main thread. Retire
    // any voices before the clone's storage becomes invalid.
    if (target && target->sprite && target->sprite->soundBank)
        target->sprite->soundBank->targetDisposed(target);
    // Recently created Scratch clones are usually also the last target.
    // Avoid a linear membership scan for the common case, but retain
    // stable ordering and the exact erase semantics for other targets.
    if (!targets.empty() && targets.back() == target) {
        target->dispose();
        targets.pop_back();
    } else {
        const auto it = std::find(targets.begin(), targets.end(), target);
        if (it == targets.end()) return;
        target->dispose();
        targets.erase(it);
    }
    // editingTarget が消えたターゲットを指すと addMonitorScript が freed
    // ターゲットへスレッドを生成する（モニタ UAF）。次のスプライトへ移す。
    if (editingTarget == target) {
        editingTarget = nullptr;
        for (Target* t : targets) {
            if (!t->isStage) { editingTarget = t; break; }
        }
        if (!editingTarget && !targets.empty()) editingTarget = targets[0];
    }
    if (onTargetRemoved) onTargetRemoved(target);
    delete target;
}

Target* Runtime::getTargetForStage() const {
    // Normal Scratch projects keep the stage at index 0. The numeric
    // continuation's binding stamp consults it twice per scheduler slice;
    // scanning thousands of clones here would erase the slot-cache gain.
    // Preserve the linear fallback for editor-created/reordered targets.
    if (!targets.empty() && targets.front()->isStage)
        return targets.front();
    for (Target* t : targets) if (t->isStage) return t;
    return nullptr;
}

Target* Runtime::getTargetById(const std::string& id) const {
    for (Target* t : targets) if (t->id == id) return t;
    return nullptr;
}

Target* Runtime::getTargetByDrawableId(int drawableId) const {
    for (Target* t : targets) if (t->drawableID == drawableId) return t;
    return nullptr;
}

Target* Runtime::getSpriteTargetByName(const std::u16string& name) const {
    for (Target* t : targets) {
        if (!t->isStage && t->sprite && t->sprite->name == name) return t;
    }
    return nullptr;
}

// --------------------------------------------------------------- monitors --

void Runtime::pushMonitors() {
    if (targets.empty()) return;
    monitorBlocks->runAllMonitored(*this);
}

int Runtime::getMonitorThreadCount(const std::vector<Thread*>& list) const {
    int n = 0;
    for (Thread* t : list) if (t->updateMonitor) ++n;
    return n;
}

void Runtime::requestUpdateMonitor(
    const std::map<std::string, std::u16string>& fields) {
    auto idIt = fields.find("id");
    if (idIt == fields.end()) return;
    std::string id = SStr::toUtf8(idIt->second);
    MonitorRecord& rec = monitorState.getOrCreate(id);
    if (rec.id.empty()) rec.id = id;
    for (auto& [k, v] : fields) {
        if (k == "id") continue;
        rec.params[k] = v;
    }
    monitorState.dirty = true;
}

void Runtime::requestUpdateMonitorValue(const std::string& id, Value v,
                                        const std::u16string& spriteName) {
    MonitorRecord& rec = monitorState.getOrCreate(id);
    if (rec.id.empty()) rec.id = id;
    // MonitorState は値が変わったときのみ dirty（文字列は内容比較）
    if (rec.value == v || rt::compareEqual(rec.value, v)) {
        if (!spriteName.empty()) rec.spriteName = spriteName;
        return;
    }
    rec.value = v;
    if (!spriteName.empty()) rec.spriteName = spriteName;
    monitorState.dirty = true;
}

void Runtime::requestAddMonitor(const MonitorRecord& rec) {
    MonitorRecord& dst = monitorState.getOrCreate(rec.id);
    // TW: 既存の位置/可視性を維持しつつ他フィールドを更新
    if (dst.id.empty()) dst = rec;
    else {
        std::string id = dst.id;
        double x = dst.x, y = dst.y;
        bool vis = dst.visible;
        dst = rec;
        dst.id = id;
        dst.x = x; dst.y = y; dst.visible = vis;
    }
    monitorState.dirty = true;
}

void Runtime::requestRemoveMonitor(const std::string& id) {
    monitorState.records.erase(id);
    monitorState.dirty = true;
}

void Runtime::requestRemoveMonitorByTargetId(const std::string& targetId) {
    for (auto it = monitorState.records.begin(); it != monitorState.records.end();) {
        if (it->second.targetId == targetId) it = monitorState.records.erase(it);
        else ++it;
    }
    monitorState.dirty = true;
}

// ------------------------------------------------------------------ misc --

void Runtime::visualReport(Target* target, const std::string& blockId, Value value) {
    if (onVisualReport) onVisualReport({target, blockId, value});
    lastVisualReport = {target, blockId, value};
}

void Runtime::emitKeyPressed(const std::u16string& scratchKey) {
    std::map<std::string, std::u16string> m1{{"KEY_OPTION", scratchKey}};
    startHats("event_whenkeypressed", &m1, nullptr);
    std::map<std::string, std::u16string> m2{{"KEY_OPTION", u"any"}};
    startHats("event_whenkeypressed", &m2, nullptr);
}

void Runtime::emitCompileError(Target* target, const std::u16string& error) {
    if (onCompileError) onCompileError(target, error);
}

void Runtime::onTargetGreenFlag(Target* target) {
    target->clearEffects();
}

// --------------------------------------------------------- ask and wait --

void Runtime::emitAnswer(const std::u16string& answerText) {
    answer = answerText;
    if (questionList.empty()) {
        if (onQuestion) onQuestion(nullptr);
        return;
    }
    AskEntry e = questionList.front();
    questionList.pop_front();
    if (e.wasVisible && !e.wasStage) e.target->setSay("say", u"");
    if (e.thread) resolveWait(*sequencer, *e.thread, Value::undefined());
    // _askNextQuestion — 次の質問を表示
    if (!questionList.empty()) {
        const AskEntry& q = questionList.front();
        if (q.wasVisible && !q.wasStage) {
            q.target->setSay("say", q.question);
            if (onQuestion) {
                static const std::u16string kEmpty;
                onQuestion(&kEmpty);
            }
        } else if (onQuestion) {
            onQuestion(&q.question);
        }
    } else if (onQuestion) {
        onQuestion(nullptr);
    }
}

void Runtime::clearAllQuestions() {
    questionList.clear();
    if (onQuestion) onQuestion(nullptr);
}

void Runtime::emitStopForTarget(Target* stopTarget, Thread*) {
    // _clearTargetQuestions — 対象の質問をキューから除去
    bool currentlyAsking =
        !questionList.empty() && questionList.front().target == stopTarget;
    for (auto it = questionList.begin(); it != questionList.end();) {
        if (it->target == stopTarget) it = questionList.erase(it);
        else ++it;
    }
    if (currentlyAsking) {
        stopTarget->setSay("say", u"");
        if (!questionList.empty()) {
            const AskEntry& q = questionList.front();
            if (q.wasVisible && !q.wasStage) {
                q.target->setSay("say", q.question);
                if (onQuestion) {
                    static const std::u16string kEmpty;
                    onQuestion(&kEmpty);
                }
            } else if (onQuestion) {
                onQuestion(&q.question);
            }
        } else if (onQuestion) {
            onQuestion(nullptr);
        }
    }
}

void Runtime::emitProjectStopAll() {
    clearAllQuestions();
    // stopAllSounds は soundBank 側（onProjectStopped 経由）
    for (Target* t : targets) {
        for (auto& [k, v] : t->soundEffects) v = 0;
    }
    if (onProjectStopped) onProjectStopped();
}

void Runtime::emitProjectStart() {
    answer.clear();
    for (Target* t : targets) {
        for (auto& [k, v] : t->soundEffects) v = 0;
    }
}

void Runtime::emitProjectRunStatus(int running) {
    projectRunStatus = running;
    if (onProjectRunStatus) onProjectRunStatus(running);
}

Variable* Runtime::createNewGlobalVariable(const std::u16string& name,
                                           const char* type,
                                           const std::string& id) {
    Target* stage = getTargetForStage();
    if (!stage) return nullptr;
    return stage->createVariable(id, name, type, false);
}

std::vector<std::u16string> Runtime::getAllVarNamesOfType(const char* type) const {
    std::vector<std::u16string> out;
    for (Target* t : targets)
        for (auto& [id, v] : t->variables)
            if (v.type == type) out.push_back(v.name);
    return out;
}

// ----------------------------------------------------------------- GC ルート --

namespace {

void markArgValues(Gc& g, const ArgValues& av) {
    for (auto& [k, v] : av.vals) g.markValue(v);
}

void markFieldMap(Gc& g, const std::map<std::string, BlockField>& fields) {
    for (auto& [k, f] : fields) g.markValue(f.value);
}

void markBlocks(Gc& g, Blocks* blocks) {
    if (!blocks) return;
    for (auto& [id, b] : blocks->allBlocks()) {
        markFieldMap(g, b.fields);
        if (b.mutation) {
            b.mutation->parseArgs();
            for (const Value& v : b.mutation->argumentDefaults) g.markValue(v);
        }
    }
    auto& c = blocks->cache();
    const auto markCached = [&](const BlockCached& bc) {
        markFieldMap(g, bc.fields);
        g.markValue(bc.shadowValue);
        markArgValues(g, bc.argValues);
        if (bc.ownedMutation) {
            bc.ownedMutation->parseArgs();
            for (const Value& v : bc.ownedMutation->argumentDefaults)
                g.markValue(v);
        }
    };
    for (const auto& [id, bc] : c.executeCached)
        markCached(*bc);
    // Native and interpreter frames may still use any pre-edit cache
    // generation. Their mutable argument values are GC roots until Blocks
    // itself is released, not merely until resetCache() invalidates lookup.
    for (const auto& retired : c.retiredExecuteCached)
        for (const auto& [id, bc] : retired)
            markCached(*bc);
    for (auto& [op, scripts] : c.scripts)
        for (auto& s : scripts)
            for (auto& [k, v] : s.fieldsOfInputs) g.markValue(v);
}

void markFrame(Gc& g, StackFrame& f) {
    for (auto& [k, v] : f.params) g.markValue(v);
    for (auto& e : f.reported) g.markValue(e.inputValue);
    if (f.executionContext) {
        for (auto& [k, v] : f.executionContext->vals) g.markValue(v);
        g.markValue(f.executionContext->returnValue);
    }
}

void markThread(Gc& g, Thread& t) {
    g.markValue(t.justReported);
    // ファイバ上に残るローカル（JIT の spill スロット・退避レジスタの
    // Value 等）は構造体ルートに無いため、ファイバスタック自体を走査する。
    if (t.fiber && !t.fiber->finished()) {
        void* base = t.fiber->stackBase();
        if (t.fiber == currentFiber()) {
            // 実行中ファイバ: この関数は collect の呼出側と同一スタック上
            // にあるため、現 sp から底までを走査する。
            uintptr_t lo = reinterpret_cast<uintptr_t>(__builtin_alloca(0));
            if (base && lo < reinterpret_cast<uintptr_t>(base))
                g.scanRange(reinterpret_cast<void*>(lo), base);
            // スケジューラ側の退避レジスタも走査
            auto* sr = &detail::gSchedulerRegs;
            g.scanRange(sr, reinterpret_cast<char*>(sr) + sizeof(*sr));
        } else {
            // 中断中ファイバ: 保存 sp から底まで + 退避済み callee-saved
            void* sp = t.fiber->suspendedSp();
            if (sp && base && sp < base) g.scanRange(sp, base);
            auto* r = t.fiber->regs();
            g.scanRange(r, reinterpret_cast<char*>(r) + sizeof(*r));
        }
    }
    for (auto& f : t.stackFrames) markFrame(g, *f);
    // trash は execute 中の参照生存期間のために残っている — 中身も生かす
    for (auto& f : t.trashFrames) markFrame(g, *f);
    if (t.hasCompatStackFrame) {
        for (auto& [k, v] : t.compatibilityStackFrame.vals) g.markValue(v);
        g.markValue(t.compatibilityStackFrame.returnValue);
    }
}

} // namespace

void Runtime::markRoots(Gc& g) {
    for (Target* t : targets) {
        for (auto& [id, v] : t->variables) {
            g.markValue(v.value);
            if (v.list) g.markPtr(v.list);
        }
        for (auto& [id, v] : t->edgeActivatedHatValues) g.markValue(v);
        markBlocks(g, t->blocks);
    }
    // Also tolerate null slots supplied by embedders or left by an older
    // reclamation pass; ownership compaction normally removes them now.
    for (auto& th : ownedThreads) if (th) markThread(g, *th);
    markBlocks(g, monitorBlocks.get());
    markBlocks(g, flyoutBlocks.get());
    for (auto& [id, rec] : monitorState.records) g.markValue(rec.value);
    g.markValue(lastVisualReport.value);
    for (auto& w : waitQueue) g.markValue(w.resolveValue);
}

} // namespace xyo
