// Backend-neutral JIT scheduler and compiled-script cache.
#include "jit.hpp"
#include "backend.hpp"
#include "aot_emit.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "../core/gc.hpp"
#include "../core/json.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/thread.hpp"
#include "../engine/numeric_thread.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../blocks/pen_api.hpp"
namespace xyo {

namespace {
/// AOT メタの hex 文字列（UTF-16 生バイト）をデコードする。
std::string hexDecode(const std::string& h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        const int a = nib(h[i]);
        const int b = nib(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(char((a << 4) | b));
    }
    return out;
}
} // namespace

/// runtime_helpers.cpp の ASCII 1 文字キャッシュ（AOT slot 束縛用）。
extern "C" SStr* const* xyo_ascii_cache();


struct JitEngine::Impl {
    std::unique_ptr<JitBackend> backend;
    std::deque<std::string> aotStrings;
    std::vector<void**> aotSlots;
    std::vector<std::unique_ptr<gpu::ParallelLoop>> aotGpuProgs;

    struct Work {
        const void* top = nullptr;
        uint64_t generation = 0;
        std::unique_ptr<PendingJitCompilation> task;
    };
    struct Result {
        const void* top = nullptr;
        uint64_t generation = 0;
        std::unique_ptr<CompiledScript> code;
    };
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Work> waiting;
    std::deque<Result> completed;
    std::thread worker;
    bool stopping = false;

    void enqueue(Work work) {
        {
            std::lock_guard lock(mutex);
            waiting.push_back(std::move(work));
            if (!worker.joinable())
                worker = std::thread([this] { runWorker(); });
        }
        available.notify_one();
    }
    void runWorker() {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || !waiting.empty(); });
                if (stopping) return;
                work = std::move(waiting.front());
                waiting.pop_front();
            }
            std::unique_ptr<CompiledScript> code = work.task->finish();
            work.task.reset();
            {
                std::lock_guard lock(mutex);
                completed.push_back({
                    work.top, work.generation, std::move(code)});
            }
        }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            waiting.clear();
        }
        available.notify_all();
        if (worker.joinable()) worker.join();
    }
};

JitEngine::JitEngine() : impl_(std::make_unique<Impl>()) {
    impl_->backend = createJitBackend();
}

namespace {

/// HotSpot のしきい値（環境変数で調整可能）。
struct HotParams {
    uint64_t invocations = 3;    // 呼び出し回数で JIT
    uint64_t blocks = 4096;      // 解釈実行したブロック数で JIT
    uint64_t size = 5000;        // 到達可能ブロック数がこれ以上なら初回 JIT
    bool loopCompile = true;     // ループを含むスクリプトは初回 JIT
    bool stats = false;          // 終了時にホットネス表を出力

    static uint64_t envU64(const char* n, uint64_t d) {
        const char* e = std::getenv(n);
        if (!e || !*e) return d;
        return std::strtoull(e, nullptr, 10);
    }
    static bool envOn(const char* n, bool d) {
        const char* e = std::getenv(n);
        if (!e) return d;
        return !(e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                 e[0] == 'f' || e[0] == 'F');
    }
    HotParams() {
        invocations = envU64("XYO_HOT_INVOCATIONS", invocations);
        blocks = envU64("XYO_HOT_BLOCKS", blocks);
        size = envU64("XYO_HOT_SIZE", size);
        loopCompile = envOn("XYO_HOT_LOOP", loopCompile);
        stats = envOn("XYO_HOT_STATS", stats);
    }
};
const HotParams& hotParams() { static HotParams p; return p; }

} // namespace

JitEngine::~JitEngine() {
    if (hotParams().stats && !profiles_.empty()) {
        struct Row {
            uint64_t inv, blocks;
            const std::string* id;
            bool jit, lazy, aot, loop;
        };
        std::vector<Row> rows;
        rows.reserve(profiles_.size());
        for (auto& [top, p] : profiles_) {
            auto cit = cache_.find(top);
            bool jit = false;
            bool aot = false;
            if (cit != cache_.end()) {
                aot = cit->second.aot;
                jit = cit->second.code != nullptr;
            }
            const bool lazy = jit && cit->second.code->lazy;
            rows.push_back({p.invocations, p.interpBlocks, &p.id,
                            jit, lazy, aot, p.hasLoop == 2});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.inv > b.inv ||
                   (a.inv == b.inv && a.blocks > b.blocks);
        });
        std::fprintf(stderr,
                     "[hot] %-32s %8s %10s %5s %5s %4s %s\n",
                     "script", "inv", "interp", "jit", "lazy", "aot", "loop");
        for (const Row& r : rows)
            std::fprintf(stderr, "[hot] %-32s %8llu %10llu %5s %5s %4d %d\n",
                         r.id ? r.id->c_str() : "?",
                         (unsigned long long)r.inv,
                         (unsigned long long)r.blocks,
                         r.jit ? "yes" : "-", r.lazy ? "yes" : "-",
                         int(r.aot), int(r.loop));
    }
    if (impl_) Gc::get().removeRootProvider(impl_.get());
    impl_.reset();
}

bool JitEngine::ok() const { return impl_ && impl_->backend && impl_->backend->ok(); }

uint8_t JitEngine::scanScriptLoop(Blocks& blocks, const Block* top,
                                  uint32_t* nodes) {
    if (nodes) *nodes = 0;
    if (!top) return 1;
    static const std::unordered_set<std::string> kLoops = {
        "control_forever", "control_repeat", "control_repeat_until",
        "control_while", "control_for_each",
    };
    std::unordered_set<const Block*> seen;
    std::vector<const Block*> work;
    auto push = [&](const Block* b) {
        if (b && seen.insert(b).second) work.push_back(b);
    };
    push(top);
    size_t guard = 0;
    while (!work.empty()) {
        // 走査不能な巨大グラフはループ扱い（= 初回 JIT）で安全側に倒す
        if (++guard > 200000) {
            if (nodes) *nodes = 200001;
            return 2;
        }
        const Block* b = work.back();
        work.pop_back();
        if (kLoops.count(b->opcode)) return 2;
        if (!b->next.empty()) push(blocks.getBlock(b->next));
        for (auto& kv : b->inputs.items) {
            const BlockInput& in = kv.second;
            if (!in.block.empty()) push(blocks.getBlock(in.block));
            if (!in.shadow.empty()) push(blocks.getBlock(in.shadow));
        }
        // 手続き呼び出しの本体も到達可能 — ホットループが手続き側に
        // あるスクリプトを取りこぼさない。
        if (b->opcode == "procedures_call" && b->mutation) {
            const std::string& pc = b->mutation->attr("proccode");
            if (!pc.empty()) {
                const std::string def = blocks.getProcedureDefinition(pc);
                if (!def.empty()) push(blocks.getBlock(def));
            }
        }
    }
    if (nodes) *nodes = uint32_t(guard);
    return 1;
}

bool JitEngine::decideCompile(Thread& thread, const Block* top) {
    const JitMode mode = jitMode();
    if (mode == JitMode::Off || !top) return false;

    auto it = cache_.find(top);
    if (it != cache_.end() && it->second.code) return true;  // 既に JIT/AOT
    if (failed_.count(top)) return false;

    ScriptProfile& p = profiles_[top];
    if (p.id.empty()) p.id = thread.topBlock;
    ++p.invocations;

    if (mode == JitMode::Eager) return true;

    if (p.hasLoop == 0 && thread.blockContainer)
        p.hasLoop = scanScriptLoop(*thread.blockContainer, top,
                                   &p.nodeCount);

    // ループを含むスクリプトは単発でも長時間走り得る。
    // OSR は明示有効な top-level repeat/条件付きループのみに限定
    // されるため、それ以外は従来通り初回 JIT する
    // （XYO_HOT_LOOP=0 で無効化できる）。巨大な直線スクリプトも
    // 解釈のまま一度だけ走らせると高くつくので同様に初回 JIT。
    const bool loopCompile = hotParams().loopCompile && p.hasLoop == 2;
    const bool large = p.nodeCount >= hotParams().size;
    const bool hot = p.invocations >= hotParams().invocations ||
                     p.interpBlocks >= hotParams().blocks;
    if (!loopCompile && !large && !hot) return false;

    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
                     "xyo-jit: hot %s (%s) inv=%llu blocks=%llu loop=%d\n",
                     thread.topBlock.c_str(), top->opcode.c_str(),
                     (unsigned long long)p.invocations,
                     (unsigned long long)p.interpBlocks,
                     int(p.hasLoop == 2));
    return true;
}

void JitEngine::noteInterpBlocks(const void* top, uint64_t blocks) {
    if (!top || !blocks) return;
    profiles_[top].interpBlocks += blocks;
}

bool JitEngine::scriptStats(const void* top, ScriptStats* out) const {
    if (!top || !out) return false;
    auto pit = profiles_.find(top);
    auto cit = cache_.find(top);
    if (pit == profiles_.end() && cit == cache_.end()) return false;
    out->invocations = pit != profiles_.end() ? pit->second.invocations : 0;
    out->interpBlocks = pit != profiles_.end() ? pit->second.interpBlocks : 0;
    out->hasLoop = pit != profiles_.end() && pit->second.hasLoop == 2;
    if (cit != cache_.end()) {
        out->aot = cit->second.aot;
        out->compiled = cit->second.code != nullptr;
        out->lazy = cit->second.code && cit->second.code->lazy;
    }
    return true;
}


const CompiledScript* JitEngine::readyScript(const Block* top) const {
    if (!top) return nullptr;
    auto it = cache_.find(top);
    return it == cache_.end() ? nullptr : it->second.code.get();
}

void JitEngine::prefetchWarpBroadcasts(Runtime& runtime) {
    // Unlike startup holdback, prefetch never postpones an observable
    // Scratch thread. The worker only compiles immutable, already captured
    // source. Opt-in pending cross-project performance verification.
    const char* flag = std::getenv("XYO_JIT_ASYNC_PREFETCH");
    if (!flag || (flag[0] != '1' && flag[0] != '2') ||
        jitMode() != JitMode::HotSpot || !runtime.renderer || !ok() ||
        jitLazyMode() == JitLazyMode::On)
        return;
    // 1: enqueue on the worker (fully interactive).
    // 2: compile the single highest-scoring receiver before starting any
    // Scratch scripts (a bounded *blocking* startup experiment).
    const bool synchronous = flag[0] == '2';
    const char* asyncFlag = std::getenv("XYO_JIT_ASYNC");
    if (asyncFlag && asyncFlag[0] == '0') return;

    struct Candidate {
        Target* target;
        const Block* top;
        std::string id;
        unsigned score;
    };
    std::vector<Candidate> candidates;

    // A one-shot, manually bounded scan. No walking dynamic reporters or
    // executing procedures, and no recursion into procedure call graphs.
    // We only prefetch scripts with an explicit warp procedure containing
    // BOTH a counted loop and a list mutation. A prime example is a CPU
    // emulator that initializes an entire list-backed RAM image.
    for (Target* target : runtime.targets) {
        if (!target || !target->blocks) continue;
        Blocks* blocks = target->blocks;
        for (const auto& script :
             blocks->getRuntimeScripts("event_whenbroadcastreceived")) {
            const Block* top = script.block;
            if (!top || cache_.count(top) || compiling_.count(top) ||
                failed_.count(top))
                continue;
            unsigned score = 0;
            const Block* call = blocks->getBlock(top->next);
            for (size_t chain = 0; call && chain < 128;
                 ++chain, call = blocks->getBlock(call->next)) {
                if (call->opcode != "procedures_call" || !call->mutation)
                    continue;
                const std::string& name = call->mutation->attr("proccode");
                const Mutation* prototype =
                    blocks->getProcedurePrototype(name);
                if (!prototype || !prototype->warp()) continue;
                const std::string definition =
                    blocks->getProcedureDefinition(name);
                const Block* def = blocks->getBlock(definition);
                if (!def) continue;
                std::vector<std::string> pending{def->next};
                std::unordered_set<std::string> seen;
                unsigned loops = 0, lists = 0;
                while (!pending.empty() && seen.size() < 256) {
                    std::string id = std::move(pending.back());
                    pending.pop_back();
                    if (id.empty() || !seen.insert(id).second) continue;
                    const Block* node = blocks->getBlock(id);
                    if (!node) continue;
                    loops += (node->opcode == "control_repeat");
                    lists += (node->opcode == "data_replaceitemoflist" ||
                              node->opcode == "data_addtolist");
                    if (!node->next.empty())
                        pending.push_back(node->next);
                    for (const auto& [_, input] : node->inputs.items) {
                        if (!input.block.empty())
                            pending.push_back(input.block);
                        if (!input.shadow.empty())
                            pending.push_back(input.shadow);
                    }
                }
                if (loops && lists)
                    score += loops * 2 + lists;
            }
            if (score)
                candidates.push_back({target, top, script.blockId, score});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });
    // Cap CPU and memory footprint. Other scripts retain normal JIT policy.
    size_t queued = 0;
    for (const Candidate& candidate : candidates) {
        if (queued == (synchronous ? 1u : 2u) ||
            compiling_.size() >= 32) break;
        if (cache_.count(candidate.top) ||
            compiling_.count(candidate.top)) continue;
        Thread temporary(candidate.id);
        temporary.target = candidate.target;
        temporary.blockContainer = candidate.target->blocks;
        temporary.topBlockPtr = candidate.top;
        const auto started = std::chrono::steady_clock::now();
        if (auto task = impl_->backend->prepareAsync(
                temporary, runtime, candidate.top, scriptCounter_++)) {
            if (synchronous) {
                // The prepared task contains a private ThreadSafeModule.
                // finish() runs only on the runtime thread in this mode.
                // Publish before greenFlag launches any Scratch thread:
                // no active Fiber, local argument or effect can be replayed.
                if (auto code = task->finish()) {
                    cache_[candidate.top].code = std::move(code);
                    ++queued;
                }
            } else {
                compiling_.insert(candidate.top);
                impl_->enqueue({candidate.top, asyncGeneration_,
                                std::move(task)});
                ++queued;
            }
            if (std::getenv("XYO_JIT_ASYNC_PREFETCH_LOG")) {
                const auto took = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                std::fprintf(stderr,
                    "[jit-prefetch] %s script=%s score=%u time=%.3fms\n",
                    synchronous ? "compiled" : "queued",
                    candidate.id.c_str(), candidate.score, took);
            }
        }
    }
}

void JitEngine::pollAsync() {
    if (!impl_) return;
    std::deque<Impl::Result> completed;
    {
        std::lock_guard lock(impl_->mutex);
        completed.swap(impl_->completed);
    }
    for (auto& item : completed) {
        // Never publish an object compiled for an invalidated block graph.
        if (item.generation != asyncGeneration_) continue;
        compiling_.erase(item.top);
        if (!item.code) {
            failed_.insert(item.top);
            continue;
        }
        const auto it = profiles_.find(item.top);
        item.code->shortSync =
            !item.code->mayYield && !item.code->mainThread &&
            it != profiles_.end() && it->second.hasLoop == 1 &&
            it->second.nodeCount > 0 && it->second.nodeCount <= 8;
        cache_[item.top].code = std::move(item.code);
        if (std::getenv("XYO_JIT_ASYNC_TIMING"))
            std::fprintf(stderr, "[jit-async] published %p\n", item.top);
    }
}

bool JitEngine::tryAttachNumeric(Thread& thread, const Block* top) {
    // Explicit experimentation flag. Avoid an additional source scan,
    // compilation, or code-cache lookup on the default JIT hot path.
    static const bool enabled = [] {
        const char* env = std::getenv("XYO_JIT_COMMON_CFG");
        return env && env[0] == '1' && env[1] == '\0';
    }();
    if (!enabled || (thread.target && thread.target->runtime &&
                     thread.target->runtime->blockCountEnabled) ||
        !ok() || !top || !thread.target ||
        !thread.blockContainer || thread.isCompiled ||
        thread.interpreterStarted || thread.numericSession ||
        thread.stackClick || thread.updateMonitor)
        return false;

    const auto* key = static_cast<const void*>(top);
    // An explicitly loaded native AOT artifact takes precedence over the
    // experimental source-to-common-CFG route.
    const auto existing = cache_.find(key);
    if (existing != cache_.end() && existing->second.aot)
        return false;
    auto cached = numericCache_.find(key);
    if (cached == numericCache_.end()) {
        if (numericRejected_.count(key)) return false;
        const ir::ScratchCompilationUnit source =
            ir::ScratchCompilationUnit::capture(*thread.blockContainer, top);
        auto lowered = ir::lowerNumericF64Script(
            source, ir::NumericF64LoweringOptions{false, true});
        if (!lowered || !lowered->requiresLoopScheduler ||
            lowered->executable.yieldBoundaries.empty() ||
            lowered->executable.yieldBoundaries.size() > 16) {
            if (std::getenv("XYO_JIT_VERBOSE"))
                std::fprintf(stderr, "xyo-common-cfg: source rejected script=%s valid=%d lowered=%d\n",
                    top->id.c_str(), int(source.valid()), int(bool(lowered)));
            numericRejected_.insert(key);
            return false;
        }
        auto kernel = std::make_shared<const ir::ScratchNumericKernel>(
            std::move(*lowered));
        if (!ir::NumericThreadSession::safeToAttach(thread, *kernel)) {
            if (std::getenv("XYO_JIT_VERBOSE"))
                std::fprintf(stderr, "xyo-common-cfg: slot guard rejected script=%s\n", top->id.c_str());
            // A failing variable-type guard may be target-specific: do not
            // poison the shared Blocks cache for its other clones.
            return false;
        }
        auto code = impl_->backend->compileNumeric(
            kernel, scriptCounter_++);
        if (!code || !code->run) {
            if (std::getenv("XYO_JIT_VERBOSE"))
                std::fprintf(stderr, "xyo-common-cfg: backend rejected script=%s\n", top->id.c_str());
            numericRejected_.insert(key);
            return false;
        }
        cached = numericCache_.emplace(key, std::move(code)).first;
    }
    // Each Scratch thread owns a separate continuation frame. The runner
    // points into backend-owned executable code (same contract as normal JIT).
    return ir::attachNumericThread(
        thread, cached->second->kernel, cached->second->run, false);
}

const CompiledScript* JitEngine::compile(Thread& thread, Runtime& runtime,
                                         Sequencer& sequencer,
                                         const Block* topHint) {
    (void)sequencer;
    if (!ok() || !thread.target) return nullptr;
    Blocks* blocks = thread.blockContainer;
    if (!blocks) return nullptr;
    const Block* top = topHint ? topHint : blocks->getBlock(thread.topBlock);
    if (!top) return nullptr;
    pollAsync();
    if (runtime.getIsEdgeActivatedHat(top->opcode)) {
        failed_.insert(top);
        return nullptr;
    }

    ScriptEntry& entry = cache_[top];
    if (entry.code) return entry.code.get();
    if (failed_.count(top)) return nullptr;

    // HotSpot can execute in the interpreter while LLVM optimizes and
    // materializes the immutable IR in a worker. Never let the worker
    // touch Thread/Runtime/Blocks or publish into the live code cache.
    // Interactive playback cannot block the render thread for hundreds
    // of milliseconds on the first appearance of a new script. Default to
    // background code generation when a renderer is attached. Keep explicit
    // XYO_JIT_ASYNC=0/1 overrides and retain synchronous behavior for CLI
    // benchmark/test runs without a renderer. OSR remains separately opt-in.
    const char* asyncSetting = std::getenv("XYO_JIT_ASYNC");
    const bool wantAsync = asyncSetting
        ? asyncSetting[0] == '1' : runtime.renderer != nullptr;
    const bool asyncEnabled = jitMode() == JitMode::HotSpot &&
        jitLazyMode() != JitLazyMode::On && wantAsync;
    if (asyncEnabled) {
        if (compiling_.count(top)) return nullptr;
        // Backpressure: do not retain an unbounded number of LLVM modules
        // when a project starts thousands of distinct event scripts.
        // The interpreter keeps running and a later spawn may retry.
        if (compiling_.size() >= 32) return nullptr;
        if (auto task = impl_->backend->prepareAsync(
                thread, runtime, top, scriptCounter_++)) {
            compiling_.insert(top);
            impl_->enqueue({top, asyncGeneration_, std::move(task)});
            return nullptr;
        }
    }

    const auto source = ir::ScratchCompilationUnit::capture(*blocks, top);
    if (!source.valid()) {
        failed_.insert(top);
        return nullptr;
    }
    auto code = impl_->backend->compile(
        thread, runtime, top, scriptCounter_++, source);
    if (!code) {
        failed_.insert(top);
        return nullptr;
    }
    // Scheduling policy is independent from the native code generator.
    const auto it = profiles_.find(top);
    code->shortSync = !code->mayYield && !code->mainThread &&
        it != profiles_.end() && it->second.hasLoop == 1 &&
        it->second.nodeCount > 0 && it->second.nodeCount <= 8;
    entry.code = std::move(code);
    return entry.code.get();
}

/// AOT オブジェクトをロードしてスクリプトをキャッシュへ登録する。
/// 生成コードは slot（xyo_aot_slot_N）経由でホスト資源を参照するため、
/// ここで実アドレスを書き込んでからルート登録する。
bool JitEngine::loadAot(Runtime& runtime, const void* object, size_t size,
                        uint64_t projectHash, std::string* why,
                        const aot::ProgressCallback& progress) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (!ok()) return fail("JIT unavailable");
    if (!object || size == 0) return fail("empty object");
    if (!impl_->aotSlots.empty()) return fail("AOT already loaded");

    aot::reportProgress(progress, aot::ProgressStage::LoadObject);
    if (!impl_->backend->addObjectFile(object, size))
        return fail("addObjectFile failed");
    auto* metaSym = static_cast<const char*>(impl_->backend->lookupSymbol("xyo_aot_meta"));
    if (!metaSym) return fail("xyo_aot_meta missing");
    const JsonValue meta = jsonParse(metaSym);
    if (!meta.isObject()) return fail("invalid AOT metadata");

    char hb[32];
    std::snprintf(hb, sizeof(hb), "%016llx",
                  (unsigned long long)projectHash);
    if (meta.get("hash").asString() != hb)
        return fail("project hash mismatch");

    // 文字列リテラル slot は途中で GC に晒されるため、SStr::create を
    // 呼ぶ前にルート登録しておく（未充填 slot は nullptr で無害）。
    Gc::get().addRootProvider(impl_.get(), [this](Gc& g) {
        if (!impl_) return;
        for (void** a : impl_->aotSlots) g.markPtr(*a);
    });

    const auto& slotEntries = meta.get("slots").asArray();
    size_t boundSlots = 0;
    aot::reportProgress(progress, aot::ProgressStage::BindSlots,
                        0, slotEntries.size());
    for (const JsonValue& sv : slotEntries) {
        const int kind = int(sv.get("k").asNumber());
        const std::string g = sv.get("g").asString();
        void** addr = static_cast<void**>(impl_->backend->lookupSymbol(g));
        if (!addr) return fail("AOT slot symbol missing");
        void* p = nullptr;
        switch (static_cast<AotSlotKind>(kind)) {
        case AotSlotKind::VarId:
        case AotSlotKind::Name:
            impl_->aotStrings.push_back(sv.get("s").asString());
            p = const_cast<char*>(impl_->aotStrings.back().c_str());
            break;
        case AotSlotKind::BlockCached: {
            // キーは "targetIndex\x01blockId"（block id はターゲット間で
            // 一意とは限らない）
            const std::string key = sv.get("s").asString();
            const size_t sep = key.find('\x01');
            const size_t ti =
                sep == std::string::npos
                    ? 0
                    : size_t(std::strtoul(key.substr(0, sep).c_str(),
                                          nullptr, 10));
            const std::string id =
                sep == std::string::npos ? key : key.substr(sep + 1);
            if (ti < runtime.targets.size()) {
                Blocks* b = runtime.targets[ti]->blocks;
                if (b && b->getBlock(id)) p = b->getCachedExecute(id);
            }
            if (!p) return fail("block for AOT slot not found");
            break;
        }
        case AotSlotKind::StrLit: {
            const std::string raw = hexDecode(sv.get("h").asString());
            std::u16string u(raw.size() / 2, u'\0');
            if (!u.empty())
                std::memcpy(u.data(), raw.data(), u.size() * 2);
            p = SStr::create(u);
            break;
        }
        case AotSlotKind::GpuProgram: {
            // キーは "targetIndex\x01loopBlockId"。AOT 時と同じ
            // 解析を再実行して Program を作り直す（決定論的）。
            // 失敗しても nullptr のまま — 生成コードは CPU ループへ。
            const std::string key = sv.get("s").asString();
            const size_t sep = key.find('\x01');
            const size_t ti =
                sep == std::string::npos
                    ? 0
                    : size_t(std::strtoul(key.substr(0, sep).c_str(),
                                          nullptr, 10));
            const std::string id =
                sep == std::string::npos ? key : key.substr(sep + 1);
            if (ti < runtime.targets.size()) {
                Blocks* b = runtime.targets[ti]->blocks;
                const Block* loop = b ? b->getBlock(id) : nullptr;
                if (loop) {
                    std::string gwhy;
                    if (auto pl =
                            gpu::compileParallelLoop(b, loop, &gwhy)) {
                        impl_->aotGpuProgs.push_back(
                            std::make_unique<gpu::ParallelLoop>(
                                std::move(*pl)));
                        p = &impl_->aotGpuProgs.back()->program;
                    } else if (std::getenv("XYO_AOT_VERBOSE")) {
                        std::fprintf(stderr,
                                     "xyo-jit: AOT GPU loop skip (%s): %s\n",
                                     id.c_str(), gwhy.c_str());
                    }
                }
            }
            break;
        }
        case AotSlotKind::AsciiCache:
            p = const_cast<void*>(
                static_cast<const void*>(xyo_ascii_cache()));
            break;
        case AotSlotKind::MovedHook:
            p = reinterpret_cast<void*>(penrt::movedHook());
            break;
        default:
            return fail("unknown AOT slot kind");
        }
        *addr = p;
        impl_->aotSlots.push_back(addr);
        aot::reportProgress(progress, aot::ProgressStage::BindSlots,
                            ++boundSlots, slotEntries.size());
    }

    const auto& scriptEntries = meta.get("scripts").asArray();
    size_t scannedScripts = 0;
    aot::reportProgress(progress, aot::ProgressStage::RegisterScripts,
                        0, scriptEntries.size());
    int nScripts = 0;
    for (const JsonValue& sj : scriptEntries) {
        const auto scanned = [&] {
            aot::reportProgress(progress, aot::ProgressStage::RegisterScripts,
                                ++scannedScripts, scriptEntries.size());
        };
        const std::string topId = sj.get("top").asString();
        auto* es = impl_->backend->lookupSymbol(sj.get("entry").asString());
        if (!es) return fail("AOT script symbol missing");
        auto cs = std::make_unique<CompiledScript>();
        cs->entry = reinterpret_cast<ScriptFn>(es);
        cs->mayYield = sj.get("mayYield").asBool();
        // Older objects lack affinity metadata (and may contain plain string
        // cache loads). Keep them serial; new objects also pin compat/GPU work.
        cs->mainThread = !sj.has("mainThread") ||
            sj.get("mainThread").asBool() || !impl_->aotGpuProgs.empty();
        for (const JsonValue& pj : sj.get("procs").asArray()) {
            auto* ps = impl_->backend->lookupSymbol(pj.at(1).asString());
            if (!ps) continue;
            cs->procedures[pj.at(0).asString()] = reinterpret_cast<ScriptFn>(ps);
        }
        const size_t ti = size_t(sj.get("ti").asNumber());
        if (ti >= runtime.targets.size()) {
            scanned();
            continue;
        }
        Target* owner = runtime.targets[ti];
        const Block* top = owner->blocks ? owner->blocks->getBlock(topId)
                                         : nullptr;
        if (!top) {
            scanned();       // skipped scripts still advance the denominator
            continue;
        }
        // AOT は全スクリプトを最適化済みで事前コンパイルしたもの —
        // 通常のコンパイルキャッシュへ登録し、再コンパイルしない。
        ScriptEntry& entry = cache_[top];
        entry.code = std::move(cs);
        entry.aot = true;
        ++nScripts;
        scanned();
    }

    std::fprintf(stderr, "aot: %d scripts precompiled (%zu slots)\n",
                 nScripts, impl_->aotSlots.size());
    if (std::getenv("XYO_AOT_VERBOSE"))
        std::fprintf(stderr, "xyo-jit: AOT loaded (%zu slots)\n",
                     impl_->aotSlots.size());
    aot::reportProgress(progress, aot::ProgressStage::Loaded);
    return true;
}

void JitEngine::invalidate() {
    ++asyncGeneration_;
    compiling_.clear();
    // Prepared jobs in the previous graph generation are safe to finish
    // (they own immutable IR) but must never be published to the new graph.
    // Pending, unstarted work can be dropped without waiting for LLVM.
    if (impl_) {
        std::lock_guard lock(impl_->mutex);
        impl_->waiting.clear();
        impl_->completed.clear();
    }
    cache_.clear();
    numericCache_.clear();
    numericRejected_.clear();
    profiles_.clear();
    failed_.clear();
    accessCache_.clear();
    ++accessGen_;   // Thread 側のキャッシュを無効化する
}

const AccessSet* JitEngine::accessFor(const Thread& thread,
                                      Runtime& runtime) {
    Target* t = thread.target;
    Blocks* blocks = thread.blockContainer;
    if (!t || !blocks) return nullptr;
    const Block* top = blocks->getBlock(thread.topBlock);
    if (!top) return nullptr;
    auto& perT = accessCache_[top];
    auto it = perT.find(t);
    if (it != perT.end()) return &it->second;
    AccessSet as = analyzeScript(runtime, *t, *blocks, top);
    auto [it2, ok] = perT.emplace(t, std::move(as));
    return &it2->second;
}

} // namespace xyo
