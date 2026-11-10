// xyo-cpp: ブロックグラフ → LLVM IR コード生成。
// jsgen.js + irgen.js の C++/LLVM 版。生成関数は void fn(JitCtx*)。
// 値は i64 の NaN ボックス bit パターンとして受け渡しする。
// 入力・スタック双方、未 lowering のものは compat 層へ落とす。
#include "jit.hpp"
#include "llvm_compat.hpp"
#include "aot_emit.hpp"
#include "parfor.hpp"
#include "../ir/numeric.hpp"
#include "../ir/diamond.hpp"
#include "../ir/loop.hpp"
#include "../ir/guard.hpp"
#include "../ir/slot_access.hpp"
#include "../ir/list_access.hpp"
#include "../ir/scratch_script.hpp"
#include "../ir/scratch_effect.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../ir/source_resume_phase.hpp"
#include "../ir/scratch_source_digest.hpp"

#include <llvm/IR/IRBuilder.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <llvm/IR/IntrinsicsX86.h>
#endif
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>

#include <bit>
#include <cstddef>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "../analysis/type_sites.hpp"
#include "../analysis/type_flow.hpp"
#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/jsnum.hpp"
#include "../core/v8math.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../gpu/gpu_compile.hpp"
#include "../engine/runtime.hpp"
#include "../engine/event_wait_dependency.hpp"
#include "../engine/thread.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

using namespace llvm;

namespace xyo {

/// runtime_helpers.cpp の ASCII 1 文字キャッシュ（letterOf fast path
/// が直接参照）。呼出しは遅延初期化を強制する — codegen 時に一度呼ぶ。
extern "C" SStr* const* xyo_ascii_cache();

namespace {

/// BB が終端命令で閉じているか（空 BB は未終端 — getTerminator は
/// 空リストで不定値を返すため使わない）
bool bbDone(BasicBlock* bb) {
    return bb && !bb->empty() && bb->back().isTerminator();
}

struct Codegen {
    // The new portable CallFrame is opt-in while the native Fiber remains
    // the authority for machine-instruction resumption. This experiment
    // must not slow down normal JIT code generation or remove GC roots.
    static bool portableCallFrames() {
        static const bool enabled = [] {
            const char* value = std::getenv("XYO_JIT_CALLFRAME");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    LLVMContext& lc;
    Module& mod;
    IRBuilder<> b;
    Runtime& runtime;
    JitPinned& pinned;
    // Persisted before lowering: codegen must not silently overwrite the
    // source revision with a newer live Blocks revision during a delayed
    // asynchronous compile.
    uint64_t compiledSourceRevision = 0;
    ir::ScratchSourceDigest compiledSourceDigest;

    // Compile-time only counters. They describe opportunities before LLVM's
    // middle-end; the optimized-IR/native-code counters are measured
    // independently by tests/bench/phi_demand_metrics.py.
    uint64_t phiMergeBlocks = 0;
    uint64_t phiDemandScans = 0;
    uint64_t phiCandidates = 0;
    uint64_t phiBoxedEmitted = 0;
    uint64_t phiBoxedSkipped = 0;
    uint64_t phiNumericEmitted = 0;
    uint64_t phiNumericSkipped = 0;

    Target* target;
    Blocks* blocks;
    /// AOT モード: ホストポインタを slot load に置き換える（null なら JIT）。
    AotEmit* aot = nullptr;
    bool fail = false;
    // Emit a plain local increment. LLVM can fold adjacent increments and
    // preserve them across SSA optimizations. Atomic synchronization is
    // needed only at fiber yields / completion, not per Scratch primitive.
    // Without counting enabled no instrumentation is emitted.
    void emitBlockCount(uint64_t amount = 1) {
        if (!runtime.blockCountEnabled || !amount) return;
        llvm::Value* slot = b.CreateConstGEP1_32(
            i8t, ctxArg, uint32_t(offsetof(JitCtx, pendingBlockCount)));
        llvm::Value* counter = b.CreateLoad(i64t, slot);
        b.CreateStore(b.CreateAdd(counter, ci64(amount)), slot);
    }

    struct ReporterCountGuard {
        Codegen& cg;
        bool active;
        ~ReporterCountGuard() { if (active) cg.emitBlockCount(); }
    };
    const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;
    // Compile-time A/B switch; no branch is added to generated hot loops.
    const bool boxedMotion = std::getenv("XYO_JIT_MOTION_BOXED") != nullptr;
    const bool genericEq = std::getenv("XYO_JIT_EQ_GENERIC") != nullptr;
    const bool slotReload = std::getenv("XYO_JIT_SLOT_RELOAD") != nullptr;
    const bool genericLiteralNum = std::getenv("XYO_JIT_LITERAL_NUM_GENERIC") != nullptr;
    const bool genericMathBox = std::getenv("XYO_JIT_MATH_BOX_GENERIC") != nullptr;
    const bool typeProfileDisabled = [] {
        const char* mode = std::getenv("XYO_JIT_TYPE_PROFILE");
        return mode && mode[0] == '0';
    }();
    // Compiled-code A/B switch; never read getenv in a generated hot path.
    const bool sharedWarpClock = [] {
        const char* env = std::getenv("XYO_JIT_WARP_CLOCK");
        return !env || env[0] != '0';
    }();
    // Inline Scratch daysSince2000 using the scheduler's 1ms wall clock.
    // Keep the synchronous helper for direct execution and A/B comparison.
    const bool sharedDaysClock = [] {
        const char* env = std::getenv("XYO_JIT_DAYS_SHARED_CLOCK");
        return env && env[0] != '0' && !std::getenv("XYO_PROF");
    }();
    const bool boxedDaysClock = [] {
        const char* env = std::getenv("XYO_JIT_DAYS_BOXED_CLOCK");
        return env && env[0] == '1';
    }();
    const bool safeArithCanon = [] {
        const char* mode = std::getenv("XYO_JIT_SAFE_ARITH_CANON");
        return !mode || mode[0] != '0';
    }();
    const bool splitModNan = [] {
        const char* mode = std::getenv("XYO_JIT_MOD_NAN_SPLIT");
        return !mode || mode[0] != '0';
    }();
    // Outline the cold variable-divisor fallback into a no-inline helper,
    // retaining the i32/i64/floating semantics without bloating the hot loop.
    // Setting this to zero restores the previous inline lowering for A/B.
    const bool modOutlineWide = [] {
        const char* mode = std::getenv("XYO_JIT_MOD_OUTLINE_WIDE");
        return !mode || mode[0] != '0';
    }();
    // Native IEEE roundeven with correct JS half-tie adjustment removes the
    // runtime helper call; retain a reference opt-out for differential tests.
    const bool inlineJsRound = [] {
        const char* mode = std::getenv("XYO_JIT_ROUND_INLINE");
        return !mode || mode[0] != '0';
    }();
    const bool integralFactsEnabled = [] {
        const char* mode = std::getenv("XYO_JIT_INTEGRAL_FACTS");
        return !mode || mode[0] != '0';
    }();
    // Prove bounded induction values instead of guessing from type feedback.
    const bool inductionRangesEnabled = [] {
        const char* mode = std::getenv("XYO_JIT_INDUCTION_RANGES");
        return !mode || mode[0] != '0';
    }();
    // Static bounded repeat counts can use an exact integral countdown.
    // No runtime guard is needed: all nonconstant or out-of-range counts
    // keep the existing IEEE double loop.
    const bool repeatIntegerCounter = [] {
        const char* mode = std::getenv("XYO_JIT_REPEAT_INT_COUNTER");
        return !mode || mode[0] != '0';
    }();
    const bool foreachIntegerCounter = [] {
        const char* mode = std::getenv("XYO_JIT_FOREACH_INT_COUNTER");
        return !mode || mode[0] != '0';
    }();
    const bool pureMathFacts = [] {
        const char* mode = std::getenv("XYO_JIT_PURE_MATH_FACTS");
        return !mode || mode[0] != '0';
    }();
    // The i32 modulo fast path is guarded to (0, INT32_MAX] (or
    // [1, INT32_MAX] for its divisor). In this domain signed and unsigned
    // conversions have identical exact results; signed conversion lowers
    // to native CVTTSD2SI without an unsigned-range fixup.
    const bool modSignedI32 = [] {
        const char* mode = std::getenv("XYO_JIT_MOD_SIGNED_I32");
        return !mode || mode[0] != '0';
    }();
    // Propagate finite f64 facts from arithmetic reporters (not only
    // literals/induction variables) into "change variable by" shadows.
    // Facts only refer to current SSA values and never cross a yield.
    const bool arithmeticDeltaFacts = [] {
        const char* mode = std::getenv("XYO_JIT_FINITE_ARITH_DELTA");
        return !mode || mode[0] != '0';
    }();
    // Optional interval transfer functions. Facts are attached to immutable
    // LLVM SSA doubles, never to mutable Scratch storage or type feedback.
    const bool arithmeticRangeFacts = [] {
        const char* mode = std::getenv("XYO_JIT_ARITH_RANGE_FACTS");
        return !mode || mode[0] != '0';
    }();


    // 手続き variantKey(proccode|warp) → 宣言済み Function
    std::map<std::string, Function*> procFns;
    std::string prefix;   // シンボル一意化用
    uint64_t procCounter = 0;

    // --- par-for（並列 append ループ）----------------------------------
    // 検出したループ本体は (ctx, iter, ctr, SList**, i64*) の別関数へ emit し、
    // 反復範囲をワーカープールで分割実行する（xyo_par_for）。
    struct ParForCtx {
        llvm::Value* lists = nullptr;   // SList** （append 先）
        llvm::Value* bases = nullptr;   // i64*   （各リストの開始 len）
        llvm::Value* iter = nullptr;    // i64    現在の反復番号
        std::unordered_map<std::string, int> index;  // LIST id → k
    };
    ParForCtx* parFor = nullptr;
    /// カウンタ変数を反復ローカルへ差し替える（VARIABLE id → alloca）
    std::unordered_map<std::string, llvm::AllocaInst*> varOverride;
    struct PendingParFor {
        Function* f = nullptr;
        jit::ParForPlan plan;
    };
    std::vector<PendingParFor> pendingParFors;
    std::vector<Function*> parFns;
    bool parForDisabled = std::getenv("XYO_PARFOR") != nullptr &&
                          std::getenv("XYO_PARFOR")[0] == '0';

    // --- ループ不変 pen setter のホイスト -------------------------------
    // warp 手続き内のループで、本体先頭に並ぶ pen 設定（サイズ/色）の引数が
    // ループ不変なら、ループの外へ 1 回だけ出す。本体からは除去する。
    // 「本体が 1 回も実行されない」ケースを保存するため、呼出し側で
    // 実行ガード（repeat なら trip>=1）の中に置く。
    std::unordered_set<Block*> hoistedSetters;
    bool curFnWarp = false;
    // Only the first top-level control_repeat of an OSR continuation.
    // TIMES must not be re-evaluated after interpreter initialization.
    const Block* osrStartLoop = nullptr;
    // Standalone native loop may read its still-live interpreter caller's
    // GC-rooted procedure arguments without evaluating a reporter block.
    bool directNestedOsrArgs = false;
    // Entry-only, read-only argument values. The interpreter procedure frame
    // remains the GC root even while these NaN-boxed SSA bits cross a yield.
    // An undefined value must take the original reporter path at each read.
    bool hoistNestedOsrArgs = false;
    // Optional bounded deopt only at a fully completed counted-loop body.
    bool nestedOsrInFlightGuard = false;
    // OSR-010: a source-proven 1..8 inner repeat must complete atomically.
    bool nestedOsrBoundedInner = false;
    // OSR-011: a source-proven 1..8192 inner repeat preserves real yields
    // and captures the two private counters at every handoff.
    bool nestedOsrInFlightInnerYield = false;
    std::map<std::u16string, llvm::Value*> hoistedNestedOsrArgs;
    /// ループ本体で書き込まれる変数/リスト（不変性判定用）
    struct LoopWriteScan {
        std::unordered_set<std::string> vars, lists;
        bool hasCall = false;
    };
    // 現在コンパイル中の手続きの引数名→index（argument reporter 用）
    std::map<std::u16string, int64_t> argIndex;
    // 手続き内なら仮引数の SSA 値（ctx=0、args=1..n）。argument
    // reporter はこれを直接返す — ctx->args 経由の load を省く。
    std::vector<llvm::Value*> curProcArgs;
    // procedures_return 用の遅延生成終了ブロック（関数単位）
    BasicBlock* exitBB = nullptr;

    // --- 変数/リストの高速アクセス -------------------------------------
    // id/name の文字列定数はモジュール内で一意化する —
    // Thread::jitVarCache は const char* のポインタ同一性キーのため、
    // 同じ id でも別 GlobalVariable だとキャッシュが全ミスになる。
    std::map<std::string, Constant*> idGlobals;
    std::map<std::u16string, Constant*> nameGlobals;
    /// 変数スロット idx（スクリプト単位 — proc fn 共有、同一 AOT Module 内でも別スクリプトとは非共有）。
    /// (id,isList) → idx。初出時に entryFn 先頭へ xyo_varslot_init を
    /// 挿入し、以後の使用は ctx->varSlotData[idx] の直接 load。
    /// clone は変数をコピーするので実行時 target 解決は維持しつつ、
    /// proc 呼出し毎の jitVar 再解決コストを除く。
    std::map<std::string, uint32_t> varSlotIdx;
    uint32_t numVarSlots = 0;
    Function* entryFn = nullptr;   // varslot init 挿入先のエントリ関数

    /// GPU 並列ループプログラムの所有（CompiledScript::gpuProgs へ
    /// emit 中に積む — 生成コードはポインタで参照する）。
    std::vector<std::unique_ptr<gpu::ParallelLoop>>* gpuProgs = nullptr;
    /// AOT emit では GPU プログラムをホスト側ヒープに持てないため
    /// jitEmitScript が true にする（env の XYO_GPU=0 と同義）。
    bool gpuDisabled =
        std::getenv("XYO_GPU") && std::getenv("XYO_GPU")[0] == '0';

    /// ループブロックが GPU 並列実行可能ならコンパイルして
    /// gpuProgs へ積み、所有付きポインタを返す（失敗 nullptr）。
    /// while/until では limitParent/limitInput に境界式が入る。
    const gpu::ParallelLoop* tryGpuLoopEx(Block* blk) {
        if (!gpuProgs || gpuDisabled || runtime.blockCountEnabled) return nullptr;
        const BlockInput* sub = blk->inputs.find("SUBSTACK");
        if (!sub || sub->block.empty()) return nullptr;
        std::string why;
        if (auto p = gpu::compileParallelLoop(blocks, blk, &why)) {
            gpuProgs->push_back(
                std::make_unique<gpu::ParallelLoop>(std::move(*p)));
            return gpuProgs->back().get();
        }
        if (verbose)
            std::fprintf(stderr, "xyo-jit: gpu loop skip: %s (%s %s)\n",
                         why.c_str(), blk->id.c_str(), blk->opcode.c_str());
        return nullptr;
    }

    /// ループ本体で書き込まれる変数/リストを集める（本体木全体。呼出しは hasCall）
    void scanLoopWrites(const std::string& firstId, LoopWriteScan& out) {
        std::vector<std::string> stack{firstId};
        std::unordered_set<std::string> seen;
        auto mark = [&](Block* b, const char* f) {
            auto it = b->fields.find(f);
            if (it != b->fields.end()) out.vars.insert(it->second.id);
        };
        while (!stack.empty()) {
            std::string id = std::move(stack.back());
            stack.pop_back();
            if (id.empty() || !seen.insert(id).second) continue;
            Block* b = blocks->getBlock(id);
            if (!b) continue;
            const std::string& op = b->opcode;
            if (op == "procedures_call") out.hasCall = true;
            if (op == "data_setvariableto" || op == "data_changevariableby" ||
                op == "control_for_each")
                mark(b, "VARIABLE");
            if (op == "data_addtolist" || op == "data_deleteoflist" ||
                op == "data_deletealloflist" || op == "data_insertatlist" ||
                op == "data_replaceitemoflist") {
                auto it = b->fields.find("LIST");
                if (it != b->fields.end()) out.lists.insert(it->second.id);
            }
            if (!b->next.empty()) stack.push_back(b->next);
            for (const auto& [n, in] : b->inputs.items) {
                (void)n;
                if (!in.block.empty()) stack.push_back(in.block);
                if (!in.shadow.empty()) stack.push_back(in.shadow);
            }
        }
    }

    /// 引数の式がループ不変か（本体が書き込む変数/リストを読まない・副作用なし）
    bool argInvariant(const BlockInput& in, const LoopWriteScan& w) {
        std::string id = !in.block.empty() ? in.block : in.shadow;
        if (id.empty()) return true;
        Block* b = blocks->getBlock(id);
        if (!b) return true;
        if (b->shadow) return true;   // 定数
        const std::string& op = b->opcode;
        if (op == "procedures_call" || op.rfind("sensing_", 0) == 0 ||
            op == "operator_random" || op == "procedures_return")
            return false;
        if (op == "data_variable") {
            auto it = b->fields.find("VARIABLE");
            return it == b->fields.end() || !w.vars.count(it->second.id);
        }
        if (op == "data_itemoflist" || op == "data_itemnumoflist" ||
            op == "data_lengthoflist" || op == "data_listcontainsitem" ||
            op == "data_listcontents") {
            auto it = b->fields.find("LIST");
            if (it != b->fields.end() && w.lists.count(it->second.id))
                return false;
        }
        // pen/motion はターゲット状態依存 — 保守側で不変扱いしない
        if (op.rfind("pen_", 0) == 0 || op.rfind("motion_", 0) == 0)
            return false;
        for (const auto& [n, in2] : b->inputs.items) {
            (void)n;
            if (!argInvariant(in2, w)) return false;
        }
        return true;
    }

    static bool hoistablePenSetter(const std::string& op) {
        return op == "pen_setPenSizeTo" || op == "pen_setPenColorToColor" ||
               op == "pen_setPenColorParamTo";
    }

    /// 本体先頭の連続する pen setter（引数がループ不変）を集める。
    /// warp 手続き内のみ（非 warp ループは他のスクリプトが間に走り得る）。
    std::vector<Block*> hoistableSetters(Block* loop) {
        std::vector<Block*> out;
        static const bool disabled = [] {
            const char* v = std::getenv("XYO_PENHOIST");
            return v && v[0] == '0';
        }();
        if (disabled || runtime.blockCountEnabled || !curFnWarp || !blocks) return out;
        const BlockInput* sub = loop->inputs.find("SUBSTACK");
        if (!sub || sub->block.empty()) return out;
        LoopWriteScan w;
        scanLoopWrites(sub->block, w);
        if (w.hasCall) return out;   // 呼出し越しの不変性は保証しない
        std::string id = sub->block;
        while (!id.empty()) {
            Block* b = blocks->getBlock(id);
            if (!b || !hoistablePenSetter(b->opcode)) break;
            bool ok = true;
            for (const auto& [n, in] : b->inputs.items) {
                (void)n;
                if (!argInvariant(in, w)) { ok = false; break; }
            }
            if (!ok) break;
            out.push_back(b);
            id = b->next;
        }
        return out;
    }

    /// ホイストした setter を、本体が 1 回以上実行されるガード内で emit する
    /// （guard が null なら無条件 — forever 用）。
    void emitHoistedSetters(const std::vector<Block*>& hs, llvm::Value* guard) {
        if (hs.empty() || runtime.blockCountEnabled) return;
        BasicBlock* runBB = nullptr, *afterBB = nullptr;
        if (guard) {
            runBB = BasicBlock::Create(lc, "", fn);
            afterBB = BasicBlock::Create(lc, "", fn);
            b.CreateCondBr(guard, runBB, afterBB);
            b.SetInsertPoint(runBB);
        }
        emitChainBlocks(hs);                       // 先に emit
        for (Block* h : hs) hoistedSetters.insert(h);   // 本体からは除去
        if (guard) {
            if (!bbDone(b.GetInsertBlock())) b.CreateBr(afterBB);
            b.SetInsertPoint(afterBB);
        }
    }

    const gpu::Program* tryGpuLoop(Block* blk) {
        const gpu::ParallelLoop* pl = tryGpuLoopEx(blk);
        return pl ? &pl->program : nullptr;
    }

    /// par-for の最小反復数（これ未満は逐次ループの方が速い）
    // Minimum trip count for the fused append batch path. The runtime uses
    // a separate, higher XYO_PARFOR_PARALLEL_MIN threshold for *workers*;
    // short eligible batches run locally without task dispatch. This retains
    // the faster fast-path for 1000-item Scratch append loops.
    static int64_t parForMinTrip() {
        static const int64_t v = [] {
            const char* s = std::getenv("XYO_PARFOR_MIN");
            int64_t r = s ? std::strtoll(s, nullptr, 10) : 512;
            return r > 0 ? r : int64_t(512);
        }();
        return v;
    }

    /// 「カウンタ + append」ループを並列実行する高速パスを emit する。
    /// 失敗（xyo_par_for が 0 を返した）ときは fallback（逐次ループの head）へ。
    /// リストは反復順に slot を書くので、最終内容は逐次と完全に一致する。
    bool tryEmitParFor(Block* blk, llvm::Value* n, BasicBlock* fallback,
                       BasicBlock* merge) {
        if (parForDisabled || runtime.blockCountEnabled || aot || !blocks) return false;
        std::string why;
        auto planOpt = jit::analyzeParFor(*blocks, blk, &why);
        if (!planOpt) {
            if (verbose)
                std::fprintf(stderr, "xyo-jit: parfor skip (%s): %s\n",
                             blk->id.c_str(), why.c_str());
            return false;
        }
        jit::ParForPlan& plan = *planOpt;
        const uint32_t k = uint32_t(plan.lists.size());

        // append 先の SList* を配列へ。1 つでも null（未生成）なら逐次へ。
        AllocaInst* listsArr = entryAlloca(ptrt, k, "pf_lists");
        AllocaInst* basesArr = entryAlloca(i64t, k, "pf_bases");
        llvm::Value* allNonNull = b.getInt1(true);
        for (uint32_t i = 0; i < k; ++i) {
            llvm::Value* lp = varListPtr(plan.lists[i]);
            b.CreateStore(lp, b.CreateGEP(ptrt, listsArr, ci64(i)));
            allNonNull = b.CreateAnd(
                allNonNull,
                b.CreateICmpNE(b.CreatePtrToInt(lp, i64t), ci64(0)));
        }

        // trip = round(n)（逐次ループの i>=0.5 判定と同値）+ 範囲ガード。
        llvm::Value* inRange = b.CreateFCmpOLE(n, cf64(9.0e15));
        llvm::Value* usable =
            b.CreateAnd(b.CreateFCmpOGE(n, cf64(0.5)), inRange);
        llvm::Value* safeN = b.CreateSelect(usable, n, cf64(0.0));
        llvm::Value* trip =
            b.CreateFPToSI(b.CreateFAdd(safeN, cf64(0.5)), i64t);
        llvm::Value* cond = b.CreateAnd(
            b.CreateICmpNE(loadIsWarp(), ci32(0)),
            b.CreateAnd(allNonNull,
                        b.CreateAnd(inRange,
                                    b.CreateICmpSGE(trip,
                                                    ci64(parForMinTrip())))));

        BasicBlock* prep = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(cond, prep, fallback);
        b.SetInsertPoint(prep);
        // 開始 len を並列領域の前に確定（ワーカーは len/cap を触らない）
        for (uint32_t i = 0; i < k; ++i) {
            llvm::Value* lp =
                b.CreateLoad(ptrt, b.CreateGEP(ptrt, listsArr, ci64(i)));
            llvm::Value* len = b.CreateZExt(
                b.CreateLoad(i32t, b.CreateConstGEP1_32(i8t, lp, kListLenOff)),
                i64t);
            b.CreateStore(len, b.CreateGEP(i64t, basesArr, ci64(i)));
        }

        // 本体関数 void(JitCtx*, iter, ctr, SList**, i64*) を宣言し、
        // 実体は emitPendingParFors で埋める（呼出し側の emit と独立）。
        std::vector<Type*> ps{ptrt, i64t, f64t, ptrt, ptrt};
        Function* bf = Function::Create(
            FunctionType::get(voidt, ps, false), Function::InternalLinkage,
            prefix + "_pf" + std::to_string(procCounter++), &mod);
        parFns.push_back(bf);
        pendingParFors.push_back({bf, plan});

        llvm::Value* initF = emitToNumF64(emitVarGet(plan.counterField));
        llvm::Value* ok = callN(
            "xyo_par_for", i32t,
            {ctxArg, trip, initF, cf64(plan.delta), listsArr, basesArr,
             ci64(k), bf});
        BasicBlock* okBB = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(b.CreateICmpNE(ok, ci32(0)), okBB, fallback);
        b.SetInsertPoint(okBB);
        // カウンタの書き戻し（整数域なので厳密）
        llvm::Value* fin = b.CreateFAdd(
            initF, b.CreateFMul(b.CreateSIToFP(trip, f64t), cf64(plan.delta)));
        emitVarSet(plan.counterField, emitCanon(fin));
        b.CreateBr(merge);
        return true;
    }

    // helpers ------------------------------------------------------------
    Type* i64t = Type::getInt64Ty(lc);
    Type* i32t = Type::getInt32Ty(lc);
    Type* i16t = Type::getInt16Ty(lc);
    Type* i8t = Type::getInt8Ty(lc);
    Type* f64t = Type::getDoubleTy(lc);
    Type* voidt = Type::getVoidTy(lc);
    Type* ptrt = PointerType::get(lc, 0);

    Constant* ci64(uint64_t v) { return ConstantInt::get(i64t, v); }
    Constant* ci32(int32_t v) { return ConstantInt::get(i32t, v); }
    Constant* ci8(uint8_t v) { return ConstantInt::get(i8t, v); }
    Constant* cf64(double v) { return ConstantFP::get(f64t, v); }
    AllocaInst* entryAlloca(Type* type, uint32_t count, const char* name) {
        IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        return eb.CreateAlloca(type, ci32(count), name);
    }

    // --- Target/Runtime/PenState のフィールドオフセット ---
    // 非標準レイアウト型のため offsetof 不可 — 実インスタンス上の
    // アドレス差から算出する（codegen 時計算・生成コードコスト 0）。
    struct Layout {
        uint32_t x = 0, y = 0, isStage = 0, dragging = 0, penState = 0,
                 moved = 0, rtRen = 0, rtRefresh = 0;
        uint32_t psDown = 0, psColor = 0, psSat = 0, psBright = 0,
                 psTransp = 0, psShade = 0, psRgba = 0, psDirty = 0,
                 psLast = 0;
        uint32_t mseDown = 0;
        bool ok = false;
    };
    Layout layout_{};
    const Layout& ly() {
        if (!layout_.ok) {
            Target* t = target;
            Runtime* r = &runtime;
            static PenState ps;
            auto d = [](const void* m, const void* b) {
                return uint32_t(
                    (const char*)m - (const char*)b);
            };
            layout_ = Layout{
                d(&t->x, t), d(&t->y, t), d(&t->isStage, t),
                d(&t->dragging, t), d(&t->penState, t),
                d(&t->onTargetMoved, t),
                d(&r->renderer, r), d(&r->refreshTargets, r),
                d(&ps.penDown, &ps), d(&ps.color, &ps),
                d(&ps.saturation, &ps), d(&ps.brightness, &ps),
                d(&ps.transparency, &ps), d(&ps.shade, &ps),
                d(ps.rgba, &ps), d(&ps.colorDirty, &ps),
                d(&ps.lastSetColor, &ps),
                d(r->ioDevices.mouse.isDownPtr(), r), true};
        }
        return layout_;
    }

    /// ctx の idx 番目ポインタフィールド（JitCtx は先頭が
    /// thread/target/runtime/sequencer/fiber のポインタ列）
    llvm::Value* ctxPtr(int idx) {
        // JitCtx begins with a pointer array. Use the target pointer size, not i64.
        return b.CreateLoad(ptrt, b.CreateConstGEP1_32(ptrt, ctxArg, idx));
    }
    llvm::Value* fld(llvm::Value* base, uint32_t off) {
        return b.CreateConstGEP1_32(i8t, base, off);
    }
    Constant* cnull() { return Constant::getNullValue(ptrt); }
    BasicBlock* newBB(const char* n) {
        return BasicBlock::Create(lc, n, fn);
    }
    Constant* cptr(const void* p) {
        return ConstantExpr::getIntToPtr(ci64(reinterpret_cast<uint64_t>(p)),
                                         ptrt);
    }

    /// ホストポインタ定数。AOT ではモジュール内グローバル slot の
    /// load に置き換え、ロード時に実アドレスを書き込む
    /// （JIT では従来どおり即値）。bb は load の挿入位置 —
    /// entry の varslot init から呼ぶ場合は entry 用 builder を渡す
    /// （dominance を守る）。
    llvm::Value* slotPtrAt(AotSlotKind kind, const std::string& key,
                           const void* host, IRBuilder<>& bb) {
        if (!aot) return cptr(host);
        const std::string& gn = aot->slot(kind, key);
        GlobalVariable* g = mod.getGlobalVariable(gn);
        if (!g) {
            g = new GlobalVariable(
                mod, PointerType::get(lc, 0), false,
                GlobalValue::ExternalLinkage,
                ConstantPointerNull::get(PointerType::get(lc, 0)), gn);
        }
        return bb.CreateLoad(PointerType::get(lc, 0), g);
    }
    llvm::Value* slotPtr(AotSlotKind kind, const std::string& key,
                         const void* host) {
        return slotPtrAt(kind, key, host, b);
    }

    /// GPU 並列ループの Program ポインタ。JIT では即値、AOT では
    /// ロード時に再コンパイルした Program を slot から load する
    /// （ホストの Program* は別プロセスでは無効なため）。
    llvm::Value* gpuProgPtr(const gpu::Program* p,
                            const std::string& loopBlockId) {
        return slotPtr(AotSlotKind::GpuProgram, bcKey(loopBlockId), p);
    }

    /// BlockCached slot のキー。block id はターゲット間で一意とは
    /// 限らないため、ターゲット index を前置する。
    std::string bcKey(const std::string& blockId) const {
        std::string k = std::to_string(aot ? aot->targetIndex : 0);
        k.push_back('\x01');
        k += blockId;
        return k;
    }

    /// SStr* リテラルを NaN-boxed bits にする。AOT では slot から
    /// load したポインタにタグを付ける（リテラルはロード時に intern）。
    llvm::Value* strLitBits(const std::u16string& u, SStr* host) {
        if (!aot) {
            if (host) pinned.strings.push_back(host);
            return ci64(Value::string(host).bits);
        }
        const std::string key(reinterpret_cast<const char*>(u.data()),
                              u.size() * 2);
        llvm::Value* p = slotPtr(AotSlotKind::StrLit, key, host);
        return b.CreateOr(b.CreatePtrToInt(p, i64t), ci64(val::kTagStr));
    }

    /// UTF-16 文字列定数グローバル（変数名等）
    Constant* u16g(const std::u16string& s) {
        std::vector<uint16_t> d(s.begin(), s.end());
        d.push_back(0);
        Constant* arr = ConstantDataArray::get(lc, d);
        auto* g = new GlobalVariable(mod, arr->getType(), true,
                                     GlobalValue::PrivateLinkage, arr, "u16");
        return g;
    }

    bool mainThread = false;
    FunctionCallee decl(const char* name, Type* ret,
                        ArrayRef<Type*> args) {

        // Compat/reporters may mutate shared execution caches or invoke
        // registered primitives that static opcode memory keys cannot model.
        if (std::string_view(name).starts_with("xyo_rt_")) mainThread = true;
        // Clone creation and deletion mutate the live scheduler/Target
        // arrays. The dependency analyzer already rejects parallel
        // execution for these opcodes (AccessSet::spawn), but it would
        // otherwise analyze and cache the same invalid plan separately
        // for every short-lived clone Target. Marking caller affinity at
        // compile time avoids those O(clones) analyses/cache entries.
        // Enabled by default after parity and paired benchmark validation.
        // Set XYO_CLONE_SERIAL_CODEGEN=0 for the historical analysis path.
        static const bool cloneSerialHint = [] {
            const char* v = std::getenv("XYO_CLONE_SERIAL_CODEGEN");
            return !v || v[0] != '0';
        }();
        if (cloneSerialHint &&
            (std::string_view(name) == "xyo_ctl_createclone" ||
             std::string_view(name) == "xyo_ctl_deleteclone"))
            mainThread = true;
        return mod.getOrInsertFunction(
            name, FunctionType::get(ret, args, false));
    }

    Function* fn = nullptr;
    llvm::Value* ctxArg = nullptr;

    /// スクリプト内に yield 点が存在するか（同期実行可否判定）
    /// 関数ごとの yield 点（直接呼ぶ yield 可能ヘルパ）—
    /// 手続き呼出しは callEdges で伝播計算するため、スクリプト全体の
    /// mayYield は entry からの到達集合で決まる。
    std::unordered_map<llvm::Function*, bool> fnDirect;
    std::unordered_map<llvm::Function*, std::vector<llvm::Function*>>
        callEdges;
    /// 内部で fiber yield し得る extern "C" ヘルパ。
    /// rt.cpp/rt_blocks.cpp 内の xyo_yield 呼出しと対応（漏れは
    /// fiber=nullptr での同期実行を壊すため新規 yield 追加時は必ず更新）。
    static bool helperMayYield(const char* n) {
        static const std::unordered_set<std::string_view> k = {
            "xyo_yield", "xyo_yield_fast", "xyo_loop_yield",
            "xyo_wait_yield", "xyo_wait_event_yield", "xyo_rt_wait", "xyo_stop_all",
            "xyo_stop_script", "xyo_broadcast_wait_v", "xyo_rt_compat",
            "xyo_rt_reporter", "xyo_rt_nested_osr_arg_get",
            "xyo_mot_glidexy", "xyo_mot_glideto",
            "xyo_looks_saysecs", "xyo_looks_thinksecs",
            "xyo_looks_switchbackdropwait", "xyo_sns_askwait",
            "xyo_ctl_deleteclone", "xyo_snd_playwait",
            "xyo_snd_seteffect", "xyo_snd_changeeffect",
            "xyo_snd_setvolume", "xyo_snd_changevolume",
        };
        return k.count(n) != 0;
    }
    void noteCall(const char* n) {
        if (helperMayYield(n) && fn) fnDirect[fn] = true;
    }

    llvm::Value* call1(const char* n, Type* ret, llvm::Value* a) {
        noteCall(n);
        return b.CreateCall(decl(n, ret, {a->getType()}), {a});
    }
    llvm::Value* call2(const char* n, Type* ret, llvm::Value* a, llvm::Value* c) {
        noteCall(n);
        return b.CreateCall(decl(n, ret, {a->getType(), c->getType()}),
                            {a, c});
    }
    llvm::Value* call3(const char* n, Type* ret, llvm::Value* x, llvm::Value* y, llvm::Value* z) {
        noteCall(n);
        return b.CreateCall(
            decl(n, ret, {x->getType(), y->getType(), z->getType()}),
            {x, y, z});
    }
    llvm::Value* callN(const char* n, Type* ret,
                       std::initializer_list<llvm::Value*> args) {
        noteCall(n);
        SmallVector<Type*, 8> ts;
        SmallVector<llvm::Value*, 8> vs;
        for (llvm::Value* a : args) { ts.push_back(a->getType()); vs.push_back(a); }
        return b.CreateCall(decl(n, ret, ArrayRef<Type*>(ts)), vs);
    }

    /// blk の入力を左→右の順で逐次 emit してヘルパを呼ぶ。
    /// C++ 引数評価順は未規定のため emitInputOf を call の引数式に
    /// 直接書くと IR 生成順が逆転し得る（副作用を持つ reporter
    /// 入力 — 手続き呼出し等 — の順序が壊れる）。
    template<typename... Names>
    llvm::Value* callOp(const char* n, Type* ret, Block* blk, Names... names) {
        SmallVector<llvm::Value*, 4> vs;
        SmallVector<Type*, 4> ts;
        (vs.push_back(emitInputOf(blk, names)), ...);
        for (llvm::Value* v : vs) ts.push_back(v->getType());
        noteCall(n);
        return b.CreateCall(decl(n, ret, ArrayRef<Type*>(ts)),
                            ArrayRef<llvm::Value*>(vs));
    }
    /// ctxArg 先頭版（xyo_random 等）
    template<typename... Names>
    llvm::Value* callOpCtx(const char* n, Type* ret, Block* blk, Names... names) {
        SmallVector<llvm::Value*, 5> vs;
        SmallVector<Type*, 5> ts;
        vs.push_back(ctxArg);
        (vs.push_back(emitInputOf(blk, names)), ...);
        for (llvm::Value* v : vs) ts.push_back(v->getType());
        noteCall(n);
        return b.CreateCall(decl(n, ret, ArrayRef<Type*>(ts)),
                            ArrayRef<llvm::Value*>(vs));
    }

    /// フィールドの変数 id → C 文字列ポインタ（id 毎に一意化 —
    /// jitVarCache のポインタ同一性ヒットに必須）。AOT では slot。
    llvm::Value* varIdCAt(const BlockField* f, IRBuilder<>& bb) {
        const std::string key = f ? f->id : "";
        if (aot)
            return slotPtrAt(AotSlotKind::VarId, key,
                             f ? f->id.c_str() : "", bb);
        auto [it, ins] = idGlobals.emplace(key, nullptr);
        // ポインタは Blocks 所有の field 文字列を指す必要がある
        // （map キーは Codegen 破棄で腐るため使えない）
        if (ins) it->second = cptr(f ? f->id.c_str() : "");
        return it->second;
    }
    llvm::Value* varIdC(const BlockField* f) { return varIdCAt(f, b); }
    /// フィールドの変数名 → UTF-16 グローバル（名毎に一意化）
    Constant* varNameU16(const BlockField* f) {
        std::u16string n;
        if (f && f->value.isString() && f->value.asStrPtr())
            n = std::u16string(f->value.asStrPtr()->view());
        auto [it, ins] = nameGlobals.emplace(n, nullptr);
        if (ins) it->second = u16g(it->first);
        return it->second;
    }

    /// 分岐重み (branch_weights) を付ける。LLVM のブロック配置が
    /// hot 側を fall-through にし、cold 側を関数末尾へ追い出す。
    void setWeights(BranchInst* br, uint32_t hot, uint32_t cold) {
        LLVMContext& c = br->getContext();
        Metadata* md[3] = {
            MDString::get(c, "branch_weights"),
            ConstantAsMetadata::get(ConstantInt::get(i32t, hot)),
            ConstantAsMetadata::get(ConstantInt::get(i32t, cold))};
        br->setMetadata(LLVMContext::MD_prof, MDNode::get(c, md));
    }
    BranchInst* hotBr(llvm::Value* cond, BasicBlock* hotBB, BasicBlock* coldBB,
                      uint32_t hotW = 1000, uint32_t coldW = 1) {
        BranchInst* br = b.CreateCondBr(cond, hotBB, coldBB);
        setWeights(br, hotW, coldW);
        return br;
    }
    /// ループのバックエッジに llvm.loop.unroll.count を付ける。
    /// （LLVM の既定ヒューリスティックでは、分岐の多いループは
    ///   アンロールされないため明示する。）
    void markUnroll(BranchInst* br, uint32_t count) {
        LLVMContext& c = br->getContext();
        Metadata* propOps[2] = {
            MDString::get(c, "llvm.loop.unroll.count"),
            ConstantAsMetadata::get(ConstantInt::get(i32t, count))};
        MDNode* prop = MDNode::get(c, propOps);
        Metadata* idOps[2] = {MDString::get(c, "llvm.loop"), prop};
        MDNode* loopID = MDNode::getDistinct(c, idOps);
        loopID->replaceOperandWith(0, loopID);
        br->setMetadata(LLVMContext::MD_loop, loopID);
    }

    /// llvm.fptosi.sat.i64.f64 — NaN/±Inf/範囲外で未定義動作にならず
    /// INT64_MIN/MAX に飽和する。リスト索引の整数境界検査に使う。
    llvm::Value* satToI64(llvm::Value* d) {
        Function* f = llvmc::intrinsicDeclaration(
            b.GetInsertBlock()->getModule(), Intrinsic::fptosi_sat,
            {i64t, f64t});
        return b.CreateCall(f, {d});
    }

    llvm::Value* boolToValue(llvm::Value* i32) {
        // i32 truth → kTrue/kFalse ビット
        llvm::Value* t = b.CreateICmpNE(i32, ci32(0));
        return b.CreateSelect(t, ci64(val::kTrue), ci64(val::kFalse));
    }

    // JitCtx レイアウト（64bit 単位）:
    //  0 thread 1 target 2 runtime 3 sequencer 4 fiber
    //  5 args   6 numArgs 7 warpStartMs 8 isWarp+stopped(i32x2)
    //  9 reportOut 10 compatBranch+isLoop 11 compatEc
    // 12 varSlotData 13 yieldCheckN
    /// 1.0 の IEEE-754 ビット表現（整数境界検査の下限）。
    static constexpr uint64_t kBitsOne = 0x3FF0000000000000ULL;
    static constexpr uint32_t kYieldCheckNOff =
        uint32_t(offsetof(JitCtx, yieldCheckN));

    /// ループ末尾の yield 点。non-warp は直接 xyo_yield(_fast)。
    /// warp は 500ms stuck 窓の時計読みを 64 回に 1 回へ間引く
    /// （判定遅延 << 500ms）。xyo_loop_yield 内で isWarp/isStuck を
    /// 再確認するので意味論は不変。
    void beginLoopYieldScope(const Block* sourceLoop) {
        yieldStack.push_back({curYieldCnt, curYieldWarp, curYieldSourceLoop,
                              curYieldRemainingF64, curYieldRemainingI32});
        curYieldSourceLoop = sourceLoop;
        curYieldRemainingF64 = nullptr;
        curYieldRemainingI32 = nullptr;
        curYieldCnt = entryAlloca(i64t, 1, "ycnt");
        b.CreateStore(
            b.CreateLoad(i64t,
                         b.CreateConstGEP1_32(i8t, ctxArg, kYieldCheckNOff)),
            curYieldCnt);
        curYieldWarp = loadIsWarp();
    }
    /// storeBack: ループが通常脱出するなら ctx へ書き戻す（global 位相を保つ）。
    void endLoopYieldScope(bool storeBack) {
        if (storeBack && curYieldCnt)
            b.CreateStore(b.CreateLoad(i64t, curYieldCnt),
                          b.CreateConstGEP1_32(i8t, ctxArg, kYieldCheckNOff));
        YieldScope s = yieldStack.back();
        yieldStack.pop_back();
        curYieldCnt = s.cnt;
        curYieldWarp = s.warp;
        curYieldSourceLoop = s.sourceLoop;
        curYieldRemainingF64 = s.remainingF64;
        curYieldRemainingI32 = s.remainingI32;
    }
    // Only active while lowering a proven self-append-only warp repeat.
    // The log remains GC-rooted by the immutable prefix snapshot in var.
    AllocaInst* activeTransient = nullptr;
    llvm::Value* activeTransientVariable = nullptr;
    // Additional live append logs in a simultaneous multi-variable loop.
    // Every log is rooted via its own published variable snapshot.
    std::vector<std::pair<AllocaInst*, llvm::Value*>> activeExtraTransients;
    void transientBeforeHandoff() {
        if (!activeTransient) return;
        llvm::Value* log = b.CreateLoad(ptrt, activeTransient);
        call2("xyo_transient_flush", voidt, log, activeTransientVariable);
        for (const auto& extra : activeExtraTransients) {
            llvm::Value* other = b.CreateLoad(ptrt, extra.first);
            call2("xyo_transient_flush", voidt, other, extra.second);
        }
    }
    void transientAfterHandoff() {
        if (!activeTransient) return;
        llvm::Value* log = b.CreateLoad(ptrt, activeTransient);
        llvm::Value* resumed = call2(
            "xyo_transient_resume", ptrt, log, activeTransientVariable);
        b.CreateStore(resumed, activeTransient);
        for (const auto& extra : activeExtraTransients) {
            llvm::Value* other = b.CreateLoad(ptrt, extra.first);
            llvm::Value* changed = call2(
                "xyo_transient_resume", ptrt, other, extra.second);
            b.CreateStore(changed, extra.first);
        }
    }
    struct PortableLoopCapture {
        llvm::Value* ids = nullptr;
        llvm::Value* counters = nullptr;
        uint32_t count = 0;
    };
    std::optional<PortableLoopCapture>
    emitPortableLoopCheckpoint(bool bodyComplete = true) {
        // Only the opt-in native LLVM mirror records portable state.
        if (!portableCallFrames() || aot || !curYieldSourceLoop)
            return std::nullopt;
        struct Snapshot {
            const Block* source;
            llvm::AllocaInst* f64;
            llvm::AllocaInst* i32;
        };
        std::vector<Snapshot> path;
        path.reserve(yieldStack.size() + 1);
        for (const auto& scope : yieldStack) {
            if (scope.sourceLoop)
                path.push_back({scope.sourceLoop, scope.remainingF64,
                                scope.remainingI32});
        }
        path.push_back({curYieldSourceLoop, curYieldRemainingF64,
                        curYieldRemainingI32});
        // The experimental frame layout is capped at 32 lexical loops.
        // Its observer is already disabled for scripts exceeding that bound.
        if (path.size() > 32) return std::nullopt;

        // Allocate once in the LLVM function entry, not inside a loop.
        // A source-level path is unique per lexical yield site. Runtime
        // receives outer and inner loop counters as one atomic checkpoint.
        auto* ids = entryAlloca(ptrt, uint32_t(path.size()), "loop.path.ids");
        auto* values = entryAlloca(i64t, uint32_t(path.size()),
                                   "loop.path.counters");
        for (uint32_t i = 0; i < path.size(); ++i) {
            const auto& scope = path[i];
            b.CreateStore(b.CreateGlobalString(scope.source->id),
                          b.CreateConstGEP1_32(ptrt, ids, i));
            llvm::Value* bits = ci64(UINT64_MAX);
            if (scope.i32) {
                bits = b.CreateBitCast(
                    b.CreateSIToFP(b.CreateLoad(i32t, scope.i32), f64t),
                    i64t);
            } else if (scope.f64) {
                bits = b.CreateBitCast(b.CreateLoad(f64t, scope.f64), i64t);
            }
            b.CreateStore(bits, b.CreateConstGEP1_32(i64t, values, i));
        }
        callN("xyo_proc_frame_checkpoint_path_phase", voidt,
              {ctxArg, ids, values, ci32(uint32_t(path.size())),
               ci32(uint32_t(bodyComplete
                   ? ir::SourceResumePhase::AfterLoopBody
                   : ir::SourceResumePhase::InsideLoopBody))});
        return PortableLoopCapture{ids, values, uint32_t(path.size())};
    }

    void emitNestedOsrYieldCheckpoint() {
        if (!nestedOsrInFlightGuard || !osrStartLoop ||
            !curYieldSourceLoop ||
            (!curYieldRemainingF64 && !curYieldRemainingI32))
            return;
        const auto remaining = [&](AllocaInst* integer, AllocaInst* real) {
            return integer
                ? b.CreateSIToFP(b.CreateLoad(i32t, integer), f64t)
                : b.CreateLoad(f64t, real);
        };
        llvm::Value* current =
            remaining(curYieldRemainingI32, curYieldRemainingF64);
        llvm::Value* afterBody = b.CreateFSub(current, cf64(1.0));
        if (curYieldSourceLoop == osrStartLoop) {
            // Phase 1: the selected outer source body has completed.
            call2("xyo_rt_nested_osr_yield_checkpoint", voidt, ctxArg,
                  afterBody);
            return;
        }
        if (!nestedOsrInFlightInnerYield || yieldStack.empty()) return;
        const auto& outer = yieldStack.back();
        if (outer.sourceLoop != osrStartLoop ||
            (!outer.remainingF64 && !outer.remainingI32)) return;
        // Phase 2: inner body completed, outer body remains incomplete.
        llvm::Value* outerCurrent =
            remaining(outer.remainingI32, outer.remainingF64);
        callN("xyo_rt_nested_osr_inner_yield_checkpoint", voidt,
              {ctxArg, b.CreateFSub(outerCurrent, cf64(1.0)),
               afterBody});
    }

    // The deadline poll is shared by ordinary Scratch loops and SSA
    // chunks. Neither path calls the scheduler unless the deadline is
    // actually exceeded (or the sampled-clock fallback is needed).
    void emitWarpDeadlineCheck(BasicBlock* chkBB, BasicBlock* done) {
        b.SetInsertPoint(chkBB);
        if (!sharedWarpClock && promoted.empty() &&
            promotedLists.empty() && !activeTransient) {
            transientBeforeHandoff();
            emitPortableLoopCheckpoint();
            emitNestedOsrYieldCheckpoint();
            call1("xyo_loop_yield", voidt, ctxArg);
            transientAfterHandoff();
            b.CreateBr(done);
        } else {
            llvm::Value* shouldYield;
            if (sharedWarpClock) {
                auto* clockField = b.CreateConstGEP1_32(
                    i8t, ctxArg,
                    uint32_t(offsetof(JitCtx, sampledWarpClockUs)));
                auto* clockPtr = b.CreateLoad(ptrt, clockField);
                auto* sampled = newBB("warp.sampled.clock");
                auto* fallback = newBB("warp.clock.fallback");
                auto* merge = newBB("warp.clock.merge");
                hotBr(b.CreateICmpNE(clockPtr, cnull()),
                      sampled, fallback, 1000, 1);

                b.SetInsertPoint(sampled);
                auto* timeUs = b.CreateLoad(i64t, clockPtr);
                timeUs->setAtomic(AtomicOrdering::Monotonic);
                auto* deadlineField = b.CreateConstGEP1_32(
                    i8t, ctxArg,
                    uint32_t(offsetof(JitCtx, sampledWarpDeadlineUs)));
                auto* deadlinePtr = b.CreateLoad(ptrt, deadlineField);
                auto* deadlineUs = b.CreateLoad(i64t, deadlinePtr);
                deadlineUs->setAtomic(AtomicOrdering::Monotonic);
                auto* expired = b.CreateICmpSGT(timeUs, deadlineUs);
                b.CreateBr(merge);

                b.SetInsertPoint(fallback);
                auto* oldDue = call1("xyo_warp_yield_due", i32t, ctxArg);
                auto* oldExpired = b.CreateICmpNE(oldDue, ci32(0));
                b.CreateBr(merge);

                b.SetInsertPoint(merge);
                auto* joined = b.CreatePHI(b.getInt1Ty(), 2);
                joined->addIncoming(expired, sampled);
                joined->addIncoming(oldExpired, fallback);
                shouldYield = joined;
            } else {
                shouldYield = b.CreateICmpNE(
                    call1("xyo_warp_yield_due", i32t, ctxArg), ci32(0));
            }
            // Neither a sampled clock read nor the fallback synchronous
            // check changes Scratch variables. Spill only on actual yield.
            BasicBlock* handoff = newBB("yield.handoff");
            hotBr(shouldYield, handoff, done, 1, 100000);
            b.SetInsertPoint(handoff);
            transientBeforeHandoff();
            spillPromoted();
            emitPortableLoopCheckpoint();
            emitNestedOsrYieldCheckpoint();
            call1("xyo_yield", voidt, ctxArg);
            reloadPromoted();
            transientAfterHandoff();
            b.CreateBr(done);
        }
        b.SetInsertPoint(done);
    }

    void emitLoopYield() {
        // non-warp は毎反復 yield。warp は 64 回に 1 回 stuck を検査。
        BasicBlock* chkBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* done = BasicBlock::Create(lc, "", fn);
        llvm::Value* np = curYieldCnt
            ? static_cast<llvm::Value*>(curYieldCnt)
            : b.CreateConstGEP1_32(i8t, ctxArg, kYieldCheckNOff);
        llvm::Value* n = b.CreateAdd(b.CreateLoad(i64t, np), ci64(1));
        b.CreateStore(n, np);
        // Non-warp loops must yield on every iteration. Bypass the generic
        // helper and its warp/time checks, leaving the 64-iteration sampling
        // only on the warp path. The counter still advances on both paths.
        llvm::Value* warp = curYieldWarp ? curYieldWarp : loadIsWarp();
        BasicBlock* warpBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* normalBB = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(b.CreateICmpEQ(warp, ci32(0)), normalBB, warpBB);
        b.SetInsertPoint(normalBB);
        transientBeforeHandoff();
        spillPromoted();
        emitPortableLoopCheckpoint();
        // Native CallFrame tracking must observe EVERY real yield, even
        // those that normally bypass the full helper via xyo_yield_fast.
        // Do not change the default fast-yield path when the experiment
        // is disabled.
        if ((portableCallFrames() && !aot) ||
            runtime.blockCountEnabled || std::getenv("XYO_PROF"))
            call1("xyo_yield", voidt, ctxArg);
        else
            call1("xyo_yield_fast", voidt, ctxPtr(4));
        reloadPromoted();
        transientAfterHandoff();
        b.CreateBr(done);
        b.SetInsertPoint(warpBB);
        llvm::Value* due =
            b.CreateICmpEQ(b.CreateAnd(n, ci64(63)), ci64(0));
        hotBr(due, chkBB, done, 1, 63);
        emitWarpDeadlineCheck(chkBB, done);
    }
    // A proven warp-only SSA chunk has already performed all iterations
    // up to a scheduler boundary. Publish its final count directly rather
    // than reconstructing a per-iteration yield, and compute whether this
    // chunk reached the original 64-tick boundary from the chunk length.
    // The runtime path is still used for mode-disabled A/B comparison.
    void emitSsaChunkScheduler(llvm::Value* ticks,
                               llvm::Value* chunk,
                               llvm::Value* toPoll,
                               bool preferDirect = false) {
        // Only the simple scalar affine repeat has a consistent win on
        // unmodified ScratchBench. Other supported SSA recurrences keep
        // the original lowering by default and are available for A/B.
        const char* mode = std::getenv("XYO_JIT_SSA_SCHEDULER");
        const bool direct = mode ? mode[0] == '1' : preferDirect;
        if (!direct) {
            b.CreateStore(b.CreateAdd(ticks, b.CreateSub(chunk, ci64(1))),
                          curYieldCnt);
            emitLoopYield();
            return;
        }
        b.CreateStore(b.CreateAdd(ticks, chunk), curYieldCnt);
        BasicBlock* poll = newBB("ssa.scheduler.poll");
        BasicBlock* done = newBB("ssa.scheduler.done");
        // Unlike a per-iteration yield check (1 in 64), a pure SSA
        // chunk almost always ends *at* the next poll boundary. Lay
        // out that path as hot, leaving the final partial chunk cold.
        hotBr(b.CreateICmpEQ(chunk, toPoll), poll, done, 1000, 1);
        emitWarpDeadlineCheck(poll, done);
    }

    // Inline the scheduler of an empty warp repeat. This is a loop over
    // *poll boundaries*, not logical Scratch iterations: all counter
    // updates are visible to LLVM, and only a real fiber handoff calls the
    // scheduler. The classic helper remains available as a fallback when
    // an outer loop already owns a private yield counter.
    void emitInlineEmptyWarpRepeat(llvm::Value* repetitions) {
        auto* left = entryAlloca(i64t, 1, "empty.scheduler.left");
        auto* tickPtr = b.CreateConstGEP1_32(
            i8t, ctxArg, kYieldCheckNOff);
        b.CreateStore(b.CreateFPToUI(repetitions, i64t), left);
        BasicBlock* head = newBB("empty.scheduler.head");
        BasicBlock* body = newBB("empty.scheduler.body");
        BasicBlock* partial = newBB("empty.scheduler.partial");
        BasicBlock* poll = newBB("empty.scheduler.poll");
        BasicBlock* done = newBB("empty.scheduler.done");
        b.CreateBr(head);
        b.SetInsertPoint(head);
        llvm::Value* remaining = b.CreateLoad(i64t, left);
        b.CreateCondBr(b.CreateICmpEQ(remaining, ci64(0)), done, body);
        b.SetInsertPoint(body);
        llvm::Value* ticks = b.CreateLoad(i64t, tickPtr);
        llvm::Value* toPoll = b.CreateSub(
            ci64(64), b.CreateAnd(ticks, ci64(63)));
        b.CreateCondBr(b.CreateICmpULT(remaining, toPoll), partial, poll);
        b.SetInsertPoint(partial);
        b.CreateStore(b.CreateAdd(ticks, remaining), tickPtr);
        b.CreateBr(done);
        b.SetInsertPoint(poll);
        b.CreateStore(b.CreateSub(remaining, toPoll), left);
        // The warp-only guard dominates this scheduler. Publish the
        // completed 64-tick boundary and poll directly instead of emitting
        // another warp-mode branch, increment and mask check. The actual
        // yield (and all state synchronization) remains unchanged.
        const char* direct = std::getenv("XYO_JIT_INLINE_EMPTY_DIRECT");
        if (direct && direct[0] == '1') {
            b.CreateStore(b.CreateAdd(ticks, toPoll), tickPtr);
            BasicBlock* afterPoll = newBB("empty.scheduler.after.poll");
            emitWarpDeadlineCheck(poll, afterPoll);
        } else {
            b.CreateStore(b.CreateAdd(
                ticks, b.CreateSub(toPoll, ci64(1))), tickPtr);
            emitLoopYield();
        }
        b.CreateBr(head);
        b.SetInsertPoint(done);
    }

    llvm::Value* loadIsWarp() {
        // isWarp は offset 8 の先頭 i32
        llvm::Value* gep = b.CreateConstGEP1_32(i32t, ctxArg, 16);
        return b.CreateLoad(i32t, gep);
    }
    void storeIsWarp(llvm::Value* v) {
        llvm::Value* gep = b.CreateConstGEP1_32(i32t, ctxArg, 16);
        b.CreateStore(v, gep);
    }

    // --- 型判定・変数スロット ---------------------------------------------

    /// NaN ボックスの number 判定（Value::isNumber と同型）
    llvm::Value* emitIsNum(llvm::Value* bits) {
        struct NumericLowerer {
            Codegen& cg;
            llvm::Value* isNumericBits(llvm::Value* raw) {
                llvm::Value* tag = cg.b.CreateAnd(
                    cg.b.CreateLShr(raw, cg.ci64(48)), cg.ci64(0xFFF8));
                return cg.b.CreateOr(
                    cg.b.CreateICmpNE(tag, cg.ci64(0x7FF8)),
                    cg.b.CreateICmpEQ(raw, cg.ci64(val::kNan)));
            }
        } backend{*this};
        return ir::lowerNumericSlotGuard(backend, bits);
    }
    llvm::Value* emitAsF64(llvm::Value* bits) {
        return b.CreateBitCast(bits, f64t);
    }
    /// emitCanon 等で num タグが保証される SSA 値（再チェック省略用）
    std::unordered_set<llvm::Value*> knownNum;
    // Feedback is a prediction, not a proof. All uses still guard the bits.
    std::unordered_set<llvm::Value*> likelyNumLoads;
    // A reporter read of a for-each index is finite when the loop body is
    // a yield-free chain and never writes that index. Such a delta cannot
    // produce NaN when added to a non-NaN promoted accumulator.
    std::unordered_set<std::string> finiteInductionVars;
    std::unordered_set<llvm::Value*> knownFiniteDeltas;
    struct IntInterval {
        int64_t lo, hi;
    };
    // Lexically scoped index proof. Valid only between for-each's implicit
    // index write and the end of its yield-free, index-write-free body.
    std::unordered_map<std::string, IntInterval> activeInductionRanges;
    // Abstract interpretation of immutable LLVM SSA doubles. Never attach
    // ranges to a mutable variable, profile prediction, or non-SSA memory.
    std::unordered_map<llvm::Value*, IntInterval> integerValueRanges;
    std::optional<IntInterval> intInterval(llvm::Value* number) const {
        if (!inductionRangesEnabled) return std::nullopt;
        if (auto it = integerValueRanges.find(number);
            it != integerValueRanges.end()) return it->second;
        const auto* c = llvm::dyn_cast<llvm::ConstantFP>(number);
        if (!c || !c->getValueAPF().isFinite()) return std::nullopt;
        const double d = c->getValueAPF().convertToDouble();
        if (d < -2147483647.0 || d > 2147483647.0 ||
            std::trunc(d) != d) return std::nullopt;
        const int64_t n = static_cast<int64_t>(d);
        return IntInterval{n, n};
    }
    void recordIntInterval(llvm::Value* boxed, IntInterval range) {
        if (!inductionRangesEnabled || range.lo < -2147483647LL ||
            range.hi > 2147483647LL || range.lo > range.hi) return;
        if (auto it = knownNumD.find(boxed); it != knownNumD.end()) {
            integerValueRanges[it->second] = range;
            integralWhenFinite.insert(it->second);
            finiteArithmetic.insert(it->second);
        }
    }
    std::optional<IntInterval> arithmeticIntInterval(
            llvm::Value* fa, llvm::Value* fb, int operation) const {
        auto a = intInterval(fa);
        auto b = intInterval(fb);
        if (!a || !b) return std::nullopt;
        int64_t lo = 0, hi = 0;
        switch (operation) {
        case 0: // add
            lo = a->lo + b->lo; hi = a->hi + b->hi; break;
        case 1: // sub
            lo = a->lo - b->hi; hi = a->hi - b->lo; break;
        case 2: { // mul: signed 32-bit inputs -> product fits signed 64-bit
            const int64_t candidates[4] = {
                a->lo * b->lo, a->lo * b->hi,
                a->hi * b->lo, a->hi * b->hi};
            lo = *std::min_element(std::begin(candidates), std::end(candidates));
            hi = *std::max_element(std::begin(candidates), std::end(candidates));
            break;
        }
        case 4: // modulo: positive constant divisor, nonnegative dividend
            if (a->lo < 0 || b->lo != b->hi || b->lo < 1)
                return std::nullopt;
            lo = 0; hi = b->lo - 1; break;
        default: return std::nullopt;
        }
        if (lo < -2147483647LL || hi > 2147483647LL) return std::nullopt;
        return IntInterval{lo, hi};
    }

    // --- ループ内スカラー変数のレジスタ昇格 -----------------------------
    // Scratch 変数は Variable::value（メモリ）に置かれるため、ループの
    // たびに store→load の転送遅延（~5 cycle）がループキャリー依存に
    // なる。ループ本体の変数アクセスがすべて JIT の inline 経路で完結し、
    // 途中に yield 以外の「観測点」が無いループでは、値を alloca（mem2reg
    // で SSA レジスタ）に載せ、yield の前後だけ実メモリへ spill/reload する。
    struct PromotedVar {
        const BlockField* field;
        llvm::AllocaInst* slot;   // i64 bits（常に有効・正準表現）
        llvm::AllocaInst* dslot;  // f64（numeric のときだけ non-null）
        llvm::AllocaInst* okslot; // i1: d を比較に使ってよいか
        bool numeric = false;
        bool written = false;  // a read-only variable needs no spill
        bool guarded = false;  // speculative string shadow revalidation
        AllocaInst* lazyValid = nullptr; // type-guarded f64 cache
    };
    /// ループ内で item/replace/length だけを受けるリストは、SList の
    /// len/items を preheader と yield 再開時にロードして SSA 化する
    /// （本体内の resize は走査で除外、他スクリプトの変更は再開時に反映）。
    struct PromotedList {
        const BlockField* field;
        AllocaInst* len;    // i32 shadow, refreshed after yield
        AllocaInst* items;  // ptr shadow, refreshed after yield
        AllocaInst* numericOnly; // i8 shadow, refreshed after yield
        AllocaInst* storageEpoch; // i64 shadow, adaptive mode only
    };
    std::vector<PromotedVar> promoted;
    std::vector<PromotedList> promotedLists;
    std::unordered_map<llvm::Value*, const BlockField*> lazyNumericLoads;
    // Local typed value forwarding: a store to a promoted scalar followed
    // by a read in the same IR basic block can reuse the SSA producer (and
    // its numeric/string fact) directly. No fact crosses a control-flow
    // edge, yield, or write through another operation.
    struct LocalVarFact {
        llvm::BasicBlock* block;
        llvm::Value* bits;
        bool fromLoad = false;
    };
    std::unordered_map<std::string, LocalVarFact> localVarFacts;
    // Opt-in load-to-load forwarding: two reads in the same LLVM basic block
    // may reuse an immutable boxed SSA producer. Only promotable lazy-typed
    // scalars are eligible; branch/yield/write invalidation is unchanged.
    // Read the experimental parser selection once per compiled script,
    // rather than querying the process environment for every numeric input.
    const bool coldStringNumberHelperEnabled = [] {
        const char* option = std::getenv("XYO_JIT_TO_NUM_COLD_HELPER");
        return option && option[0] == '1';
    }();
    const bool localReadForwardEnabled = [] {
        const char* option = std::getenv("XYO_JIT_LOCAL_READ_FORWARD");
        return option && option[0] == '1';
    }();
    uint64_t localReadReuses = 0;
    bool localFactsAliasSafe = false;
    const bool localForwardEnabled = [] {
        const char* setting = std::getenv("XYO_JIT_LOCAL_VAR_FORWARD");
        return !setting || setting[0] != '0';
    }();
    // A condition may lower into several LLVM BBs even if it cannot write a
    // Scratch variable (for example, comparing numeric strings). Accept
    // incoming SSA values across those BBs ONLY when every reporter in the
    // expression is on this explicit read-only whitelist. Unknown extensions,
    // procedures and malformed cyclic graphs are barriers.
    bool readonlyCondition(const BlockInput* input) const {
        if (!blocks || !input) return false;
        static const std::unordered_set<std::string_view> kReadOnly = {
            "math_number", "math_integer", "math_whole_number",
            "math_positive_number", "math_angle", "math_boolean",
            "text", "colour_picker", "data_variable", "data_itemoflist",
            "data_lengthoflist", "data_listcontainsitem",
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_mathop", "operator_lt", "operator_gt",
            "operator_equals", "operator_and", "operator_or",
            "operator_not", "operator_join", "operator_letter_of",
            "operator_length", "operator_contains",
        };
        std::unordered_set<std::string> active;
        std::function<bool(const BlockInput&, unsigned)> walk =
            [&](const BlockInput& in, unsigned depth) -> bool {
                if (depth > 64) return false;
                const std::string& id =
                    in.block.empty() ? in.shadow : in.block;
                const Block* source = blocks->getBlock(id);
                if (!source || !kReadOnly.contains(source->opcode) ||
                    !active.insert(id).second) return false;
                for (const auto& [name, child] : source->inputs.items) {
                    (void)name;
                    if (!walk(child, depth + 1)) {
                        active.erase(id);
                        return false;
                    }
                }
                active.erase(id);
                return true;
            };
        return walk(*input, 0);
    }
    struct PhiDemand {
        bool boxed = false;
        bool numeric = false;
    };

    // Local, bounded profitability analysis for a branch join. The
    // forwarded SSA value is available only until the first subsequent
    // assignment/control boundary. We therefore inspect the same linear
    // Scratch stack sequence, evaluating each statement's input reporters
    // BEFORE its write (as the normal lowerer does). This is NOT a type
    // proof: skipping a PHI only falls back to canonical promoted loads.
    std::unordered_map<std::string, PhiDemand> phiDemandAfter(
            const Block* branch) const {
        std::unordered_map<std::string, PhiDemand> demand;
        if (!blocks || !branch) return demand;
        static const std::unordered_set<std::string_view> kNumericParents = {
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_mathop", "operator_lt", "operator_gt",
            "operator_equals", "data_changevariableby",
        };
        std::unordered_set<std::string> killed;
        std::unordered_set<std::string> stackSeen;
        unsigned visitedReporters = 0;
        std::function<void(const BlockInput&, std::string_view, unsigned)>
            visitInput = [&](const BlockInput& in, std::string_view parent,
                             unsigned depth) {
                if (depth >= 32 || visitedReporters >= 256) return;
                const std::string& id =
                    in.block.empty() ? in.shadow : in.block;
                const Block* source = blocks->getBlock(id);
                if (!source || !stackSeen.insert(id).second) return;
                ++visitedReporters;
                if (source->opcode == "data_variable") {
                    auto field = source->fields.find("VARIABLE");
                    if (field != source->fields.end() &&
                        !killed.contains(field->second.id)) {
                        auto& use = demand[field->second.id];
                        use.boxed = true;
                        use.numeric |= kNumericParents.contains(parent);
                    }
                }
                for (const auto& [name, child] : source->inputs.items) {
                    (void)name;
                    visitInput(child, source->opcode, depth + 1);
                }
                stackSeen.erase(id);
            };
        std::unordered_set<std::string> seenBlocks;
        std::string id = branch->next;
        for (unsigned i = 0; !id.empty() && i < 32; ++i) {
            if (!seenBlocks.insert(id).second) break;
            const Block* node = blocks->getBlock(id);
            if (!node) break;
            // An opaque call, loop, branch or scheduler boundary can change
            // the insertion BB and the variable without a visible write.
            // Never forecast SSA consumers across one.
            if (node->opcode.rfind("control_", 0) == 0 ||
                node->opcode == "procedures_call" ||
                node->opcode == "event_broadcast" ||
                node->opcode == "event_broadcastandwait" ||
                (!promotableOpcode(node->opcode) &&
                 node->opcode != "data_setvariableto"))
                break;
            stackSeen.clear();
            for (const auto& [name, input] : node->inputs.items) {
                if (name.rfind("SUBSTACK", 0) == 0) continue;
                visitInput(input, node->opcode, 0);
            }
            if (node->opcode == "data_setvariableto" ||
                node->opcode == "data_changevariableby") {
                auto field = node->fields.find("VARIABLE");
                if (field == node->fields.end()) break;
                killed.insert(field->second.id);
            }
            id = node->next;
        }
        return demand;
    }

    void rememberLocalVar(const BlockField* f, llvm::Value* bits) {
        // The normal numeric shadow already forwards numeric values.
        // Local forwarding is most useful for a mixed-type lazy shadow,
        // where it also preserves the producer's known storage-type fact.
        PromotedVar* promotedVar = findPromoted(f);
        if (localForwardEnabled && localFactsAliasSafe &&
            promotedVar && promotedVar->lazyValid)
            localVarFacts[f->id] = {b.GetInsertBlock(), bits};
    }
    const bool lazyTypedSSA = [] {
        const char* mode = std::getenv("XYO_JIT_LAZY_TYPED_SSA");
        return mode && mode[0] == '1';
    }();
    /// 昇格した数値変数の「boxed bits → f64」対応。emitToNumF64 が
    /// タグ検査・NaN 正規化・GP↔XMM 移動なしで値を返せるようにする。
    std::unordered_map<llvm::Value*, llvm::Value*> knownNumD;
    /// knownNumD の値が「比較の数値 fast path に載せてよい」か（i1）。
    /// 影は toNum の結果なので、非数値文字列は NaN、数値文字列は数値、
    /// 空白のみの文字列は 0 になる — 数値比較になるのは「素の double で
    /// 非 NaN」のときだけ（空白のみの文字列は cast::compare が文字列比較）。
    std::unordered_map<llvm::Value*, llvm::Value*> knownNumOk;
    struct ReadNumberMemo {
        AllocaInst* key;
        AllocaInst* number;
    };
    std::vector<ReadNumberMemo> readNumberMemos;
    std::unordered_map<llvm::Value*, llvm::Value*> finiteMemoPaths;
    std::unordered_set<llvm::Value*> memoizableListReads;
    // A loaded item carries a list-level numeric type guard. Only consumed
    // by explicit numeric conversion; generic Value reads stay untouched.
    std::unordered_map<llvm::Value*, llvm::Value*> numericListReadGuards;
    // Conservative compile-time site filter to avoid overhead on string/mixed
    // lists. The runtime numericOnly guard is still mandatory for safety.
    std::unordered_map<std::string, bool> numericListSiteHints;
    BasicBlock* readMemoPreheader = nullptr;
    bool readMemoAllowed = false;

    // --- ループ yield 判定の巻き上げ ------------------------------------
    // isWarp はループ境界で不変（手続き呼出しは push/pop で復元する）ので
    // ループ入口で一度だけロードする。yieldCheckN は alloca（mem2reg で
    // レジスタ）に載せ、ループ脱出時だけ ctx へ書き戻す — ctx の
    // store→load 依存を毎反復から外す。
    struct YieldScope {
        llvm::AllocaInst* cnt;
        llvm::Value* warp;
        const Block* sourceLoop;
        llvm::AllocaInst* remainingF64;
        llvm::AllocaInst* remainingI32;
    };
    std::vector<YieldScope> yieldStack;
    llvm::AllocaInst* curYieldCnt = nullptr;
    llvm::Value* curYieldWarp = nullptr;
    // Source-level loop checkpoint; distinct from the machine Fiber PC.
    const Block* curYieldSourceLoop = nullptr;
    llvm::AllocaInst* curYieldRemainingF64 = nullptr;
    llvm::AllocaInst* curYieldRemainingI32 = nullptr;
    bool promotingLoop = false;   // このループが write-back を持つ（最外）
    // Resume-epoch guard: compare the actual boxed value after a yield,
    // reusing its numeric shadow when unchanged. A GC collection invalidates
    // pointer identity for strings (the same address can be reused).
    bool epochGuardEnabled = false;
    AllocaInst* promotedGcEpoch = nullptr;
    std::unordered_set<std::string> promotionWrites;
    // Experimental loop-invariant trig: read-only promoted angles are
    // recomputed only at loop entry and after a real scheduler handoff.
    struct TrigHoist {
        std::string variableId;
        int32_t mathop;
        AllocaInst* value;
    };
    std::vector<TrigHoist> trigHoists;
    bool trigHoistAllowed = false;

    // Only constValue registers immutable string literals, keyed by their exact
    // emitted boxed Value, never by interpreting arbitrary ConstantInt pointers.
    // Keep across native functions: LLVM constants are safe module-wide, while
    // instruction keys (including AOT slot-derived bits) are unique SSA values.
    // Payloads are doubles only: no GC references or AOT host pointers.
    std::map<llvm::Value*, double> literalNums;
    /// 比較用: cast::compare の規則（空白のみの文字列は NaN = 文字列比較）。
    /// 相手が数値と分かっている比較でのみ使う（下の foldLiteral）。
    std::map<llvm::Value*, double> literalCmpNums;

    /// double → NaN 正規化 bits（Value::numCanonical と同型）
    llvm::Value* emitCanon(llvm::Value* d) {
        llvm::Value* isn = b.CreateFCmpUNO(d, d);
        llvm::Value* r = b.CreateSelect(isn, ci64(val::kNan),
                                      b.CreateBitCast(d, i64t));
        knownNum.insert(r);
        // Keep the numeric producer in SSA: the next arithmetic input can
        // consume d without bitcasting the canonicalized NaN-box and
        // retesting for NaN. Scratch toNumber(NaN) is zero, but numeric
        // comparisons must still reject the canonical NaN.
        knownNumD[r] = b.CreateSelect(isn, cf64(0.0), d);
        knownNumOk[r] = b.CreateNot(isn);
        return r;
    }
    // Arithmetic facts describe the generated f64 SSA value, not a mutable
    // Scratch variable. They never survive a load or an interpreter handoff.
    // Scratch's toNumber() is non-NaN even for an invalid numeric string.
    std::unordered_set<llvm::Value*> finiteArithmetic;
    // Absolute-value upper bounds on finite SSA values. Rounded upward so
    // a saved double never understates the mathematical interval endpoint.
    std::unordered_map<llvm::Value*, double> finiteUpperBounds;
    std::optional<double> upperBound(llvm::Value* v) const {
        if (!arithmeticRangeFacts) return std::nullopt;
        if (auto it = finiteUpperBounds.find(v);
            it != finiteUpperBounds.end()) return it->second;
        auto* constant = llvm::dyn_cast<llvm::ConstantFP>(v);
        if (!constant || !constant->getValueAPF().isFinite())
            return std::nullopt;
        return std::abs(constant->getValueAPF().convertToDouble());
    }
    void noteFiniteUpperBound(llvm::Value* boxed, double bound) {
        if (!arithmeticRangeFacts || !std::isfinite(bound) || bound < 0)
            return;
        auto it = knownNumD.find(boxed);
        if (it == knownNumD.end()) return;
        finiteArithmetic.insert(it->second);
        finiteUpperBounds[it->second] = bound;
    }
    // Exact IEEE integer *when finite*. This is a property of an SSA producer,
    // never of mutable variables or profiling predictions. All modulo range
    // checks remain in place; only its redundant FP->int->FP exactness test
    // can be skipped when a preceding numeric operation proved integrality.
    std::unordered_set<llvm::Value*> integralWhenFinite;
    bool isIntegralF64(llvm::Value* v) const {
        if (!integralFactsEnabled) return false;
        if (integralWhenFinite.count(v)) return true;
        auto* c = llvm::dyn_cast<llvm::ConstantFP>(v);
        if (!c) return false;
        const llvm::APFloat& ap = c->getValueAPF();
        if (!ap.isFinite()) return false;
        const double value = ap.convertToDouble();
        return std::trunc(value) == value;
    }
    bool isUnitDivisor(llvm::Value* v) const {
        const auto* c = llvm::dyn_cast<llvm::ConstantFP>(v);
        return c && c->getValueAPF().isFinite() &&
               std::abs(c->getValueAPF().convertToDouble()) == 1.0;
    }
    void noteIntegralBox(llvm::Value* bits) {
        if (!integralFactsEnabled) return;
        if (auto it = knownNumD.find(bits); it != knownNumD.end())
            integralWhenFinite.insert(it->second);
    }
    bool arithmeticFinite(llvm::Value* v) const {
        if (finiteArithmetic.count(v)) return true;
        const auto* c = llvm::dyn_cast<llvm::ConstantFP>(v);
        return c && c->getValueAPF().isFinite();
    }
    bool arithmeticFiniteNonzero(llvm::Value* v) const {
        const auto* c = llvm::dyn_cast<llvm::ConstantFP>(v);
        return c && c->getValueAPF().isFinite() && !c->getValueAPF().isZero();
    }
    // A non-NaN IEEE double is already Scratch's canonical numeric Value.
    // Keep the boxed result for comparisons/observers but avoid a redundant
    // fcmp-unordered + two selects when the transfer function proves this.
    llvm::Value* emitCanonKnownNonNaN(llvm::Value* d) {
        llvm::Value* bits = b.CreateBitCast(d, i64t);
        knownNum.insert(bits);
        knownNumD[bits] = d;
        knownNumOk[bits] = b.getInt1(true);
        return bits;
    }
    /// 定数 i64 が number 値ならその double を返す（定数畳み込み用）
    std::optional<double> constNumV(llvm::Value* v) {
        auto* ci = dyn_cast<ConstantInt>(v);
        if (!ci) return std::nullopt;
        Value vv; vv.bits = ci->getZExtValue();
        if (!vv.isNumber()) return std::nullopt;
        return vv.asNumberUnchecked();
    }

    /// num|true|false → f64 の inline 変換。*okOut = 変換可否（他型は false）。
    /// cast::toNumber の fast subset（true→1, false→0）。
    /// ok 判定: ((bits>>48)&0xFFF8)!=0x7FF8（num）|| bits<=kTrue
    /// （0x7FF8 タグの payload 0=kNan=num,1/2=bool、≥3 は他型）
    llvm::Value* emitNumBoolF64(llvm::Value* bits, llvm::Value** okOut) {
        if (knownNum.count(bits)) {
            *okOut = b.getInt1(true);
            return emitAsF64(bits);
        }
        llvm::Value* tag = b.CreateAnd(b.CreateLShr(bits, ci64(48)),
                                       ci64(0xFFF8));
        llvm::Value* isn = b.CreateICmpNE(tag, ci64(0x7FF8));
        *okOut = b.CreateOr(
            isn, b.CreateICmpULE(bits, ci64(val::kTrue)));
        llvm::Value* isTrue = b.CreateICmpEQ(bits, ci64(val::kTrue));
        llvm::Value* isFalse = b.CreateICmpEQ(bits, ci64(val::kFalse));
        // str ポインタの bitcast は select で捨てられるので安全
        return b.CreateSelect(
            isTrue, cf64(1.0),
            b.CreateSelect(isFalse, cf64(0.0), emitAsF64(bits)));
    }
    llvm::Value* emitNotNaN(llvm::Value* d) {
        return b.CreateSelect(b.CreateFCmpUNO(d, d), cf64(0.0), d);
    }
    /// Value → f64（cast::toNumber と同型）。num|bool と SStr 数値
    /// キャッシュは inline、初回パースのみ xyo_to_num helper。
    /// undefined/color/空文字は toNumber → 0。
    llvm::Value* emitToNumF64(llvm::Value* bits) {
        // 昇格した数値変数: 影の f64 をそのまま返す（タグ検査・NaN 正規化・
        // GP↔XMM 移動なし）。影の不変条件が d を非 NaN に保っている。
        if (auto it = knownNumD.find(bits); it != knownNumD.end())
            return it->second;
        if (auto d = constNumV(bits))
            return cf64(std::isnan(*d) ? 0.0 : *d);
        if (!genericLiteralNum) {
            auto it = literalNums.find(bits);
            if (it != literalNums.end()) return cf64(it->second);
        }
        if (knownNum.count(bits)) return emitNotNaN(emitAsF64(bits));
        // Generic mixed-type SSA: perform Scratch coercion only for numeric
        // consumers. Cache the resulting number and its comparison validity
        // until the next write or scheduler handoff. All state is function-
        // local alloca storage, so LLVM can construct PHIs via mem2reg.
        if (auto it = lazyNumericLoads.find(bits);
            it != lazyNumericLoads.end()) {
            const BlockField* field = it->second;
            lazyNumericLoads.erase(it); // prevent redispatch
            PromotedVar* p = findPromoted(field);
            if (p && p->lazyValid) {
                BasicBlock* cached = newBB("typed.ssa.cached");
                BasicBlock* cold = newBB("typed.ssa.convert");
                BasicBlock* merge = newBB("typed.ssa.merge");
                hotBr(b.CreateLoad(b.getInt1Ty(), p->lazyValid),
                      cached, cold, 50, 1);
                b.SetInsertPoint(cached);
                llvm::Value* cachedNumber = b.CreateLoad(f64t, p->dslot);
                llvm::Value* cachedOk =
                    b.CreateLoad(b.getInt1Ty(), p->okslot);
                b.CreateBr(merge);
                b.SetInsertPoint(cold);
                llvm::Value* number = emitToNumF64(bits);
                llvm::Value* numericOk = numericCmpOk(bits, number, true);
                b.CreateStore(number, p->dslot);
                b.CreateStore(numericOk, p->okslot);
                b.CreateStore(b.getTrue(), p->lazyValid);
                BasicBlock* coldEnd = b.GetInsertBlock();
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
                auto* result = b.CreatePHI(f64t, 2, "typed.ssa.number");
                result->addIncoming(cachedNumber, cached);
                result->addIncoming(number, coldEnd);
                auto* compareOk =
                    b.CreatePHI(b.getInt1Ty(), 2, "typed.ssa.cmp");
                compareOk->addIncoming(cachedOk, cached);
                compareOk->addIncoming(numericOk, coldEnd);
                knownNumD[bits] = result;
                knownNumOk[bits] = compareOk;
                return result;
            }
        }
        if (const auto iter = numericListReadGuards.find(bits);
            iter != numericListReadGuards.end() &&
            std::getenv("XYO_JIT_NUMERIC_LIST") &&
            std::getenv("XYO_JIT_NUMERIC_LIST")[0] == '1') {
            // A list-wide monotonic type guard allows direct IEEE decoding
            // without inspecting this element's NaN-box tag. On a mixed
            // list, retain the full Scratch conversion (numeric strings,
            // booleans, color, empty string and canonical NaN included).
            llvm::Value* guard = iter->second;
            numericListReadGuards.erase(iter); // avoid recursive dispatch
            BasicBlock* fast = newBB("list.numeric.fast");
            BasicBlock* slow = newBB("list.numeric.fallback");
            BasicBlock* done = newBB("list.numeric.done");
            hotBr(guard, fast, slow, 1000, 1);
            b.SetInsertPoint(fast);
            llvm::Value* d = emitNotNaN(emitAsF64(bits));
            b.CreateBr(done);
            b.SetInsertPoint(slow);
            llvm::Value* fallback = emitToNumF64(bits);
            // The generic Scratch conversion itself expands into multiple
            // basic blocks. Its final merge, not the initial fallback block,
            // is the predecessor of done.
            BasicBlock* slowEnd = b.GetInsertBlock();
            b.CreateBr(done);
            b.SetInsertPoint(done);
            auto* result = b.CreatePHI(f64t, 2);
            result->addIncoming(d, fast);
            result->addIncoming(fallback, slowEnd);
            return result;
        }
        if (likelyNumLoads.count(bits)) {
            // Monomorphic numeric variables use a small fast path. A change
            // by a different script/clone/monitor falls through to Scratch's
            // general conversion, including NaN, strings and booleans.
            BasicBlock* fast = BasicBlock::Create(lc, "profile.number", fn);
            BasicBlock* slow = BasicBlock::Create(lc, "profile.other", fn);
            BasicBlock* done = BasicBlock::Create(lc, "profile.done", fn);
            hotBr(b.CreateICmpSLT(bits, ci64(val::kNan)),
                  fast, slow, 200, 1);
            b.SetInsertPoint(fast);
            llvm::Value* d = emitNotNaN(emitAsF64(bits));
            b.CreateBr(done);
            b.SetInsertPoint(slow);
            llvm::Value* fallback = call1("xyo_to_num", f64t, bits);
            b.CreateBr(done);
            b.SetInsertPoint(done);
            auto* result = b.CreatePHI(f64t, 2);
            result->addIncoming(d, fast);
            result->addIncoming(fallback, slow);
            return result;
        }
        // Plain-double fast path. `bits < kFalse` is a *signed* compare:
        // every IEEE double (negative ones included, plus +-inf and negative
        // NaN payloads) and kNan sort below the 0x7FF8 payload tags, while
        // false/true/""/undefined, heap strings and colors do not. Hot
        // numeric loops (change i by 1, comparisons, list indices) thus skip
        // the true/false/kNan mapping cascade entirely.
        // 整数比較の fast path（符号付き bits < kFalse）。実測では
        // `d == d`（ord）版より速い — FP 比較は分岐解決が 2〜3 サイクル
        // 遅く、ループのキャリー依存に乗るため。
        BasicBlock* plainBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* outBB = BasicBlock::Create(lc, "", fn);
        // `bits < kFalse` は kTrue/kFalse と同じ 0x7FF8 系列の定数で、
        // LLVM が movabs + leaq で共有できる。kNan を除く版（NaN 正規化を
        // 省ける）は read が 10% 遅くなった（実測）のでこちらを採る。
        hotBr(b.CreateICmpSLT(bits, ci64(val::kFalse)), plainBB, slowBB);
        b.SetInsertPoint(plainBB);
        llvm::Value* plain = emitNotNaN(emitAsF64(bits));
        b.CreateBr(outBB);
        b.SetInsertPoint(slowBB);
        std::optional<ReadNumberMemo> memo;
        BasicBlock* memoHit = nullptr;
        llvm::Value* memoNumber = nullptr;
        const char* memoMode = std::getenv("XYO_LIST_READ_MEMO");
        if ((!memoMode || memoMode[0] != '0') && memoizableListReads.count(bits)) {
            memo = ReadNumberMemo{entryAlloca(i64t, 1, "read.key"),
                                  entryAlloca(f64t, 1, "read.number")};
            readNumberMemos.push_back(*memo);
            IRBuilder<> init(readMemoPreheader->getTerminator());
            init.CreateStore(ci64(val::kUndefined), memo->key);
            init.CreateStore(cf64(0.0), memo->number);
            memoHit = BasicBlock::Create(lc, "read.memo.hit", fn);
            BasicBlock* miss = BasicBlock::Create(lc, "read.memo.miss", fn);
            hotBr(b.CreateICmpEQ(bits, b.CreateLoad(i64t, memo->key)), memoHit, miss);
            b.SetInsertPoint(memoHit);
            memoNumber = b.CreateLoad(f64t, memo->number);
            b.CreateBr(outBB);
            b.SetInsertPoint(miss);
        }
        BasicBlock* fastBB = BasicBlock::Create(lc, "number.other", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "number.string", fn);
        BasicBlock* hitBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* missBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* zeroBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        // Plain numbers were handled above. Dispatch strings before doing the
        // number/boolean tag mapping, especially on non-repeating list values.
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48));
        hotBr(isStr, strBB, fastBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* ok;
        llvm::Value* d = emitNumBoolF64(bits, &ok);
        llvm::Value* nd = emitNotNaN(d);
        b.CreateCondBr(ok, merge, zeroBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(bits, ci64(val::kPtrMask)), ptrt);
        llvm::Value* st = loadSStrCache(i32t, sp, kSStrNumStateOff, true);
        llvm::Value* hit = b.CreateICmpNE(st, ci32(0));
        b.CreateCondBr(hit, hitBB, missBB);
        b.SetInsertPoint(hitBB);
        llvm::Value* nc = loadSStrCache(f64t, sp, kSStrNumCacheOff, false);
        // numState==2 は NaN — cast::toNumber 意味では 0
        llvm::Value* cd = b.CreateSelect(b.CreateICmpEQ(st, ci32(2)),
                                         cf64(0.0), nc);
        b.CreateBr(merge);
        b.SetInsertPoint(missBB);
        // The predecessor proved this is an SStr and numState == 0.
        // The experimental cold-only helper avoids rechecking both facts
        // in the general xyo_to_num entry.
        llvm::Value* hv = coldStringNumberHelperEnabled
            ? call1("xyo_to_num_uncached", f64t, sp)
            : call1("xyo_to_num", f64t, bits);
        b.CreateBr(merge);
        b.SetInsertPoint(zeroBB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(f64t, 4);
        p->addIncoming(nd, fastBB);
        p->addIncoming(cd, hitBB);
        p->addIncoming(hv, missBB);
        p->addIncoming(cf64(0.0), zeroBB);
        if (memo) {
            // Keep one monomorphic entry until yield, rather than thrashing
            // on heterogeneous strings. Only the first miss publishes a key.
            BasicBlock* publish = BasicBlock::Create(lc, "read.memo.publish", fn);
            BasicBlock* done = BasicBlock::Create(lc, "read.memo.ready", fn);
            hotBr(b.CreateICmpEQ(b.CreateLoad(i64t, memo->key), ci64(val::kUndefined)),
                  publish, done, 1, 63);
            b.SetInsertPoint(publish);
            // A hit proves a finite delta. A non-finite first value disables
            // the entry with key=+0,D=0 (also a valid finite coercion pair).
            llvm::Value* finite = b.CreateFCmpOLT(
                b.CreateUnaryIntrinsic(Intrinsic::fabs, p),
                cf64(std::numeric_limits<double>::infinity()));
            b.CreateStore(b.CreateSelect(finite, bits, ci64(0)), memo->key);
            b.CreateStore(b.CreateSelect(finite, p, cf64(0.0)), memo->number);
            b.CreateBr(done);
            b.SetInsertPoint(done);
        }
        BasicBlock* convertedBB = b.GetInsertBlock();
        b.CreateBr(outBB);
        b.SetInsertPoint(outBB);
        PHINode* outp = b.CreatePHI(f64t, memo ? 3 : 2);
        outp->addIncoming(plain, plainBB);
        outp->addIncoming(p, convertedBB);
        if (memo) {
            outp->addIncoming(memoNumber, memoHit);
            PHINode* finitePath = b.CreatePHI(b.getInt1Ty(), 3);
            finitePath->addIncoming(b.getFalse(), plainBB);
            finitePath->addIncoming(b.getFalse(), convertedBB);
            finitePath->addIncoming(b.getTrue(), memoHit);
            finiteMemoPaths[outp] = finitePath;
        }
        return outp;
    }
    // Match SStr's atomic_ref cache protocol, including speculative payload
    // loads in list-index lowering. Acquire state publishes the relaxed payload.
    llvm::Value* loadSStrCache(Type* type, llvm::Value* str, uint32_t off,
                                bool state) {
        auto* load = b.CreateLoad(type, b.CreateConstGEP1_32(i8t, str, off));
        load->setAtomic(state ? AtomicOrdering::Acquire
                              : AtomicOrdering::Monotonic);
        return load;
    }
    static constexpr uint32_t kSStrNumStateOff =
        uint32_t(offsetof(SStr, numState));
    static constexpr uint32_t kSStrNumCacheOff =
        uint32_t(offsetof(SStr, numCache));
    static constexpr uint32_t kSStrLenOff =
        uint32_t(offsetof(SStr, len));
    static constexpr uint32_t kSStrDataOff =
        uint32_t(sizeof(SStr));
    static constexpr uint32_t kSStrFlagsOff =
        uint32_t(offsetof(SStr, flags));
    /// Cons/append-log/Latin-1 have no inlined UTF-16 body. Any direct
    /// UTF-16 load must use the helper for these representations.
    llvm::Value* emitIsConsStr(llvm::Value* sp) {
        auto* flags = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, sp, kSStrFlagsOff));
        flags->setAtomic(AtomicOrdering::Monotonic);
        llvm::Value* f = flags;
        return b.CreateICmpNE(
            b.CreateAnd(f, ci32(SStr::kConsFlag | SStr::kAppendFlag |
                                  SStr::kLatin1Flag)), ci32(0));
    }
    static constexpr uint32_t kSStrRgbStateOff =
        uint32_t(offsetof(SStr, rgbState));
    static constexpr uint32_t kSStrRgbCacheOff =
        uint32_t(offsetof(SStr, rgbCache));

    /// ctx->varSlotData[idx] から Variable* を直接 load して返す。
    /// 初出の (id,isList) にはエントリ関数先頭へ xyo_varslot_init を
    /// 挿入（全使用点を支配。proc 呼出しでは init は再実行されない）。
    llvm::Value* varSlot(const BlockField* f, bool isList,
                         uint64_t* idxOut = nullptr) {
        std::string key = f->id;
        key += '\x03';
        if (f->value.isString() && f->value.asStrPtr()) {
            const auto& n = f->value.asStrPtr()->view();
            key.append(reinterpret_cast<const char*>(n.data()),
                       n.size() * sizeof(char16_t));
        }
        key += isList ? '\x01' : '\x02';
        auto [it, ins] = varSlotIdx.emplace(key, numVarSlots);
        if (idxOut) *idxOut = it->second;
        if (ins) {
            ++numVarSlots;
            IRBuilder<> ib(&entryFn->getEntryBlock(),
                           entryFn->getEntryBlock().begin());
            ib.CreateCall(
                decl("xyo_varslot_init", voidt,
                     {ptrt, i64t, ptrt, ptrt, i32t}),
                {entryFn->getArg(0), ci64(it->second), varIdCAt(f, ib),
                 varNameU16(f), ci32(isList ? 1 : 0)});
        }
        llvm::Value* arr = b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, ctxArg, kVarSlotDataOff));
        llvm::Value* p = b.CreateGEP(ptrt, arr, ci64(it->second));
        auto* load = b.CreateLoad(ptrt, p);
        // Survives procedure inlining; only the final Variable* load is marked,
        // never Variable::value, Variable::list, or any list backing storage.
        load->setMetadata("xyo.varslot", MDNode::get(lc,
            {ConstantAsMetadata::get(ci64(it->second))}));
        return load;
    }
    static constexpr uint32_t kVarSlotDataOff =
        uint32_t(offsetof(JitCtx, varSlotData));
    static constexpr uint32_t kVarValueOff =
        uint32_t(offsetof(Variable, value));
    static constexpr uint32_t kVarListOff =
        uint32_t(offsetof(Variable, list));
    /// 実メモリ（Variable::value）を指す。昇格中でも spill 側はこちらを使う。
    llvm::Value* realVarValuePtr(const BlockField* f) {
        llvm::Value* v = varSlot(f, false);
        return b.CreateConstGEP1_32(i8t, v, kVarValueOff);
    }
    llvm::Value* varValuePtr(const BlockField* f) {
        // par-for の反復カウンタは反復ローカル（i64 の Value bits）へ差し替える
        if (!varOverride.empty()) {
            auto ov = varOverride.find(f->id);
            if (ov != varOverride.end()) return ov->second;
        }
        for (const PromotedVar& p : promoted)
            if (p.field->id == f->id) return p.slot;
        return realVarValuePtr(f);
    }

    /// ループ本体の走査: 変数アクセスがすべて inline で、yield 以外の
    /// 観測点（手続き呼出し・broadcast・wait・stop・compat・sensing_of・
    /// for_each・append 系）を含まないときだけ昇格する。
    static bool promotableOpcode(const std::string& op) {
        static const std::unordered_set<std::string> kOk = {
            "data_variable", "data_setvariableto", "data_changevariableby",
            "data_itemoflist", "data_replaceitemoflist",
            "data_lengthoflist", "data_listcontainsitem",
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_mathop", "operator_lt", "operator_gt",
            "operator_equals", "operator_and", "operator_or",
            "operator_not", "operator_join", "operator_letter_of",
            "operator_length", "operator_contains",
            "control_if", "control_if_else", "control_repeat",
            "control_repeat_until", "control_while", "control_forever",
            "argument_reporter_string_number", "argument_reporter_boolean",
            "argument_reporter",
        };
        return kOk.count(op) != 0;
    }
    /// ループ内で「数値を生まない」書き込みを受ける変数の id。
    /// こういう変数に f64 の数値影を持たせると、代入のたびに toNum が要る
    /// （文字列なら view() = flatten — cons string を毎回 materialize して
    /// しまい、連結ループが O(n²) に戻る）。影は最適化なので、疑わしければ
    /// 付けない（意味論は tagged 経路で常に正しい）。
    std::unordered_set<std::string> nonNumericVars;
    // Unknown-typed set-variable writes, keyed by the exact block ID.
    // The CFG type-flow pass may prove an entire set of writes numeric.
    // This controls only numeric-shadow profitability, not guard elision.
    std::unordered_map<std::string, std::vector<std::string>> dynamicWriteSites;
    // Read/demand facts are scoped to a single promotable loop. They never
    // prove a runtime type or survive a scheduler handoff.
    std::unordered_set<std::string> promotionReads;
    std::unordered_map<std::string, unsigned> numericDemand;

    /// 代入値が数値を生むと静的に言えるか（保守的）。
    // Shared, conservative storage-kind inference used by xyo analyze.
    // In particular, a variable reporter and a list item are NOT necessarily
    // numeric merely because a surrounding Scratch operator coerces them.
    bool valueIsNumeric(Block* blk, const char* input) {
        if (!blocks || !blk) return false;
        // Opt-out for comparing against the original permissive classifier.
        if (const char* flag = std::getenv("XYO_JIT_STATIC_TYPE_SITES");
            flag && flag[0] == '0') {
            const BlockInput* in = blk->inputs.find(input);
            if (!in) return false;
            const std::string& id = in->block.empty() ? in->shadow : in->block;
            Block* source = blocks->getBlock(id);
            if (!source) return false;
            static const std::unordered_set<std::string> originalNumeric = {
                "math_number", "operator_add", "operator_subtract",
                "operator_multiply", "operator_divide", "operator_mod",
                "operator_round", "operator_mathop", "data_variable",
                "data_itemoflist", "data_lengthoflist",
                "sensing_timer", "sensing_dayssince2000", "sensing_answer",
                "sensing_loudness", "motion_xposition", "motion_yposition",
                "motion_direction", "looks_size", "control_get_counter",
                "sensing_mousex", "sensing_mousey",
            };
            return originalNumeric.count(source->opcode) != 0;
        }
        const BlockInput* sourceInput = blk->inputs.find(input);
        const uint32_t kinds =
            analysis::inferInputStorageKinds(*blocks, sourceInput);
        if (kinds == Variable::kObservedNumber) return true;
        // Scratch's [4, "123"] numeric shadow is stored as a string.
        // Nonetheless, its numeric coercion is constant and cheap. Numeric
        // shadow profitability is distinct from the actual storage kind.
        const std::string sourceId = sourceInput
            ? (sourceInput->block.empty() ? sourceInput->shadow
                                          : sourceInput->block)
            : std::string();
        const Block* source = blocks->getBlock(sourceId);
        if (!source || source->fields.size() != 1) return false;
        static const std::unordered_set<std::string_view> numericLiterals = {
            "math_number", "math_integer", "math_whole_number",
            "math_positive_number", "math_angle",
        };
        return numericLiterals.count(source->opcode) != 0;
    }

    bool scanPromotable(const std::string& firstId,
                        std::vector<const BlockField*>& vars,
                        std::vector<const BlockField*>& lists) {
        std::vector<std::string> stack{firstId};
        std::unordered_set<std::string> seen;
        std::unordered_set<std::string> varIds;
        std::unordered_set<std::string> listIds;
        while (!stack.empty()) {
            std::string id = std::move(stack.back());
            stack.pop_back();
            if (id.empty() || !seen.insert(id).second) continue;
            Block* b2 = blocks->getBlock(id);
            if (!b2) return false;
            if (b2->shadow) continue;   // 定数
            // The first implementation only supports straight-line bodies:
            // a nested loop can emit a yield before a trig use is discovered.
            if (b2->opcode.rfind("control_", 0) == 0)
                trigHoistAllowed = false;
            // No list mutation anywhere in the loop, including aliases by
            // another id/name. A cached string remains rooted by its list.
            if (b2->opcode == "data_replaceitemoflist" ||
                b2->opcode == "control_repeat" || b2->opcode == "control_forever" ||
                b2->opcode == "control_while" || b2->opcode == "control_repeat_until" ||
                b2->opcode == "control_for_each") readMemoAllowed = false;
            if (!promotableOpcode(b2->opcode)) {
                if (std::getenv("XYO_JIT_VERBOSE"))
                    std::fprintf(stderr, "xyo-jit: promo skip (%s at %s)\n",
                                 b2->opcode.c_str(), id.c_str());
                return false;
            }
            if (b2->opcode == "data_variable" ||
                b2->opcode == "data_setvariableto" ||
                b2->opcode == "data_changevariableby") {
                const BlockField* f = findField(b2, "VARIABLE");
                if (!f) return false;
                if (varIds.insert(f->id).second) vars.push_back(f);
                if (b2->opcode == "data_variable" ||
                    b2->opcode == "data_changevariableby")
                    promotionReads.insert(f->id);
                if (b2->opcode == "data_changevariableby")
                    ++numericDemand[f->id];
                if (b2->opcode != "data_variable") {
                    promotionWrites.insert(f->id);
                    // Change-by is always a numeric *write*, regardless
                    // of the storage type of its input/delta expression.
                    if (b2->opcode == "data_setvariableto" &&
                        !valueIsNumeric(b2, "VALUE")) {
                        nonNumericVars.insert(f->id);
                        dynamicWriteSites[f->id].push_back(b2->id);
                    }
                }
            }
            // A variable reporter is consumed numerically only when it is
            // a direct operand of a coercing operator. A join/text consumer
            // must not activate a number cache (flattening can be expensive).
            static const std::unordered_set<std::string_view> kNumericParents = {
                "operator_add", "operator_subtract", "operator_multiply",
                "operator_divide", "operator_mod", "operator_round",
                "operator_mathop", "operator_lt", "operator_gt",
                "operator_equals", "data_changevariableby",
            };
            if (kNumericParents.count(b2->opcode)) {
                for (const auto& [name, input] : b2->inputs.items) {
                    (void)name;
                    const auto& childId = input.block.empty() ? input.shadow : input.block;
                    Block* child = blocks->getBlock(childId);
                    if (!child || child->opcode != "data_variable") continue;
                    if (const BlockField* field = findField(child, "VARIABLE"))
                        ++numericDemand[field->id];
                }
            }
            if (b2->opcode == "data_itemoflist" ||
                b2->opcode == "data_replaceitemoflist" ||
                b2->opcode == "data_lengthoflist") {
                const BlockField* f = findField(b2, "LIST");
                if (!f) return false;
                if (listIds.insert(f->id).second) lists.push_back(f);
            }
            if (!b2->next.empty()) stack.push_back(b2->next);
            for (const auto& [n, in] : b2->inputs.items) {
                (void)n;
                if (!in.block.empty()) stack.push_back(in.block);
                if (!in.shadow.empty()) stack.push_back(in.shadow);
            }
        }
        return true;
    }
    /// ループ本体の直前（preheader）で呼ぶ。このループが昇格を所有したら true。
    bool beginLoopPromotion(Block* loopBlk, bool gpuHandled) {
        if (!promoted.empty() || promotingLoop || gpuHandled) return false;
        readMemoAllowed = true;
        const BlockInput* in = loopBlk->inputs.find("SUBSTACK");
        if (!in || in->block.empty()) return false;
        std::vector<const BlockField*> vars;
        std::vector<const BlockField*> lists;
        nonNumericVars.clear();
        dynamicWriteSites.clear();
        localVarFacts.clear();
        promotionReads.clear();
        numericDemand.clear();
        promotionWrites.clear();
        trigHoists.clear();
        trigHoistAllowed = std::getenv("XYO_JIT_TRIG_HOIST") &&
            std::getenv("XYO_JIT_TRIG_HOIST")[0] == '1';
        // The body plus every other input of the loop block: repeat_until /
        // while re-evaluate CONDITION inside the loop, so its accesses must be
        // part of the promotion set (a procedure call there would write the
        // variable behind the shadow's back -> scanPromotable bails).
        bool ok = true;
        for (const auto& [n, li] : loopBlk->inputs.items) {
            (void)n;
            if (!li.block.empty() && !scanPromotable(li.block, vars, lists)) {
                ok = false;
                break;
            }
            if (!li.shadow.empty() && !scanPromotable(li.shadow, vars, lists)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            trigHoistAllowed = false;
            return false;
        }
        // for_each implicitly writes VARIABLE before running its body.
        // Include that slot even when the body never refers to it, so the
        // regular yield spill/reload and loop-exit writeback stay coherent.
        if (loopBlk->opcode == "control_for_each") {
            if (const BlockField* f = findField(loopBlk, "VARIABLE")) {
                vars.push_back(f);
                promotionWrites.insert(f->id);
            }
        }
        if (vars.empty() && lists.empty()) return false;
        // Each input of the loop block is scanned separately, so the same
        // variable can be collected more than once — dedupe by id. A duplicate
        // slot would also break the yield spill (the stale twin would be
        // written back last).
        std::unordered_set<std::string> seenVar;
        std::vector<const BlockField*> uniq;
        for (const BlockField* f : vars)
            if (!f->id.empty() && seenVar.insert(f->id).second)
                uniq.push_back(f);
        // A malformed/name-aliased project may address the same Variable
        // through different IDs. Never forward local SSA values in that
        // case: a store through an alternate ID can invalidate the fact.
        localFactsAliasSafe = target != nullptr;
        std::unordered_set<const Variable*> resolvedVars;
        for (const BlockField* f : uniq) {
            const Variable* v = target ? target->lookupVariableById(f->id)
                                       : nullptr;
            if (!v || !resolvedVars.insert(v).second) {
                localFactsAliasSafe = false;
                break;
            }
        }
        // Optional CFG-aware profitability hint. This requires every
        // uncertain write to have a Number-only post-state at every reachable
        // occurrence, with no unresolved/aliased variable IDs. No dynamic
        // type check is removed: the numeric shadow always maintains
        // toNumber(boxed) plus Scratch comparison eligibility.
        const char* cfgTypeHints = std::getenv("XYO_JIT_CFG_TYPE_HINTS");
        if (cfgTypeHints && cfgTypeHints[0] == '1' &&
            localFactsAliasSafe && !dynamicWriteSites.empty()) {
            const auto flow = analysis::analyzeTypeFlow(*blocks, loopBlk->id,
                                                         8192, target);
            if (!flow.cfg.truncated) {
                for (const auto& [variableId, writes] : dynamicWriteSites) {
                    bool numericAtAllWrites = !writes.empty();
                    for (const std::string& write : writes) {
                        auto positions = flow.cfg.blockNodes.find(write);
                        if (positions == flow.cfg.blockNodes.end()) {
                            numericAtAllWrites = false;
                            break;
                        }
                        bool sawReachable = false;
                        for (size_t index : positions->second) {
                            if (!flow.reachable[index]) continue;
                            sawReachable = true;
                            if (flow.kindAfter(index, variableId) !=
                                Variable::kObservedNumber) {
                                numericAtAllWrites = false;
                                break;
                            }
                        }
                        if (!sawReachable) numericAtAllWrites = false;
                        if (!numericAtAllWrites) break;
                    }
                    if (numericAtAllWrites) {
                        nonNumericVars.erase(variableId);
                        if (verbose)
                            std::fprintf(stderr,
                                "xyo-jit: cfg numeric-shadow %s\n",
                                variableId.c_str());
                    }
                }
            }
        }
        // 数値影の可否は**変数ごと**に決める: ループ内の書き込みがすべて
        // 数値を生むと分かる変数だけ f64 の影を持つ。文字列を受ける変数に
        // 影を持たせると、代入のたびに toNum（= view = flatten）が走る。
        const char* guardEnv = std::getenv("XYO_JIT_EPOCH_GUARD");
        epochGuardEnabled = guardEnv && guardEnv[0] == '1';
        promotedGcEpoch = nullptr;
        for (const BlockField* f : uniq) {
            AllocaInst* slot = entryAlloca(i64t, 1, "pv");
            llvm::Value* init = b.CreateLoad(i64t, realVarValuePtr(f));
            b.CreateStore(init, slot);
            AllocaInst* ds = nullptr;
            AllocaInst* oks = nullptr;
            // Write-only variables do not need a numeric shadow at all:
            // writes still update the canonical i64 slot and are published at
            // yield/exit. The old path eagerly coerced their initial values
            // and every subsequent assignment despite no numeric consumer.
            const bool read = promotionReads.count(f->id) != 0;
            const bool numeric = read && !nonNumericVars.count(f->id);
            const char* typeFlowEnv = std::getenv("XYO_JIT_TYPE_FLOW");
            const bool typeFlow = !typeFlowEnv || typeFlowEnv[0] != '0';
            // Reuse the existing guarded lazy SSA for genuinely mixed
            // variables with repeated static numeric consumption. Its
            // validity bit is cleared on every unknown write and yield.
            const bool lazy = read && !numeric &&
                (lazyTypedSSA ||
                 (typeFlow && numericDemand[f->id] >= 2));
            AllocaInst* lazyValid = nullptr;
            if (lazy) {
                ds = entryAlloca(f64t, 1, "pv.lazy.number");
                oks = entryAlloca(b.getInt1Ty(), 1, "pv.lazy.cmp");
                lazyValid = entryAlloca(b.getInt1Ty(), 1, "pv.lazy.valid");
                b.CreateStore(b.getFalse(), lazyValid);
            }
            if (numeric) {
                ds = entryAlloca(f64t, 1, "pvd");
                // 不変条件: d == toNum(bits)。初期値は文字列でもよい
                // （"0" のような数値リテラルは constValue が畳む）。
                llvm::Value* d0 = emitToNumF64(init);
                b.CreateStore(d0, ds);
                // 比較に d を使ってよいのは初期値が素の double のとき。
                oks = entryAlloca(b.getInt1Ty(), 1, "pvok");
                b.CreateStore(numericCmpOk(init, d0, true), oks);
            }
            const bool written = promotionWrites.count(f->id) != 0;
            // Currently guard only read-only variables initially holding a
            // string. Speculation only affects performance, never correctness.
            const Variable* known = target && !f->id.empty()
                ? target->lookupVariableById(f->id) : nullptr;
            const bool guarded = epochGuardEnabled && numeric && !written &&
                known && known->value.isString();
            promoted.push_back({f, slot, ds, oks, numeric, written, guarded, lazyValid});
        }
        // リストヘッダ (len/items) を preheader へ。リストは slot init で
        // lookupOrCreateList 済みなので通常は非 null だが、null 経路でも
        // 本体は slow path へ落ちるので 0/null を phi で渡す。
        std::unordered_set<std::string> seenList;
        for (const BlockField* f : lists) {
            if (!seenList.insert(f->id).second) continue;
            llvm::Value* l = varListPtr(f);
            BasicBlock* loadBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* zeroBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* mergeBB = BasicBlock::Create(lc, "", fn);
            hotBr(b.CreateICmpNE(b.CreatePtrToInt(l, i64t), ci64(0)),
                  loadBB, zeroBB);
            b.SetInsertPoint(loadBB);
            llvm::Value* len =
                b.CreateLoad(i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
            llvm::Value* items =
                b.CreateLoad(ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
            llvm::Value* numericValue = b.CreateLoad(b.getInt8Ty(),
                b.CreateConstGEP1_32(i8t, l, kListNumericOnlyOff));
            llvm::Value* epoch = xyo_list_storage_mode == 7
                ? b.CreateLoad(i64t, b.CreateConstGEP1_32(
                      i8t, l, uint32_t(offsetof(SList, storageEpoch))))
                : nullptr;
            b.CreateBr(mergeBB);
            b.SetInsertPoint(zeroBB);
            b.CreateBr(mergeBB);
            b.SetInsertPoint(mergeBB);
            PHINode* lenPhi = b.CreatePHI(i32t, 2);
            lenPhi->addIncoming(ci32(0), zeroBB);
            lenPhi->addIncoming(len, loadBB);
            PHINode* itemsPhi = b.CreatePHI(ptrt, 2);
            itemsPhi->addIncoming(
                ConstantPointerNull::get(
                    static_cast<llvm::PointerType*>(ptrt)), zeroBB);
            itemsPhi->addIncoming(items, loadBB);
            PHINode* numericPhi = b.CreatePHI(b.getInt8Ty(), 2);
            numericPhi->addIncoming(b.getInt8(0), zeroBB);
            numericPhi->addIncoming(numericValue, loadBB);
            PHINode* epochPhi = nullptr;
            if (xyo_list_storage_mode == 7) {
                epochPhi = b.CreatePHI(i64t, 2);
                epochPhi->addIncoming(ci64(0), zeroBB);
                epochPhi->addIncoming(epoch, loadBB);
            }
            AllocaInst* lenSlot = entryAlloca(i32t, 1, "list.len");
            AllocaInst* itemsSlot = entryAlloca(ptrt, 1, "list.items");
            AllocaInst* numberSlot = entryAlloca(b.getInt8Ty(), 1, "list.numeric");
            AllocaInst* epochSlot = xyo_list_storage_mode == 7
                ? entryAlloca(i64t, 1, "list.storage.epoch") : nullptr;
            b.CreateStore(lenPhi, lenSlot);
            b.CreateStore(itemsPhi, itemsSlot);
            b.CreateStore(numericPhi, numberSlot);
            if (epochSlot) b.CreateStore(epochPhi, epochSlot);
            promotedLists.push_back({f, lenSlot, itemsSlot, numberSlot, epochSlot});
        }
        promotingLoop = !promoted.empty() || !promotedLists.empty();
        if (std::any_of(promoted.begin(), promoted.end(),
                        [](const PromotedVar& p) { return p.guarded; }))
            promotedGcEpoch = entryAlloca(i64t, 1, "pv.gc.epoch");
        readMemoPreheader = promotingLoop ? b.GetInsertBlock() : nullptr;
        return promotingLoop;
    }
    // A pure, uninterrupted for-each body that never writes its induction
    // variable will always observe the loop header's finite i+1. Reject
    // nested control structures, procedures, and all opaque blocks rather
    // than assuming that they cannot yield or mutate state.
    bool finiteIndexBody(const BlockInput* body, const std::string& indexId) {
        if (!body || body->block.empty()) return false;
        std::vector<std::string> todo{body->block};
        std::unordered_set<std::string> visited;
        while (!todo.empty()) {
            std::string id = std::move(todo.back());
            todo.pop_back();
            if (id.empty() || !visited.insert(id).second) continue;
            Block* node = getBlock(id);
            if (!node) return false;
            const std::string& op = node->opcode;
            if (!node->shadow &&
                op != "data_variable" &&
                op != "data_setvariableto" &&
                op != "data_changevariableby" &&
                op.rfind("operator_", 0) != 0)
                return false;
            if (op == "data_setvariableto" ||
                op == "data_changevariableby") {
                const BlockField* target = findField(node, "VARIABLE");
                if (!target || target->id == indexId) return false;
            }
            if (!node->next.empty()) todo.push_back(node->next);
            for (const auto& [name, in] : node->inputs.items) {
                (void)name;
                if (!in.block.empty()) todo.push_back(in.block);
                if (!in.shadow.empty()) todo.push_back(in.shadow);
            }
        }
        return true;
    }
    // The historical finiteIndexBody() recognizes many operators and merely
    // proves a finite delta. A *typed interval* is stronger: reject opaque,
    // extension, random, and potentially side-effecting reporters. The
    // compiler never substitutes a profile prediction for this structural
    // proof. It also rejects all nested control constructs and index writes.
    bool strictInductionBody(const BlockInput* body,
                             const BlockField* indexField) {
        if (!indexField || !indexField->value.isString() ||
            !indexField->value.asStrPtr()) return false;
        const std::string& indexId = indexField->id;
        const auto indexName = indexField->value.asStrPtr()->view();
        if (!finiteIndexBody(body, indexId)) return false;
        if (!body || body->block.empty()) return false;
        static const std::unordered_set<std::string> kPure = {
            "data_variable", "data_setvariableto", "data_changevariableby",
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_mathop", "operator_lt", "operator_gt",
            "operator_equals", "operator_and", "operator_or",
            "operator_not"
        };
        std::vector<std::string> todo{body->block};
        std::unordered_set<std::string> visited;
        while (!todo.empty()) {
            std::string id = std::move(todo.back());
            todo.pop_back();
            if (id.empty() || !visited.insert(id).second) continue;
            Block* node = getBlock(id);
            if (!node) return false;
            if (!node->shadow && !kPure.count(node->opcode)) return false;
            if (node->opcode == "data_setvariableto" ||
                node->opcode == "data_changevariableby") {
                const BlockField* written = findField(node, "VARIABLE");
                if (!written) return false;
                // An invalid/stale ID can resolve by name to this same
                // Scratch variable. Check both ID and name rather than
                // trusting the textual ID alone.
                if (written->id == indexId || !written->value.isString() ||
                    !written->value.asStrPtr()) return false;
                if (written->value.asStrPtr()->view() == indexName)
                    return false;
            }
            if (!node->next.empty()) todo.push_back(node->next);
            for (const auto& [name, in] : node->inputs.items) {
                (void)name;
                if (!in.block.empty()) todo.push_back(in.block);
                if (!in.shadow.empty()) todo.push_back(in.shadow);
            }
        }
        return true;
    }

    const PromotedList* findPromotedList(const std::string& id) const {
        for (const PromotedList& pl : promotedLists)
            if (pl.field->id == id) return &pl;
        return nullptr;
    }
    /// ループ脱出点（merge）で呼ぶ。実メモリへ書き戻す。
    void endLoopPromotion(bool owner) {
        if (!owner || !promotingLoop) return;
        for (const PromotedVar& p : promoted) {
            if (p.written)
                b.CreateStore(b.CreateLoad(i64t, p.slot),
                              realVarValuePtr(p.field));
        }
        promoted.clear();
        lazyNumericLoads.clear();
        promotedLists.clear();
        promotedGcEpoch = nullptr;
        trigHoists.clear();
        trigHoistAllowed = false;
        readNumberMemos.clear();
        finiteMemoPaths.clear();
        memoizableListReads.clear();
        readMemoPreheader = nullptr;
        readMemoAllowed = false;
        promotingLoop = false;
    }
    void clearLoopPromotion(bool owner) {
        if (!owner) return;
        promoted.clear();
        lazyNumericLoads.clear();
        promotedLists.clear();
        promotedGcEpoch = nullptr;
        trigHoists.clear();
        trigHoistAllowed = false;
        readNumberMemos.clear();
        finiteMemoPaths.clear();
        memoizableListReads.clear();
        readMemoPreheader = nullptr;
        readMemoAllowed = false;
        promotingLoop = false;
    }
    /// yield の直前/直後: 実メモリと同期する（他スレッドが読み書きできる）。
    void spillPromoted() {
        // bits 影は常に正準表現（NaN は kNan）なので、そのまま書き戻せる。
        for (const PromotedVar& p : promoted) {
            if (p.written)
                b.CreateStore(b.CreateLoad(i64t, p.slot),
                              realVarValuePtr(p.field));
        }
        if (promotedGcEpoch)
            b.CreateStore(callN("xyo_gc_epoch", i64t, {}), promotedGcEpoch);
    }
    void reloadPromoted() {
        localVarFacts.clear(); // external scripts may have changed any type
        // Stop-the-world collection cannot advance during a non-yielding reload.
        llvm::Value* resumeGcEpoch = promotedGcEpoch
            ? callN("xyo_gc_epoch", i64t, {})
            : nullptr;
        for (const PromotedVar& p : promoted) {
            auto* ld = b.CreateLoad(i64t, realVarValuePtr(p.field));
            ld->setVolatile(true);   // 呼出し前の store を畳ませない
            if (p.guarded && promotedGcEpoch) {
                // A bitwise identical immediate is always the same value.
                // For heap strings an allocation can reuse the same pointer
                // after GC, so identical bits are usable only if the GC epoch
                // is also unchanged. This is safe even for writers that do
                // not publish a per-variable version (GPU/UI/transient).
                auto* old = b.CreateLoad(i64t, p.slot);
                llvm::Value* equalBits = b.CreateICmpEQ(ld, old);
                llvm::Value* wasHeapString = b.CreateICmpEQ(
                    b.CreateLShr(old, ci64(48)), ci64(0x7ff9));
                BasicBlock* checkEpoch = newBB("pv.check.epoch");
                BasicBlock* keep = newBB("pv.keep");
                BasicBlock* changed = newBB("pv.changed");
                BasicBlock* done = newBB("pv.reload.done");
                BasicBlock* maybeHeap = newBB("pv.maybe.heap");
                hotBr(equalBits, maybeHeap, changed, 1000, 1);
                b.SetInsertPoint(maybeHeap);
                hotBr(wasHeapString, checkEpoch, keep, 1, 1000);
                b.SetInsertPoint(checkEpoch);
                llvm::Value* sameEpoch = b.CreateICmpEQ(
                    b.CreateLoad(i64t, promotedGcEpoch),
                    resumeGcEpoch);
                hotBr(sameEpoch, keep, changed, 1000, 1);
                b.SetInsertPoint(keep);
                b.CreateBr(done);
                b.SetInsertPoint(changed);
                b.CreateStore(ld, p.slot);
                llvm::Value* d = emitToNumF64(ld);
                b.CreateStore(d, p.dslot);
                b.CreateStore(numericCmpOk(ld, d, true), p.okslot);
                b.CreateBr(done);
                b.SetInsertPoint(done);
            } else {
                b.CreateStore(ld, p.slot);
                if (p.lazyValid) {
                    // A yield can change the variable's value and type.
                    b.CreateStore(b.getFalse(), p.lazyValid);
                }
                if (p.numeric) {
                    llvm::Value* d = emitToNumF64(ld);
                    b.CreateStore(d, p.dslot);
                    b.CreateStore(numericCmpOk(ld, d, true), p.okslot);
                }
            }
        }
        // Refresh loop-invariant trigonometry after observing all writes
        // from other Scratch threads at this yield boundary.
        for (const TrigHoist& hoist : trigHoists) {
            auto it = std::find_if(promoted.begin(), promoted.end(),
                [&](const PromotedVar& p) {
                    return p.field->id == hoist.variableId;
                });
            if (it != promoted.end()) {
                llvm::Value* angle = b.CreateLoad(f64t, it->dslot);
                b.CreateStore(emitMathop(hoist.mathop, angle), hoist.value);
            }
        }
        // A yielded script may resize or clear a list. Keep only the SList*
        // invariant; len/items must be refreshed before any resumed access.
        for (const PromotedList& list : promotedLists) {
            llvm::Value* l = varListPtr(list.field);
            BasicBlock* have = BasicBlock::Create(lc, "list.reload", fn);
            BasicBlock* empty = BasicBlock::Create(lc, "list.reload.empty", fn);
            BasicBlock* done = BasicBlock::Create(lc, "list.reload.done", fn);
            hotBr(b.CreateICmpNE(b.CreatePtrToInt(l, i64t), ci64(0)), have, empty);
            b.SetInsertPoint(have);
            b.CreateStore(b.CreateLoad(i32t,
                b.CreateConstGEP1_32(i8t, l, kListLenOff)), list.len);
            b.CreateStore(b.CreateLoad(ptrt,
                b.CreateConstGEP1_32(i8t, l, kListItemsOff)), list.items);
            b.CreateStore(b.CreateLoad(b.getInt8Ty(),
                b.CreateConstGEP1_32(i8t, l, kListNumericOnlyOff)),
                list.numericOnly);
            if (list.storageEpoch)
                b.CreateStore(b.CreateLoad(i64t, b.CreateConstGEP1_32(
                    i8t, l, uint32_t(offsetof(SList, storageEpoch)))),
                    list.storageEpoch);
            b.CreateBr(done);
            b.SetInsertPoint(empty);
            b.CreateStore(ci32(0), list.len);
            b.CreateStore(ConstantPointerNull::get(llvm::cast<PointerType>(ptrt)), list.items);
            b.CreateStore(b.getInt8(0), list.numericOnly);
            if (list.storageEpoch)
                b.CreateStore(ci64(0), list.storageEpoch);
            b.CreateBr(done);
            b.SetInsertPoint(done);
        }
        // A different script may remove a cached string while we yield.
        // Never compare its old pointer after resuming (GC may reuse it).
        for (const ReadNumberMemo& memo : readNumberMemos) {
            b.CreateStore(ci64(val::kUndefined), memo.key);
            b.CreateStore(cf64(0.0), memo.number);
        }
    }
    llvm::Value* varListPtr(const BlockField* f) {
        uint64_t idx = 0;
        llvm::Value* v = varSlot(f, true, &idx);
        llvm::Value* lp = b.CreateConstGEP1_32(i8t, v, kVarListOff);
        auto* load = b.CreateLoad(ptrt, lp);
        // Variable::list is filled once by xyo_varslot_init (lookupOrCreateList)
        // and never replaced (delete all keeps the SList), so the pointer is
        // loop-invariant. hoistVarSlotPointers lifts it to the function entry
        // together with the Variable* load; without that every list access
        // pays a 3-deep dependent load chain (slot -> Variable -> SList).
        load->setMetadata("xyo.varlist", MDNode::get(lc,
            {ConstantAsMetadata::get(ci64(idx))}));
        return load;
    }

    PromotedVar* findPromoted(const BlockField* f) {
        if (!f) return nullptr;
        for (PromotedVar& p : promoted)
            if (p.field->id == f->id) return &p;
        return nullptr;
    }
    /// 比較の数値 fast path に載せてよいか（素の double かつ非 NaN）。
    /// 影の d だけでは足りない — toNum(" ")==0 だが cast::compare は
    /// 空白のみの文字列を文字列比較する（"1000" のような数値文字列は
    /// 数値比較でよいので d がそのまま使える）。
    llvm::Value* numericCmpOk(llvm::Value* bits, llvm::Value* d,
                              bool checkCachedString = false) {
        // A Scratch numeric shadow such as "0" or "1" is stored as a
        // string, not an IEEE double. constValue already recorded exactly
        // whether this immutable literal participates in numeric comparison
        // (whitespace-only strings and NaN do not). Preserve its numeric
        // shadow for all writes, including after loop-promotion reloads.
        if (auto it = literalCmpNums.find(bits); it != literalCmpNums.end())
            return b.getInt1(!std::isnan(it->second));
        if (auto it = knownNumOk.find(bits); it != knownNumOk.end())
            return it->second;
        llvm::Value* numeric = b.CreateAnd(
            b.CreateICmpSLT(bits, ci64(val::kNan)),
            b.CreateFCmpORD(d, d));
        if (!checkCachedString) return numeric;
        // A promoted value may be reloaded after a yield. The stored boxed
        // value can then be a numeric string supplied by another script,
        // even if it did not originate from this function's literal table.
        // emitToNumF64(bits) already populated the string's immutable
        // parse cache; state 1/4 is exactly the numeric-comparison subset.
        BasicBlock* stringBB = newBB("cmpok.string");
        BasicBlock* doneBB = newBB("cmpok.done");
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(isStr, stringBB, doneBB);
        BasicBlock* numBB = b.GetInsertBlock();
        b.SetInsertPoint(stringBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(bits, ci64(val::kPtrMask)), ptrt);
        llvm::Value* state = loadSStrCache(i32t, sp, kSStrNumStateOff, true);
        llvm::Value* isNumericStr = b.CreateOr(
            b.CreateICmpEQ(state, ci32(1)),
            b.CreateICmpEQ(state, ci32(4)));
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        PHINode* ok = b.CreatePHI(b.getInt1Ty(), 2);
        ok->addIncoming(numeric, numBB);
        ok->addIncoming(isNumericStr, stringBB);
        return ok;
    }
    /// (bits, d) の整合ペアを書き込む。NaN は bits=kNan / d=0 に落とす
    /// （Scratch の正準表現 + toNum(NaN)=0 を、読み出し側の検査なしで保つ）。
    /// ok は「比較に d を使えるか」— 省略時は (bits, d) から判定する。
    void storeNumericPair(PromotedVar& p, llvm::Value* bits,
                          llvm::Value* d, llvm::Value* ok = nullptr) {
        b.CreateStore(bits, p.slot);
        b.CreateStore(d, p.dslot);
        llvm::Value* o = ok ? ok : numericCmpOk(bits, d);
        b.CreateStore(o, p.okslot);
        knownNumD[bits] = d;
        knownNumOk[bits] = o;
    }
    // Sample the resolved variable once while emitting this module. A
    // guard on each read keeps the compiled code valid across type changes.
    std::unordered_map<const BlockField*, bool> numberProfileCache;
    bool profiledNumber(const BlockField* f) {
        if (!f || aot || typeProfileDisabled || !target) return false;
        if (auto it = numberProfileCache.find(f);
            it != numberProfileCache.end()) return it->second;
        Variable* v = target->lookupVariableById(f->id);
        if (!v && f->value.isString()) {
            if (SStr* name = f->value.asStrPtr())
                v = target->lookupVariableByNameAndType(
                    std::u16string(name->view()), Variable::kScalarType);
        }
        bool numeric = false;
        if (v && v->type == Variable::kScalarType) {
            // Include initial SB3/host values not observed by the interpreter.
            v->observeValueType(v->value);
            numeric = v->observedOnlyNumber();
        }
        numberProfileCache.emplace(f, numeric);
        return numeric;
    }

    llvm::Value* emitVarGet(const BlockField* f) {
        // A fact is usable only in the block where the corresponding store
        // was emitted. This avoids a compiler-side CFG merge or stale value.
        if (localForwardEnabled && localFactsAliasSafe &&
            f && findPromoted(f) && findPromoted(f)->lazyValid) {
            auto it = localVarFacts.find(f->id);
            if (it != localVarFacts.end() &&
                it->second.block == b.GetInsertBlock()) {
                if (localReadForwardEnabled && it->second.fromLoad)
                    ++localReadReuses;
                return it->second.bits;
            }
        }
        // jitVar（lookupOrCreate）は常に非 null — helper の v? ガードと等価
        llvm::Value* vp = varValuePtr(f);
        struct SlotLowerer {
            IRBuilder<>& builder;
            llvm::Type* bitsType;
            llvm::Value* loadSlot(llvm::Value* address) {
                return builder.CreateLoad(bitsType, address);
            }
        } slotLowerer{b, i64t};
        llvm::Value* bits = ir::lowerSlotLoad(slotLowerer, vp);
        if (f && finiteInductionVars.count(f->id))
            knownFiniteDeltas.insert(bits);
        if (f && inductionRangesEnabled) {
            if (auto range = activeInductionRanges.find(f->id);
                range != activeInductionRanges.end()) {
                // Valid immediately after this for-each's implicit numeric
                // write, with no yield, opaque call or index write in body.
                llvm::Value* number = emitAsF64(bits);
                knownNum.insert(bits);
                knownNumD[bits] = number;
                knownNumOk[bits] = b.getTrue();
                recordIntInterval(bits, range->second);
                return bits;
            }
        }
        if (PromotedVar* p = findPromoted(f)) {
            if (p->numeric) {
                knownNumD[bits] = b.CreateLoad(f64t, p->dslot);
                knownNumOk[bits] = b.CreateLoad(b.getInt1Ty(), p->okslot);
            } else if (p->lazyValid) {
                lazyNumericLoads[bits] = f;
                // A straight-line repeated reporter read (e.g. x + x)
                // reuses the boxed SSA load rather than generating another
                // independent load and guarded numeric conversion.
                // No fact survives a BB change or the existing spill/reload.
                if (localReadForwardEnabled && localFactsAliasSafe &&
                    f && b.GetInsertBlock())
                    localVarFacts[f->id] = {b.GetInsertBlock(), bits, true};
            }
        } else if (profiledNumber(f)) {
            likelyNumLoads.insert(bits);
        }
        return bits;
    }
    void emitVarSet(const BlockField* f, llvm::Value* val) {
        if (PromotedVar* p = findPromoted(f)) {
            if (p->numeric) {
                // 影の不変条件: d == toNum(bits)。bits は代入された値そのもの
                // （"0" のような文字列リテラルでもよい）。
                storeNumericPair(*p, val, emitToNumF64(val));
                rememberLocalVar(f, val);
                return;
            }
        }
        struct SlotLowerer {
            IRBuilder<>& builder;
            void storeSlot(llvm::Value* address, llvm::Value* bits) {
                builder.CreateStore(bits, address);
            }
        } slotLowerer{b};
        ir::lowerSlotStore(slotLowerer, varValuePtr(f), val);
        rememberLocalVar(f, val);
        if (PromotedVar* p = findPromoted(f); p && p->lazyValid) {
            // Generic producer-to-consumer forwarding: when the producer
            // already carries a proven numeric SSA value and comparison
            // eligibility, reuse them without another Scratch cast.
            if (auto d = knownNumD.find(val); d != knownNumD.end()) {
                b.CreateStore(d->second, p->dslot);
                auto ok = knownNumOk.find(val);
                b.CreateStore(ok != knownNumOk.end()
                                  ? ok->second : numericCmpOk(val, d->second),
                              p->okslot);
                b.CreateStore(b.getTrue(), p->lazyValid);
            } else {
                b.CreateStore(b.getFalse(), p->lazyValid);
            }
        }
    }
    void emitVarChange(const BlockField* f, llvm::Value* delta,
                       bool sameScalarDelta = false) {
        if (f) localVarFacts.erase(f->id);
        llvm::Value* vp = varValuePtr(f);
        // A direct reporter reading this exact scalar was evaluated before
        // the change-by stack operation; it is the previous boxed value.
        llvm::Value* cur = sameScalarDelta ? delta : b.CreateLoad(i64t, vp);
        if (!findPromoted(f) && profiledNumber(f))
            likelyNumLoads.insert(cur);
        // xyo_var_change と同型: numCanonical(toNum(cur)+toNum(delta)).
        // emitToNumF64 never yields NaN, so a finite constant delta cannot
        // produce NaN here (inf + finite = +-inf) and canon collapses.
        llvm::Value* fb = emitToNumF64(delta);
        PromotedVar* pv = findPromoted(f);
        if (pv && pv->lazyValid) lazyNumericLoads[cur] = f;
        if (pv && pv->numeric) {
            // 影は f64。有限定数デルタなら結果は NaN になり得ない
            // （影の不変条件: d は NaN でない）ので正規化を省く。
            llvm::Value* r =
                b.CreateFAdd(b.CreateLoad(f64t, pv->dslot), fb);
            BasicBlock* finiteJoin = nullptr;
            if (auto it = finiteMemoPaths.find(fb); it != finiteMemoPaths.end()) {
                BasicBlock* finiteBB = BasicBlock::Create(lc, "memo.add.finite", fn);
                BasicBlock* checkedBB = BasicBlock::Create(lc, "memo.add.checked", fn);
                finiteJoin = BasicBlock::Create(lc, "memo.add.done", fn);
                hotBr(it->second, finiteBB, checkedBB);
                b.SetInsertPoint(finiteBB);
                storeNumericPair(*pv, b.CreateBitCast(r, i64t), r);
                b.CreateBr(finiteJoin);
                b.SetInsertPoint(checkedBB);
            }
            bool finite = false;
            if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(fb)) {
                const llvm::APFloat& ap = cf->getValueAPF();
                finite = !ap.isNaN() && !ap.isInfinity();
            }
            finite |= knownFiniteDeltas.count(delta) != 0;
            if (arithmeticDeltaFacts) finite |= arithmeticFinite(fb);
            if (finite) {
                // 影の d は決して NaN にならない（toNum の結果）ので、
                // 有限デルタでは結果も NaN にならない = 数値比較可。
                llvm::Value* rb = b.CreateBitCast(r, i64t);
                storeNumericPair(*pv, rb, r, b.getInt1(true));
            } else if (finiteJoin || memoizableListReads.count(delta)) {
                // Keep NaN repair off the ordinary list-accumulation carry,
                // including when coercion memoization is explicitly disabled.
                // Other dynamic changes retain their branchless lowering.
                BasicBlock* repair = BasicBlock::Create(lc, "change.nan", fn);
                BasicBlock* normal = BasicBlock::Create(lc, "change.number", fn);
                BasicBlock* merge = BasicBlock::Create(lc, "change.done", fn);
                llvm::Value* isNan = b.CreateFCmpUNO(r, r);
                hotBr(isNan, repair, normal, 1, 1000000);
                b.SetInsertPoint(normal);
                storeNumericPair(*pv, b.CreateBitCast(r, i64t), r,
                                 b.getInt1(true));
                b.CreateBr(merge);
                b.SetInsertPoint(repair);
                b.CreateCall(llvmc::intrinsicDeclaration(&mod, Intrinsic::sideeffect));
                b.CreateStore(cf64(0.0), pv->dslot);
                b.CreateStore(ci64(val::kNan), pv->slot);
                b.CreateStore(b.getInt1(false), pv->okslot);
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
            } else {
                // NaN 判定は 1 回で済ませる（emitCanon + emitNotNaN は
                // それぞれ fcmp uno を出す）。
                llvm::Value* isn = b.CreateFCmpUNO(r, r);
                llvm::Value* dv = b.CreateSelect(isn, cf64(0.0), r);
                llvm::Value* bv = b.CreateSelect(
                    isn, ci64(val::kNan), b.CreateBitCast(r, i64t));
                b.CreateStore(dv, pv->dslot);
                b.CreateStore(bv, pv->slot);
                llvm::Value* ok = b.CreateNot(isn);
                b.CreateStore(ok, pv->okslot);
                knownNumD[bv] = dv;
                knownNumOk[bv] = ok;
            }
            if (finiteJoin) {
                b.CreateBr(finiteJoin);
                b.SetInsertPoint(finiteJoin);
            }
            return;
        }
        llvm::Value* s = b.CreateFAdd(
            sameScalarDelta ? fb : emitToNumF64(cur), fb);
        bool finiteDelta = false;
        if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(fb)) {
            const llvm::APFloat& ap = cf->getValueAPF();
            finiteDelta = !ap.isNaN() && !ap.isInfinity();
        }
        if (pv && pv->lazyValid) {
            // change-variable-by always writes a numeric Scratch Value.
            // Forward its canonical SSA representation into the guarded
            // cache, regardless of the variable's previous type.
            emitVarSet(f, finiteDelta ? emitCanonKnownNonNaN(s)
                                     : emitCanon(s));
        } else if (finiteDelta) {
            b.CreateStore(b.CreateBitCast(s, i64t), vp);
        } else {
            b.CreateStore(emitCanon(s), vp);
        }
    }

    // --- 算術・比較の fast path -------------------------------------------


    // Shared slow path for dynamic-divisor modulo, emitted as one private
    // no-inline function per JIT/AOT module. It preserves signed wide integer
    // remainder and the original floating fallback, including signed zero.
    // The caller's i32 fast path remains contiguous in the hot function.
    llvm::Function* modWideVariableFn() {
        constexpr const char* name = "__xyo_mod_var_wide";
        if (auto* existing = mod.getFunction(name)) return existing;
        auto* f = Function::Create(
            FunctionType::get(f64t, {f64t, f64t}, false),
            GlobalValue::InternalLinkage, name, mod);
        f->addFnAttr(Attribute::NoInline);
        f->addFnAttr(Attribute::Cold);
        auto args = f->arg_begin();
        llvm::Value* fa = &*args++;
        llvm::Value* fb = &*args;
        IRBuilder<> ir(lc);
        auto* entry = BasicBlock::Create(lc, "entry", f);
        auto* check = BasicBlock::Create(lc, "check", f);
        auto* integer = BasicBlock::Create(lc, "integer", f);
        auto* slow = BasicBlock::Create(lc, "float", f);
        ir.SetInsertPoint(entry);
        const auto bounded = [&](llvm::Value* v) {
            return ir.CreateAnd(
                ir.CreateFCmpOGT(v, ConstantFP::get(f64t, -4503599627370496.0)),
                ir.CreateFCmpOLT(v, ConstantFP::get(f64t, 4503599627370496.0)));
        };
        llvm::Value* range = ir.CreateAnd(
            ir.CreateAnd(bounded(fa), bounded(fb)),
            ir.CreateAnd(ir.CreateFCmpONE(fa, ConstantFP::get(f64t, 0.0)),
                         ir.CreateFCmpONE(fb, ConstantFP::get(f64t, 0.0))));
        ir.CreateCondBr(range, check, slow);

        ir.SetInsertPoint(check);
        llvm::Value* ni = ir.CreateFPToSI(fa, i64t);
        llvm::Value* di = ir.CreateFPToSI(fb, i64t);
        llvm::Value* ints = ir.CreateAnd(
            ir.CreateFCmpOEQ(ir.CreateSIToFP(ni, f64t), fa),
            ir.CreateFCmpOEQ(ir.CreateSIToFP(di, f64t), fb));
        ir.CreateCondBr(ints, integer, slow);

        ir.SetInsertPoint(integer);
        llvm::Value* rem = ir.CreateSRem(ni, di);
        llvm::Value* adjust = ir.CreateAnd(
            ir.CreateXor(ir.CreateICmpSLT(rem, ConstantInt::get(i64t, 0)),
                         ir.CreateICmpSLT(di, ConstantInt::get(i64t, 0))),
            ir.CreateICmpNE(rem, ConstantInt::get(i64t, 0)));
        llvm::Value* wrapped = ir.CreateSelect(
            adjust, ir.CreateAdd(rem, di), rem);
        llvm::Value* numeric = ir.CreateSIToFP(wrapped, f64t);
        llvm::Value* negativeZero = ir.CreateAnd(
            ir.CreateICmpEQ(rem, ConstantInt::get(i64t, 0)),
            ir.CreateICmpSLT(ni, ConstantInt::get(i64t, 0)));
        ir.CreateRet(ir.CreateSelect(
            negativeZero, ConstantFP::get(f64t, -0.0), numeric));

        ir.SetInsertPoint(slow);
        llvm::Value* r0 = ir.CreateFRem(fa, fb);
        llvm::Value* signDiff = ir.CreateFCmpOLT(
            ir.CreateFDiv(r0, fb), ConstantFP::get(f64t, 0.0));
        ir.CreateRet(ir.CreateSelect(signDiff, ir.CreateFAdd(r0, fb), r0));
        return f;
    }

    enum class AOp { Add, Sub, Mul, Div, Mod };
    // Native round-to-even is an SSE4.1 instruction on x86-64. JavaScript
    // Math.round differs from ties-to-even only when a half-integer rounds
    // down: those cases need an exact +1 correction. Negative zero, infinities
    // and already integral values remain unchanged. Sterbenz's lemma makes
    // x - rounded exact for half-integers, even near the 2^52 boundary.
    llvm::Value* emitJsRound(llvm::Value* x) {
        llvm::Value* even = b.CreateUnaryIntrinsic(Intrinsic::roundeven, x);
        llvm::Value* isHalfDown = b.CreateFCmpOEQ(
            b.CreateFSub(x, even), cf64(0.5));
        return b.CreateSelect(isHalfDown,
                              b.CreateFAdd(even, cf64(1.0)), even);
    }

    /// xyo_add/sub/mul/div/mod と同型 — numCanonical(toNum(a) op
    /// toNum(b)) を直接 emit（helper 呼出しなし）。
    llvm::Value* emitArithNumeric(llvm::Value* fa, llvm::Value* fb,
                                 AOp aop) {
        llvm::Value* r;
        // A native integer remainder can never produce NaN. Preserve the
        // floating fallback's NaN bit as a separate SSA flag, so only the
        // cold fmod path pays for an unordered FP comparison.
        llvm::Value* modNan = nullptr;
        switch (aop) {
        case AOp::Add:
        case AOp::Sub:
        case AOp::Mul:
        case AOp::Div: {
            // Both native LLVM and WASM lower the same typed numeric SSA.
            const auto opcode = aop == AOp::Add ? ir::Op::Add :
                                aop == AOp::Sub ? ir::Op::Sub :
                                aop == AOp::Mul ? ir::Op::Mul : ir::Op::Div;
            const ir::Function& graph = ir::binaryFunction(opcode);
            struct Lowerer {
                IRBuilder<>& builder;
                llvm::Value* inputs[2];
                llvm::Value* parameter(uint32_t index) {
                    return inputs[index];
                }
                llvm::Value* binary(ir::Op op, llvm::Value* a, llvm::Value* b) {
                    switch (op) {
                    case ir::Op::Add: return builder.CreateFAdd(a, b);
                    case ir::Op::Sub: return builder.CreateFSub(a, b);
                    case ir::Op::Mul: return builder.CreateFMul(a, b);
                    case ir::Op::Div: return builder.CreateFDiv(a, b);
                    default: return nullptr;
                    }
                }
            } lowerer{b, {fa, fb}};
            r = ir::lower(graph, lowerer, 2);
            break;
        }
        case AOp::Mod: {
            // Scratch: rem = fmod(n,m); rem/m < 0 ? rem+m : rem.
            // For an integral positive constant divisor, use integer srem
            // only if the dividend is exactly a representable integer. In
            // this range srem has the same mathematical remainder as fmod.
            // Preserve negative zero when a negative multiple divides evenly.
            auto slowMod = [&]() -> llvm::Value* {
                llvm::Value* r0 = b.CreateFRem(fa, fb);
                llvm::Value* signDiff =
                    b.CreateFCmpOLT(b.CreateFDiv(r0, fb), cf64(0.0));
                return b.CreateSelect(signDiff, b.CreateFAdd(r0, fb), r0);
            };
            auto* div = dyn_cast<ConstantFP>(fb);
            double m = div ? div->getValueAPF().convertToDouble() : 0.0;
            if (inductionRangesEnabled && m >= 1 && m <= 2147483647.0 &&
                std::trunc(m) == m) {
                if (auto range = intInterval(fa);
                    range && range->lo > 0 && range->hi <= 2147483647LL) {
                    // Positive, exact signed-i32 dividend is statically
                    // proven. Remove all modulo range/integer/NaN branches.
                    llvm::Value* n = b.CreateFPToSI(fa, i32t);
                    llvm::Value* rem = b.CreateSRem(
                        n, b.getInt32(static_cast<uint32_t>(m)));
                    llvm::Value* number = b.CreateSIToFP(rem, f64t);
                    llvm::Value* boxed = emitCanonKnownNonNaN(number);
                    recordIntInterval(boxed,
                        IntInterval{0, static_cast<int64_t>(m) - 1});
                    return boxed;
                }
            }
            if (m >= 1 && m <= 2147483647.0 && std::trunc(m) == m) {
                // For an exact positive i32 dividend, a u32 remainder by a
                // compile-time constant is much cheaper than a 64-bit signed
                // remainder. Keep the general path for negatives, fractions,
                // negative zero and values outside this exact domain.
                BasicBlock* smallCheck = BasicBlock::Create(lc, "mod.c32.check", fn);
                BasicBlock* smallFast = BasicBlock::Create(lc, "mod.c32.fast", fn);
                BasicBlock* widePath = BasicBlock::Create(lc, "mod.c32.wide", fn);
                BasicBlock* finalMerge = BasicBlock::Create(lc, "mod.c32.done", fn);
                llvm::Value* smallRange = b.CreateAnd(
                    b.CreateFCmpOGT(fa, cf64(0.0)),
                    b.CreateFCmpOLE(fa, cf64(2147483647.0)));
                b.CreateCondBr(smallRange, smallCheck, widePath);
                b.SetInsertPoint(smallCheck);
                llvm::Value* smallN = modSignedI32
                    ? b.CreateFPToSI(fa, i32t)
                    : b.CreateFPToUI(fa, i32t);
                llvm::Value* exact32 = isIntegralF64(fa)
                    ? b.getInt1(true)
                    : b.CreateFCmpOEQ(b.CreateUIToFP(smallN, f64t), fa);
                b.CreateCondBr(exact32, smallFast, widePath);
                b.SetInsertPoint(smallFast);
                llvm::Value* rem32 = b.CreateURem(
                    smallN, b.getInt32(uint32_t(m)));
                llvm::Value* res32 = b.CreateUIToFP(rem32, f64t);
                b.CreateBr(finalMerge);
                b.SetInsertPoint(widePath);

                BasicBlock* checkInt = BasicBlock::Create(lc, "mod.range", fn);
                BasicBlock* fast = BasicBlock::Create(lc, "mod.int", fn);
                BasicBlock* slow = BasicBlock::Create(lc, "mod.float", fn);
                BasicBlock* merge = BasicBlock::Create(lc, "mod.done", fn);
                llvm::Value* inRange = b.CreateAnd(
                    b.CreateAnd(
                        b.CreateFCmpOGT(fa, cf64(-4503599627370496.0)),
                        b.CreateFCmpOLT(fa, cf64(4503599627370496.0))),
                    b.CreateFCmpONE(fa, cf64(0.0)));
                b.CreateCondBr(inRange, checkInt, slow);
                b.SetInsertPoint(checkInt);
                llvm::Value* n = b.CreateFPToSI(fa, i64t);
                llvm::Value* exact = isIntegralF64(fa)
                    ? b.getInt1(true)
                    : b.CreateFCmpOEQ(b.CreateSIToFP(n, f64t), fa);
                b.CreateCondBr(exact, fast, slow);
                b.SetInsertPoint(fast);
                llvm::Value* rem = b.CreateSRem(n, ci64(uint64_t(m)));
                llvm::Value* adjusted = b.CreateSelect(
                    b.CreateICmpSLT(rem, ci64(0)),
                    b.CreateAdd(rem, ci64(uint64_t(m))), rem);
                llvm::Value* integerResult = b.CreateSIToFP(adjusted, f64t);
                llvm::Value* negativeZero = b.CreateAnd(
                    b.CreateICmpEQ(rem, ci64(0)),
                    b.CreateICmpSLT(n, ci64(0)));
                llvm::Value* fastResult = b.CreateSelect(
                    negativeZero, cf64(-0.0), integerResult);
                b.CreateBr(merge);
                b.SetInsertPoint(slow);
                llvm::Value* slowResult = slowMod();
                llvm::Value* slowNan = splitModNan
                    ? b.CreateFCmpUNO(slowResult, slowResult) : nullptr;
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
                PHINode* result = b.CreatePHI(f64t, 2);
                result->addIncoming(fastResult, fast);
                result->addIncoming(slowResult, slow);
                llvm::Value* nanWide = nullptr;
                if (splitModNan) {
                    auto* phi = b.CreatePHI(b.getInt1Ty(), 2);
                    phi->addIncoming(b.getInt1(false), fast);
                    phi->addIncoming(slowNan, slow);
                    nanWide = phi;
                }
                b.CreateBr(finalMerge);
                b.SetInsertPoint(finalMerge);
                PHINode* merged = b.CreatePHI(f64t, 2);
                merged->addIncoming(res32, smallFast);
                merged->addIncoming(result, merge);
                if (splitModNan) {
                    auto* phi = b.CreatePHI(b.getInt1Ty(), 2);
                    phi->addIncoming(b.getInt1(false), smallFast);
                    phi->addIncoming(nanWide, merge);
                    modNan = phi;
                }
                r = merged;
            } else {
                // Positive integral operands fitting i32 can use the shorter
                // 32-bit remainder instruction. Range-check before fptoui;
                // all other cases retain the existing general modulo path.
                BasicBlock* smallExact = BasicBlock::Create(lc, "mod32.exact", fn);
                BasicBlock* smallFast = BasicBlock::Create(lc, "mod32.fast", fn);
                BasicBlock* wide = BasicBlock::Create(lc, "mod32.wide", fn);
                BasicBlock* both = BasicBlock::Create(lc, "mod32.merge", fn);
                llvm::Value* inSmallRange = b.CreateAnd(
                    b.CreateAnd(b.CreateFCmpOGT(fa, cf64(0.0)),
                                b.CreateFCmpOLE(fa, cf64(2147483647.0))),
                    b.CreateAnd(b.CreateFCmpOGE(fb, cf64(1.0)),
                                b.CreateFCmpOLE(fb, cf64(2147483647.0))));
                b.CreateCondBr(inSmallRange, smallExact, wide);
                b.SetInsertPoint(smallExact);
                llvm::Value* smallN = modSignedI32
                    ? b.CreateFPToSI(fa, i32t)
                    : b.CreateFPToUI(fa, i32t);
                llvm::Value* smallD = modSignedI32
                    ? b.CreateFPToSI(fb, i32t)
                    : b.CreateFPToUI(fb, i32t);
                llvm::Value* smallIntegers = b.CreateAnd(
                    isIntegralF64(fa) ? b.getInt1(true)
                        : b.CreateFCmpOEQ(b.CreateUIToFP(smallN, f64t), fa),
                    isIntegralF64(fb) ? b.getInt1(true)
                        : b.CreateFCmpOEQ(b.CreateUIToFP(smallD, f64t), fb));
                b.CreateCondBr(smallIntegers, smallFast, wide);
                b.SetInsertPoint(smallFast);
                llvm::Value* smallResult = b.CreateUIToFP(
                    b.CreateURem(smallN, smallD), f64t);
                b.CreateBr(both);
                b.SetInsertPoint(wide);
                if (modOutlineWide) {
                    llvm::Value* wideResult = b.CreateCall(
                        modWideVariableFn(), {fa, fb});
                    llvm::Value* wideNan = splitModNan
                        ? b.CreateFCmpUNO(wideResult, wideResult) : nullptr;
                    b.CreateBr(both);
                    b.SetInsertPoint(both);
                    auto* joined = b.CreatePHI(f64t, 2);
                    joined->addIncoming(smallResult, smallFast);
                    joined->addIncoming(wideResult, wide);
                    if (splitModNan) {
                        auto* flag = b.CreatePHI(b.getInt1Ty(), 2);
                        flag->addIncoming(b.getInt1(false), smallFast);
                        flag->addIncoming(wideNan, wide);
                        modNan = flag;
                    }
                    r = joined;
                } else {
                b.SetInsertPoint(wide);
                // Variable divisor: exact integer operands also admit a
                // native signed remainder. Both conversions are performed
                // only after a finite-range check (fptosi would otherwise
                // produce poison for NaN/Infinity/out-of-range values).
                BasicBlock* checkInt = BasicBlock::Create(lc, "mod.var.range", fn);
                BasicBlock* fast = BasicBlock::Create(lc, "mod.var.int", fn);
                BasicBlock* slow = BasicBlock::Create(lc, "mod.var.float", fn);
                BasicBlock* merge = BasicBlock::Create(lc, "mod.var.done", fn);
                auto inSignedRange = [&](llvm::Value* x) {
                    return b.CreateAnd(
                        b.CreateFCmpOGT(x, cf64(-4503599627370496.0)),
                        b.CreateFCmpOLT(x, cf64(4503599627370496.0)));
                };
                llvm::Value* range = b.CreateAnd(
                    b.CreateAnd(inSignedRange(fa), inSignedRange(fb)),
                    b.CreateAnd(b.CreateFCmpONE(fa, cf64(0.0)),
                                b.CreateFCmpONE(fb, cf64(0.0))));
                b.CreateCondBr(range, checkInt, slow);
                b.SetInsertPoint(checkInt);
                llvm::Value* ni = b.CreateFPToSI(fa, i64t);
                llvm::Value* di = b.CreateFPToSI(fb, i64t);
                llvm::Value* ints = b.CreateAnd(
                    isIntegralF64(fa) ? b.getInt1(true)
                        : b.CreateFCmpOEQ(b.CreateSIToFP(ni, f64t), fa),
                    isIntegralF64(fb) ? b.getInt1(true)
                        : b.CreateFCmpOEQ(b.CreateSIToFP(di, f64t), fb));
                b.CreateCondBr(ints, fast, slow);
                b.SetInsertPoint(fast);
                llvm::Value* rem = b.CreateSRem(ni, di);
                llvm::Value* neg = b.CreateXor(
                    b.CreateICmpSLT(rem, ci64(0)),
                    b.CreateICmpSLT(di, ci64(0)));
                llvm::Value* adjust = b.CreateAnd(
                    neg, b.CreateICmpNE(rem, ci64(0)));
                llvm::Value* wrapped = b.CreateSelect(
                    adjust, b.CreateAdd(rem, di), rem);
                llvm::Value* result = b.CreateSIToFP(wrapped, f64t);
                llvm::Value* negZero = b.CreateAnd(
                    b.CreateICmpEQ(rem, ci64(0)),
                    b.CreateICmpSLT(ni, ci64(0)));
                result = b.CreateSelect(negZero, cf64(-0.0), result);
                b.CreateBr(merge);
                b.SetInsertPoint(slow);
                llvm::Value* fallback = slowMod();
                llvm::Value* slowNan = splitModNan
                    ? b.CreateFCmpUNO(fallback, fallback) : nullptr;
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
                PHINode* phi = b.CreatePHI(f64t, 2);
                phi->addIncoming(result, fast);
                phi->addIncoming(fallback, slow);
                llvm::Value* nanWide = nullptr;
                if (splitModNan) {
                    auto* flag = b.CreatePHI(b.getInt1Ty(), 2);
                    flag->addIncoming(b.getInt1(false), fast);
                    flag->addIncoming(slowNan, slow);
                    nanWide = flag;
                }
                b.CreateBr(both);
                b.SetInsertPoint(both);
                PHINode* joined = b.CreatePHI(f64t, 2);
                joined->addIncoming(smallResult, smallFast);
                joined->addIncoming(phi, merge);
                if (splitModNan) {
                    auto* flag = b.CreatePHI(b.getInt1Ty(), 2);
                    flag->addIncoming(b.getInt1(false), smallFast);
                    flag->addIncoming(nanWide, merge);
                    modNan = flag;
                }
                r = joined;
                }
            }
            break;
        }
        }
        // Range transfer functions for a bounded expression, including a
        // modulo whose dividend may be infinity/NaN. Canonicalization repairs
        // an invalid remainder to zero; a finite modulus bounds any other
        // result by abs(modulus), even after Scratch's sign adjustment.
        std::optional<double> finiteBound;
        if (arithmeticRangeFacts) {
            const auto boundA = upperBound(fa);
            const auto boundB = upperBound(fb);
            long double bound = -1.0L;
            if (aop == AOp::Mod && boundB) {
                bound = *boundB;
            } else if (boundA && boundB) {
                switch (aop) {
                case AOp::Add: case AOp::Sub:
                    bound = static_cast<long double>(*boundA) + *boundB;
                    break;
                case AOp::Mul:
                    bound = static_cast<long double>(*boundA) * *boundB;
                    break;
                case AOp::Div:
                    if (auto* divisor = dyn_cast<ConstantFP>(fb)) {
                        const double d = divisor->getValueAPF().convertToDouble();
                        if (std::isfinite(d) && d != 0.0)
                            bound = static_cast<long double>(*boundA) / std::abs(d);
                    }
                    break;
                case AOp::Mod: break;
                }
            }
            // Avoid claims near the overflow boundary. nextafter rounds any
            // converted long-double bound up to the next representable double.
            if (bound >= 0.0L &&
                bound < static_cast<long double>(
                            std::numeric_limits<double>::max()) / 4.0L) {
                finiteBound = std::nextafter(static_cast<double>(bound),
                                             std::numeric_limits<double>::infinity());
            }
        }
        // Both operands arrived through emitToNumF64(), so neither is NaN.
        // IEEE-754 add/sub with one finite operand cannot produce NaN;
        // multiply/divide with a finite nonzero factor/divisor cannot either.
        // Do not use fast-math: infinity, signed zero and overflow stay exact.
        // The general modulo path still needs canonicalization (fmod(inf,m)
        // is NaN), but its numeric coercion is always finite after repair.
        const auto integerRange = inductionRangesEnabled
            ? arithmeticIntInterval(fa, fb, static_cast<int>(aop))
            : std::nullopt;
        if (modNan) {
            // The integer paths have a constant false flag. Only the floating
            // fallback computes isNaN, while preserving NaN-box canonical form
            // and its toNumber zero shadow.
            llvm::Value* boxed = b.CreateSelect(modNan, ci64(val::kNan),
                                                 b.CreateBitCast(r, i64t));
            llvm::Value* number = b.CreateSelect(modNan, cf64(0.0), r);
            knownNum.insert(boxed);
            knownNumD[boxed] = number;
            knownNumOk[boxed] = b.CreateNot(modNan);
            finiteArithmetic.insert(number);
            if (finiteBound) noteFiniteUpperBound(boxed, *finiteBound);
            if (integerRange) recordIntInterval(boxed, *integerRange);
            if (isIntegralF64(fa) && isIntegralF64(fb))
                noteIntegralBox(boxed);
            return boxed;
        }
        const bool integralResult =
            (aop == AOp::Add || aop == AOp::Sub ||
             aop == AOp::Mul || aop == AOp::Mod)
                ? isIntegralF64(fa) && isIntegralF64(fb)
                : (aop == AOp::Div && isIntegralF64(fa) &&
                   isUnitDivisor(fb));
        const bool finiteA = arithmeticFinite(fa);
        const bool finiteB = arithmeticFinite(fb);
        const bool noNaN =
            (aop == AOp::Add || aop == AOp::Sub) ? (finiteA || finiteB) :
            aop == AOp::Mul ? (finiteA && finiteB) ||
                                 arithmeticFiniteNonzero(fa) ||
                                 arithmeticFiniteNonzero(fb) :
            aop == AOp::Div ? arithmeticFiniteNonzero(fb) :
            false;
        if (noNaN && safeArithCanon) {
            llvm::Value* result = emitCanonKnownNonNaN(r);
            if (finiteBound) noteFiniteUpperBound(result, *finiteBound);
            if (integralResult) noteIntegralBox(result);
            if (integerRange) recordIntInterval(result, *integerRange);
            return result;
        }
        llvm::Value* result = emitCanon(r);
        if (finiteBound) noteFiniteUpperBound(result, *finiteBound);
        if (integralResult) noteIntegralBox(result);
        if (integerRange) recordIntInterval(result, *integerRange);
        if (aop == AOp::Mod)
            finiteArithmetic.insert(knownNumD[result]);
        return result;
    }

    // Exact scalar reporter CSE, not algebraic simplification of arbitrary
    // Scratch expressions. Resolving the ID in the current target ensures
    // that the two reporters denote one Variable (never name-only aliases).
    // No reporter or helper call may interleave the two plain reads.
    bool sameScalarReporters(Block* parent, const char* left,
                             const char* right) const {
        const char* option = std::getenv("XYO_JIT_DUPLICATE_SCALAR_NUM");
        if (!option || option[0] != '1' || !blocks || !target || !parent)
            return false;
        const BlockInput* a = parent->inputs.find(left);
        const BlockInput* bInput = parent->inputs.find(right);
        if (!a || !bInput) return false;
        const std::string& aId = a->block.empty() ? a->shadow : a->block;
        const std::string& bId = bInput->block.empty()
            ? bInput->shadow : bInput->block;
        const Block* aBlock = blocks->getBlock(aId);
        const Block* bBlock = blocks->getBlock(bId);
        if (!aBlock || !bBlock ||
            aBlock->opcode != "data_variable" ||
            bBlock->opcode != "data_variable")
            return false;
        const auto fa = aBlock->fields.find("VARIABLE");
        const auto fb = bBlock->fields.find("VARIABLE");
        if (fa == aBlock->fields.end() || fb == bBlock->fields.end() ||
            fa->second.id.empty() || fa->second.id != fb->second.id)
            return false;
        const Variable* v = target->lookupVariableById(fa->second.id);
        return v && v->type == Variable::kScalarType;
    }

    bool sameScalarChangeInput(Block* parent,
                               const BlockField* variable) const {
        const char* option = std::getenv("XYO_JIT_DUPLICATE_SCALAR_NUM");
        if (!option || option[0] != '1' ||
            !blocks || !target || !parent || !variable ||
            variable->id.empty())
            return false;
        const BlockInput* input = parent->inputs.find("VALUE");
        if (!input) return false;
        const std::string& id = input->block.empty()
            ? input->shadow : input->block;
        const Block* source = blocks->getBlock(id);
        if (!source || source->opcode != "data_variable") return false;
        const auto field = source->fields.find("VARIABLE");
        if (field == source->fields.end() ||
            field->second.id != variable->id)
            return false;
        const Variable* resolved = target->lookupVariableById(variable->id);
        return resolved && resolved->type == Variable::kScalarType;
    }

    uint64_t duplicateScalarNumericReads = 0;
    uint64_t duplicateScalarChanges = 0;

    llvm::Value* emitArith(Block* blk, const char* in1, const char* in2,
                           AOp aop) {
        if (sameScalarReporters(blk, in1, in2)) {
            // The two direct variable reporters are pure and read the same
            // resolved slot with no scheduler handoff. Preserve Scratch
            // number coercion exactly (numeric strings, booleans and NaN);
            // only share the immutable boxed load and its conversion.
            llvm::Value* bits = emitInputOf(blk, in1);
            llvm::Value* number = emitToNumF64(bits);
            ++duplicateScalarNumericReads;
            return emitArithNumeric(number, number, aop);
        }
        llvm::Value* a = emitInputOf(blk, in1);
        llvm::Value* bv = emitInputOf(blk, in2);
        // C++ function argument evaluation order is unspecified. Emit both
        // conversions in Scratch's left-to-right evaluation order.
        llvm::Value* fa = emitToNumF64(a);
        llvm::Value* fb = emitToNumF64(bv);
        return emitArithNumeric(fa, fb, aop);
    }

    /// cast::compare 意味での数値変換。*okOut=true なら f64 が有効
    /// （num(kNan除く)|true|false|numState==1 の数値キャッシュ済み str）。
    /// それ以外は NaN→文字列比較の領域なので helper に任せる。
    /// その SSA が数値だと分かっているか（数値影・既知数値・数値定数）。
    bool isNumericOperand(llvm::Value* v) {
        return knownNumD.count(v) || knownNum.count(v) || constNumV(v);
    }

    /// foldLiteral: 相手側が数値と分かっているときに限り、数値として
    /// 解釈できる文字列リテラルをコンパイル時に畳む。相手が数値なら
    /// cast::compare は必ず数値比較になるので、リテラル側の numState
    /// 経路（空白のみ = state 3 で文字列比較）を飛ばしてよい。
    llvm::Value* emitCmpNumF64(llvm::Value* bits, llvm::Value** okOut,
                               bool foldLiteral = false) {
        // 昇格した変数の影: (bits, d=toNum(bits)) の d をそのまま使う。
        // ただし **d が非 NaN でも数値比較になるとは限らない** — 昇格は
        // 「ループ内のアクセスが inline で完結する」だけで型は証明して
        // いないので bits は任意の文字列でありうる（toNum(" ")==0 だが
        // cast::compare は文字列比較）。可否は変数ごとに持ち
        // （numericCmpOk）、書き込み側で更新しておく — 比較のたびに
        // タグ検査を出すと分岐が増えてループが伸びる。
        if (auto it = knownNumD.find(bits); it != knownNumD.end()) {
            auto ok = knownNumOk.find(bits);
            *okOut = ok != knownNumOk.end()
                         ? ok->second
                         : numericCmpOk(bits, it->second);
            return it->second;
        }
        if (foldLiteral) {
            if (auto it = literalCmpNums.find(bits);
                it != literalCmpNums.end() && !std::isnan(it->second)) {
                *okOut = b.getInt1(true);
                return cf64(it->second);
            }
        }
        if (knownNum.count(bits)) {
            *okOut = b.CreateICmpNE(bits, ci64(val::kNan));
            return emitAsF64(bits);
        }
        if (likelyNumLoads.count(bits)) {
            // Only a non-NaN numeric payload is safe for native comparison.
            // A mismatched type makes okOut false; the caller invokes the
            // generic Scratch comparison (including numeric strings).
            llvm::Value* d = emitAsF64(bits);
            *okOut = b.CreateAnd(
                b.CreateICmpSLT(bits, ci64(val::kNan)),
                b.CreateFCmpORD(d, d));
            return d;
        }
        // 数値として解釈できる文字列リテラル（"1000" など）は比較を
        // コンパイル時に畳む。literalNums は cast::toNumber の結果で、
        // 空白のみ・非数値は NaN なので fold しない（Scratch は
        // その場合だけ文字列比較へ落ちる）。
        // Plain IEEE double (see emitToNumF64): skip the tag mapping cascade.
        // Comparisons need the raw double (NaN stays NaN), not toNum().
        BasicBlock* dblBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* tagBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* outBB = BasicBlock::Create(lc, "", fn);
        hotBr(b.CreateICmpSLT(bits, ci64(val::kNan)), dblBB, tagBB);
        b.SetInsertPoint(dblBB);
        llvm::Value* dbl = emitAsF64(bits);
        b.CreateBr(outBB);
        b.SetInsertPoint(tagBB);
        llvm::Value* isn = emitIsNum(bits);
        llvm::Value* isNan = b.CreateICmpEQ(bits, ci64(val::kNan));
        llvm::Value* okNum = b.CreateAnd(isn, b.CreateNot(isNan));
        llvm::Value* isTrue = b.CreateICmpEQ(bits, ci64(val::kTrue));
        llvm::Value* isFalse = b.CreateICmpEQ(bits, ci64(val::kFalse));
        llvm::Value* ok0 =
            b.CreateOr(okNum, b.CreateOr(isTrue, isFalse));
        llvm::Value* d0 = b.CreateSelect(
            isTrue, cf64(1.0),
            b.CreateSelect(isFalse, cf64(0.0), emitAsF64(bits)));
        BasicBlock* fastBB = b.GetInsertBlock();
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strHitBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBadBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* otherBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        hotBr(ok0, merge, slowBB);
        b.SetInsertPoint(slowBB);
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(isStr, strBB, otherBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(bits, ci64(val::kPtrMask)), ptrt);
        llvm::Value* st = loadSStrCache(i32t, sp, kSStrNumStateOff, true);
        // state1(数値) | state4(Infinity リテラル) は数値比較可
        llvm::Value* isOne = b.CreateOr(
            b.CreateICmpEQ(st, ci32(1)), b.CreateICmpEQ(st, ci32(4)));
        b.CreateCondBr(isOne, strHitBB, strBadBB);
        b.SetInsertPoint(strHitBB);
        llvm::Value* nc = loadSStrCache(f64t, sp, kSStrNumCacheOff, false);
        b.CreateBr(merge);
        b.SetInsertPoint(strBadBB);
        b.CreateBr(merge);
        b.SetInsertPoint(otherBB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* ok = b.CreatePHI(b.getInt1Ty(), 4);
        ok->addIncoming(b.getInt1(true), fastBB);
        ok->addIncoming(b.getInt1(true), strHitBB);
        ok->addIncoming(b.getInt1(false), strBadBB);
        ok->addIncoming(b.getInt1(false), otherBB);
        PHINode* d = b.CreatePHI(f64t, 4);
        d->addIncoming(d0, fastBB);
        d->addIncoming(nc, strHitBB);
        d->addIncoming(d0, strBadBB);
        d->addIncoming(d0, otherBB);
        b.CreateBr(outBB);
        b.SetInsertPoint(outBB);
        PHINode* okOutP = b.CreatePHI(b.getInt1Ty(), 2);
        okOutP->addIncoming(b.getInt1(true), dblBB);
        okOutP->addIncoming(ok, merge);
        PHINode* dOut = b.CreatePHI(f64t, 2);
        dOut->addIncoming(dbl, dblBB);
        dOut->addIncoming(d, merge);
        *okOut = okOutP;
        return dOut;
    }

    /// operator_mathop の f64 実装（xyo_mathop と同型）。
    /// tan(6) はエッジケース（±90/270）があるため helper に委譲。
    llvm::Value* emitMathop(int32_t fn, llvm::Value* x) {
        constexpr double kPi = 3.14159265358979323846;
        auto uni = [&](Intrinsic::ID id) {
            return b.CreateUnaryIntrinsic(id, x);
        };
        auto libm = [&](const char* n) {
            return b.CreateCall(decl(n, f64t, {f64t}), {x});
        };
        switch (fn) {
        case 0: return uni(Intrinsic::fabs);
        case 1: return uni(Intrinsic::floor);
        case 2: return uni(Intrinsic::ceil);
        case 3: return uni(Intrinsic::sqrt);
        case 4: case 5: {
            // round(f(pi*x/180)*1e10)/1e10 — math-util 準拠。
            // 掛け順は Scratch と同じ (pi*x)/180（x*(pi/180) だと
            // x=1e308 で overflow せず sin/cos が NaN にならない）。
            llvm::Value* rad =
                b.CreateFDiv(b.CreateFMul(cf64(kPi), x), cf64(180.0));
            llvm::Value* v = b.CreateCall(
                decl(fn == 4 ? "sin" : "cos", f64t, {f64t}), {rad});
            v = b.CreateUnaryIntrinsic(Intrinsic::round,
                                       b.CreateFMul(v, cf64(1e10)));
            return b.CreateFDiv(v, cf64(1e10));
        }
        case 7: case 8: case 9: {
            // Scratch: (asin(x) * 180) / pi。定数を先に畳まない。
            // V8 の fdlibm と最終ビットまで一致させるため、libc ではなく
            // v8math ヘルパを呼ぶ（interp と共通）。
            const char* f = fn == 7 ? "xyo_asin" : fn == 8 ? "xyo_acos" : "xyo_atan";
            return b.CreateFDiv(b.CreateFMul(call1(f, f64t, x), cf64(180.0)),
                                cf64(kPi));
        }
        case 10: return call1("xyo_log", f64t, x);
        case 11:
            return b.CreateFDiv(call1("xyo_log", f64t, x),
                                cf64(2.302585092994046));
        case 12: return call1("xyo_exp", f64t, x);
        case 13:
            return b.CreateCall(decl("pow", f64t, {f64t, f64t}),
                                {cf64(10.0), x});
        default:  // 6=tan 他 → helper
            return call3("xyo_mathop", f64t, ctxArg, ci32(fn), x);
        }
    }

    /// 手続き引数読み出し — レジスタ引数 ABI により仮引数 SSA を
    /// 直接返す。OOB/手続き外（argIndex は手続き内でのみ設定される
    /// ため通常は到達しない）は undefined 値を保持するグローバル。
    llvm::Value* emitArgGet(int64_t idx) {
        if (idx >= 0 && size_t(idx) < curProcArgs.size())
            return curProcArgs[size_t(idx)];
        auto* g = new GlobalVariable(
            mod, i64t, true, GlobalValue::PrivateLinkage,
            ci64(val::kUndefined), "arg_oob");
        return b.CreateLoad(i64t, g);
    }

    /// operator_letter_of の fast path: LETTER が num・STRING が str
    /// タグ・範囲内・ASCII(<=127) なら共有キャッシュ SStr を直接返す。
    /// それ以外（非数値 idx・非 str・OOB・非 ASCII・NaN）は全て
    /// xyo_letterof（rt::letterOf と同一意味論）へ分岐。
    llvm::Value* emitLetterOf(Block* blk) {
        llvm::Value* l = emitInputOf(blk, "LETTER");
        llvm::Value* s = emitInputOf(blk, "STRING");
        BasicBlock* numBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* chrBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* cacheBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        BasicBlock* numLetterBB = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(emitIsNum(l), numBB, slowBB);
        b.SetInsertPoint(numBB);
        b.CreateCondBr(b.CreateICmpEQ(
                           b.CreateLShr(s, ci64(48)),
                           ci64(val::kTagStr >> 48)),
                       strBB, slowBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(s, ci64(val::kPtrMask)), ptrt);
        // 連結ノードは本文が別確保 — helper（view() が flatten する）へ。
        b.CreateCondBr(emitIsConsStr(sp), slowBB, numLetterBB);
        b.SetInsertPoint(numLetterBB);
        // i0 = trunc(toNumber(letter))-1 — |x|>1e18 は整数域外になる
        // ので先にクランプして fptosi の poison を避ける
        llvm::Value* lf = emitToNumF64(l);
        llvm::Value* lc2 = b.CreateSelect(
            b.CreateFCmpOLT(lf, cf64(-1e18)), cf64(-1e18),
            b.CreateSelect(b.CreateFCmpOGT(lf, cf64(1e18)),
                           cf64(1e18), lf));
        llvm::Value* i0 = b.CreateSub(
            b.CreateFPToSI(lc2, i64t), ci64(1));
        llvm::Value* len = b.CreateZExt(
            b.CreateLoad(i32t,
                         b.CreateConstGEP1_32(i8t, sp, kSStrLenOff)),
            i64t);
        // i0<0 は巨大正数になり OOB 判定で弾かれる（unsigned 比較）
        b.CreateCondBr(b.CreateICmpULT(i0, len), chrBB, slowBB);
        b.SetInsertPoint(chrBB);
        llvm::Value* cp16 = b.CreateLoad(
            i16t,
            b.CreateGEP(i8t, sp,
                        b.CreateAdd(ci64(kSStrDataOff),
                                    b.CreateShl(i0, ci64(1)))));
        b.CreateCondBr(b.CreateICmpULT(cp16, b.getInt16(128)),
                       cacheBB, slowBB);
        b.SetInsertPoint(cacheBB);
        static SStr* const* kCache = xyo_ascii_cache();
        llvm::Value* ent = b.CreateLoad(
            i64t,
            b.CreateGEP(i64t,
                        slotPtr(AotSlotKind::AsciiCache, "", kCache),
                        b.CreateZExt(cp16, i64t)));
        llvm::Value* fastBits = b.CreateOr(ent, ci64(val::kTagStr));
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* hv = call2("xyo_letterof", i64t, l, s);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i64t, 2);
        p->addIncoming(fastBits, cacheBB);
        p->addIncoming(hv, slowBB);
        return p;
    }

    /// operator_length の fast path: 文字列タグなら len を直接読む。
    /// 空文字列シングルトンは 0。それ以外は xyo_strlen へ（数値や真偽値の
    /// 文字列化は確保を伴うので helper に任せる）。
    llvm::Value* emitLength(Block* blk) {
        llvm::Value* s = emitInputOf(blk, "STRING");
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        llvm::Value* empty = b.CreateICmpEQ(s, ci64(val::kEmptyStr));
        llvm::Value* tagged = b.CreateICmpEQ(b.CreateLShr(s, ci64(48)),
                                             ci64(val::kTagStr >> 48));
        b.CreateCondBr(b.CreateOr(empty, tagged), strBB, slowBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(s, ci64(val::kPtrMask)), ptrt);
        // A select does not guard its operands: branch around the load for
        // the empty singleton, which has no SStr payload.
        BasicBlock* emptyBB = b.GetInsertBlock();
        BasicBlock* loadBB = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(empty, merge, loadBB);
        b.SetInsertPoint(loadBB);
        llvm::Value* len = b.CreateZExt(b.CreateLoad(i32t,
            b.CreateConstGEP1_32(i8t, sp, kSStrLenOff)), i64t);
        // Numbers are the raw double bits (the NaN-box tag is the leftmost
        // 0x7FF8 group, which a u32 length can never produce).
        llvm::Value* fast =
            b.CreateBitCast(b.CreateUIToFP(len, f64t), i64t);
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* hv = b.CreateCall(decl("xyo_strlen", i64t, {i64t}), {s});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i64t, 3);
        p->addIncoming(ci64(Value::num(0).bits), emptyBB);
        p->addIncoming(fast, loadBB);
        p->addIncoming(hv, slowBB);
        return p;
    }

    /// Recognize a literal from the input AST, rather than emitted pointer bits:
    /// AOT string literals are slot loads, not ConstantInts. Only a text shadow
    /// is accepted; arbitrary reporters with a TEXT field must still execute.
    std::optional<std::u16string> eqAsciiWord(Block* blk, const char* input) {
        const BlockInput* in = blk->inputs.find(input);
        if (!in) return std::nullopt;
        const std::string& id = in->block.empty() ? in->shadow : in->block;
        Block* literal = id.empty() ? nullptr : getBlock(id);
        if (!literal || !literal->shadow || literal->opcode != "text")
            return std::nullopt;
        const BlockField* field = findField(literal, "TEXT");
        if (!field || !field->value.isString() || !field->value.asStrPtr())
            return std::nullopt;
        auto text = field->value.asStrPtr()->view();
        if (text.empty() || text.size() > 8) return std::nullopt;
        std::u16string word(text);
        for (char16_t& c : word) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            // The old fast path accepted only letters. Brainfuck's hot
            // operators are ASCII punctuation ('+', '-', '<', '>', '[', ']').
            // Reject non-printable/Unicode literals, which may case-fold to
            // different lengths; numeric-looking literals remain excluded by
            // the toNumberOrNaN test below.
            if (c < 0x21 || c > 0x7e) return std::nullopt;
        }
        // Test the ORIGINAL spelling: "Infinity" is numeric but "infinity"
        // is not. Numeric literals must retain Scratch's numeric comparison
        // (e.g. Infinity == "1e999"), not just case-insensitive text matching.
        if (!std::isnan(cast::toNumberOrNaN(field->value))) return std::nullopt;
        return word;
    }

    /// Equality with a nonnumeric, printable ASCII literal of 1..8 chars
    /// (including Brainfuck operators and 3D2's "n"/"none"). A different
    /// first ASCII unit cannot lowercase to the literal:
    /// lowercasing never removes that ASCII prefix. Otherwise inline comparison
    /// requires equal length and ALL ASCII units. Unicode anywhere, or unequal
    /// length after a matching prefix, falls back without assuming anything
    /// about Unicode length or mappings (Kelvin sign, dotted I, surrogates).
    /// Only SINGLE letters can reject non-heap Values: numeric cast of the
    /// literal is NaN, forcing string comparison; finite numbers contain digits,
    /// NaN/Infinity/true/false/undefined/r,g,b have multiple characters, and the
    /// inline empty string has none. Punctuation and longer words retain the
    /// helper for non-string values because e.g. bool
    /// true == "true" and numeric NaN == "NaN" must still succeed.
    llvm::Value* emitEqAsciiWord(llvm::Value* a, llvm::Value* bv,
                               llvm::Value* dynamic, std::u16string_view word) {
        BasicBlock* strBB = newBB("eq.lit.str");
        BasicBlock* firstBB = newBB("eq.lit.first");
        BasicBlock* asciiBB = newBB("eq.lit.ascii");
        BasicBlock* lenBB = newBB("eq.lit.len");
        BasicBlock* wordBB = newBB("eq.lit.word");
        BasicBlock* compareBB = newBB("eq.lit.compare");
        BasicBlock* lenZeroBB = newBB("eq.lit.lenzero");
        BasicBlock* ret0BB = newBB("eq.lit.no");
        BasicBlock* ret1BB = newBB("eq.lit.yes");
        BasicBlock* helperBB = newBB("eq.lit.slow");
        BasicBlock* merge = newBB("eq.lit.merge");
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(dynamic, ci64(48)), ci64(val::kTagStr >> 48));
        // For a one-letter literal no non-string Scratch Value can compare
        // equal. With punctuation, retain xyo_eq for non-string values
        // (including extensions/undefined) rather than assuming their cast.
        const bool singleLetter = word.size() == 1 &&
            word[0] >= 'a' && word[0] <= 'z';
        b.CreateCondBr(isStr, strBB, singleLetter ? ret0BB : helperBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(dynamic, ci64(val::kPtrMask)), ptrt);
        llvm::Value* len = b.CreateLoad(i32t, fld(sp, kSStrLenOff));
        b.CreateCondBr(emitIsConsStr(sp), helperBB, lenZeroBB);
        b.SetInsertPoint(lenZeroBB);
        b.CreateCondBr(b.CreateICmpEQ(len, ci32(0)), ret0BB, firstBB);
        b.SetInsertPoint(firstBB);
        llvm::Value* first = b.CreateLoad(i16t, fld(sp, kSStrDataOff));
        b.CreateCondBr(b.CreateICmpULT(first, b.getInt16(128)),
                       asciiBB, helperBB);
        b.SetInsertPoint(asciiBB);
        llvm::Value* upper = b.CreateAnd(
            b.CreateICmpUGE(first, b.getInt16('A')),
            b.CreateICmpULE(first, b.getInt16('Z')));
        llvm::Value* lower = b.CreateSelect(
            upper, b.CreateAdd(first, b.getInt16('a' - 'A')), first);
        b.CreateCondBr(b.CreateICmpEQ(lower, b.getInt16(word[0])),
                       lenBB, ret0BB);
        b.SetInsertPoint(lenBB);
        // Do not load any trailing code units until the length check dominates.
        b.CreateCondBr(b.CreateICmpEQ(len, ci32(uint32_t(word.size()))),
                       wordBB, helperBB);
        b.SetInsertPoint(wordBB);
        llvm::Value* allAscii = b.getTrue();
        llvm::Value* matches = b.getTrue(); // first unit was already matched
        for (size_t i = 1; i < word.size(); ++i) {
            llvm::Value* unit = b.CreateLoad(
                i16t, fld(sp, kSStrDataOff + uint32_t(i * sizeof(char16_t))));
            allAscii = b.CreateAnd(allAscii,
                b.CreateICmpULT(unit, b.getInt16(128)));
            llvm::Value* isUpper = b.CreateAnd(
                b.CreateICmpUGE(unit, b.getInt16('A')),
                b.CreateICmpULE(unit, b.getInt16('Z')));
            llvm::Value* folded = b.CreateSelect(
                isUpper, b.CreateAdd(unit, b.getInt16('a' - 'A')), unit);
            matches = b.CreateAnd(matches,
                b.CreateICmpEQ(folded, b.getInt16(word[i])));
        }
        // A mismatch in one ASCII unit must not hide Unicode in another unit:
        // use the original helper whenever ANY trailing unit is non-ASCII.
        b.CreateCondBr(allAscii, compareBB, helperBB);
        b.SetInsertPoint(compareBB);
        b.CreateCondBr(matches, ret1BB, ret0BB);
        b.SetInsertPoint(ret0BB);
        b.CreateBr(merge);
        b.SetInsertPoint(ret1BB);
        b.CreateBr(merge);
        b.SetInsertPoint(helperBB);
        llvm::Value* slow = call2("xyo_eq", i32t, a, bv);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* result = b.CreatePHI(i32t, 3);
        result->addIncoming(ci32(0), ret0BB);
        result->addIncoming(ci32(1), ret1BB);
        result->addIncoming(slow, helperBB);
        return result;
    }

    // Cross-runtime helper calls are expensive when a loop repeatedly
    // compares already-parsed numeric strings. Their immutable SStr caches
    // contain both the coercion class and IEEE number; compare these directly
    // in LLVM IR. A missing parse result, lexical-zero, NaN, non-string or
    // nonnumeric operand always keeps the full Scratch comparison semantics.
    llvm::Value* emitCachedNumericStringPair(
        llvm::Value* lhs, llvm::Value* rhs, const char* fallback,
        llvm::CmpInst::Predicate predicate, bool allowSimd = false) {
        const char* setting = std::getenv("XYO_JIT_STRING_PAIR_NUMERIC");
        if (setting && setting[0] == '0')
            return call2(fallback, i32t, lhs, rhs);

        const char* asciiSetting = std::getenv("XYO_JIT_STRING_PAIR_ASCII");
        const bool inlineAscii = !asciiSetting || asciiSetting[0] != '0';
        BasicBlock* check = newBB("cmp.pair.cache");
        BasicBlock* numeric = newBB("cmp.pair.numeric");
        BasicBlock* lexical = newBB("cmp.pair.lexical");
        const char* simdSetting = std::getenv("XYO_JIT_EQ_ASCII_SIMD");
        const bool useSimd = inlineAscii && allowSimd &&
            predicate == CmpInst::FCMP_OEQ &&
            (!simdSetting || simdSetting[0] != '0');
        BasicBlock* simdCheck = useSimd ? newBB("eq.simd.check") : nullptr;
        BasicBlock* simdBody = useSimd ? newBB("eq.simd.body") : nullptr;
        BasicBlock* simdTail = useSimd ? newBB("eq.simd.tail") : nullptr;
        BasicBlock* simdDone = useSimd ? newBB("eq.simd.done") : nullptr;
        BasicBlock* shortCheck = inlineAscii ? newBB("cmp.pair.short.check") : nullptr;
        BasicBlock* scanHead = inlineAscii ? newBB("cmp.pair.ascii.scan") : nullptr;
        BasicBlock* scanDone = inlineAscii ? newBB("cmp.pair.ascii.done") : nullptr;
        BasicBlock* unicodeCheck = inlineAscii ? newBB("cmp.pair.fold.check") : nullptr;
        BasicBlock* unicodeDirect = inlineAscii ? newBB("cmp.pair.fold.fast") : nullptr;
        BasicBlock* asciiDirect = inlineAscii ? newBB("cmp.pair.ascii.fast") : nullptr;
        BasicBlock* slow = newBB("cmp.pair.generic");
        BasicBlock* done = newBB("cmp.pair.done");
        llvm::Value* bothStrings = b.CreateAnd(
            b.CreateICmpEQ(b.CreateLShr(lhs, ci64(48)),
                            ci64(val::kTagStr >> 48)),
            b.CreateICmpEQ(b.CreateLShr(rhs, ci64(48)),
                            ci64(val::kTagStr >> 48)));
        b.CreateCondBr(bothStrings, check, slow);
        b.SetInsertPoint(check);
        llvm::Value* lstr = b.CreateIntToPtr(
            b.CreateAnd(lhs, ci64(val::kPtrMask)), ptrt);
        llvm::Value* rstr = b.CreateIntToPtr(
            b.CreateAnd(rhs, ci64(val::kPtrMask)), ptrt);
        llvm::Value* lst = loadSStrCache(i32t, lstr, kSStrNumStateOff, true);
        llvm::Value* rst = loadSStrCache(i32t, rstr, kSStrNumStateOff, true);
        auto isCompareNumber = [&](llvm::Value* st) {
            return b.CreateOr(b.CreateICmpEQ(st, ci32(1)),
                              b.CreateICmpEQ(st, ci32(4)));
        };
        llvm::Value* numericPair = b.CreateAnd(isCompareNumber(lst),
                                               isCompareNumber(rst));
        b.CreateCondBr(numericPair, numeric, lexical);
        b.SetInsertPoint(numeric);
        llvm::Value* ln = loadSStrCache(f64t, lstr, kSStrNumCacheOff, false);
        llvm::Value* rn = loadSStrCache(f64t, rstr, kSStrNumCacheOff, false);
        llvm::Value* direct = b.CreateZExt(
            b.CreateFCmp(predicate, ln, rn), i32t);
        b.CreateBr(done);

        b.SetInsertPoint(lexical);
        llvm::Value* lexResult = nullptr;
        llvm::Value* unicodeResult = nullptr;
        llvm::Value* simdResult = nullptr;
        if (!inlineAscii) {
            b.CreateBr(slow);
        } else {
            // Every ASCII byte lowercases to exactly one ASCII byte.
            // Unicode can expand (e.g. U+0130), so lexical comparisons are
            // only settled here after scanning *both entire strings* and
            // proving all UTF-16 code units are ASCII.
            llvm::Value* known = b.CreateAnd(
                b.CreateICmpNE(lst, ci32(0)),
                b.CreateICmpNE(rst, ci32(0)));
            b.CreateCondBr(known, useSimd ? simdCheck : shortCheck, slow);
            if (useSimd) {
                b.SetInsertPoint(simdCheck);
                llvm::Value* lhsLength = b.CreateLoad(i32t,
                    b.CreateConstGEP1_32(i8t, lstr, kSStrLenOff));
                llvm::Value* rhsLength = b.CreateLoad(i32t,
                    b.CreateConstGEP1_32(i8t, rstr, kSStrLenOff));
                llvm::Value* bothFlat = b.CreateNot(b.CreateOr(
                    emitIsConsStr(lstr), emitIsConsStr(rstr)));
                auto isAsciiCached = [&](llvm::Value* str) {
                    auto* flags = b.CreateLoad(i32t,
                        b.CreateConstGEP1_32(i8t, str, kSStrFlagsOff));
                    flags->setAtomic(AtomicOrdering::Acquire);
                    constexpr uint32_t required =
                        SStr::kAsciiKnown | SStr::kAsciiValue;
                    return b.CreateICmpEQ(
                        b.CreateAnd(flags, ci32(required)), ci32(required));
                };
                llvm::Value* checks = b.CreateAnd(
                    b.CreateICmpEQ(lhsLength, rhsLength),
                    b.CreateAnd(b.CreateICmpUGE(lhsLength, ci32(8)),
                        b.CreateAnd(b.CreateICmpULE(lhsLength, ci32(16)),
                            b.CreateAnd(bothFlat, b.CreateAnd(
                                isAsciiCached(lstr), isAsciiCached(rstr))))));
                b.CreateCondBr(checks, simdBody, shortCheck);
                b.SetInsertPoint(simdBody);
                auto* v16 = llvm::FixedVectorType::get(i16t, 8);
                auto* repeatA = llvm::ConstantVector::getSplat(
                    llvm::ElementCount::getFixed(8), b.getInt16('A'));
                auto* repeatZ = llvm::ConstantVector::getSplat(
                    llvm::ElementCount::getFixed(8), b.getInt16('Z'));
                auto* repeat32 = llvm::ConstantVector::getSplat(
                    llvm::ElementCount::getFixed(8), b.getInt16(32));
                auto loadFolded = [&](llvm::Value* str) {
                    auto* load = b.CreateLoad(v16, b.CreateConstGEP1_32(
                        i8t, str, kSStrDataOff));
                    load->setAlignment(llvm::Align(1));
                    llvm::Value* upper = b.CreateAnd(
                        b.CreateICmpUGE(load, repeatA),
                        b.CreateICmpULE(load, repeatZ));
                    return b.CreateAdd(load, b.CreateSelect(upper, repeat32,
                        llvm::Constant::getNullValue(v16)));
                };
                llvm::Value* vectorEq = b.CreateICmpEQ(
                    b.CreateBitCast(loadFolded(lstr), b.getIntNTy(128)),
                    b.CreateBitCast(loadFolded(rstr), b.getIntNTy(128)));
                b.CreateCondBr(b.CreateICmpEQ(lhsLength, ci32(8)),
                               simdDone, simdTail);
                b.SetInsertPoint(simdTail);
                BasicBlock* loopHead = newBB("eq.simd.tail.loop");
                BasicBlock* loopEnd = newBB("eq.simd.tail.end");
                b.CreateBr(loopHead);
                b.SetInsertPoint(loopHead);
                auto* idx = b.CreatePHI(i32t, 2, "eq.simd.index");
                auto* acc = b.CreatePHI(b.getInt1Ty(), 2, "eq.simd.acc");
                idx->addIncoming(ci32(8), simdTail);
                acc->addIncoming(vectorEq, simdTail);
                auto unit = [&](llvm::Value* str) {
                    llvm::Value* data = b.CreateConstGEP1_32(i8t, str, kSStrDataOff);
                    return b.CreateLoad(i16t, b.CreateGEP(i16t, data, idx));
                };
                auto fold = [&](llvm::Value* unitValue) {
                    llvm::Value* upper = b.CreateAnd(
                        b.CreateICmpUGE(unitValue, b.getInt16('A')),
                        b.CreateICmpULE(unitValue, b.getInt16('Z')));
                    return b.CreateSelect(upper,
                        b.CreateAdd(unitValue, b.getInt16(32)), unitValue);
                };
                llvm::Value* nextEq = b.CreateAnd(acc,
                    b.CreateICmpEQ(fold(unit(lstr)), fold(unit(rstr))));
                llvm::Value* nextIndex = b.CreateAdd(idx, ci32(1));
                idx->addIncoming(nextIndex, loopHead);
                acc->addIncoming(nextEq, loopHead);
                b.CreateCondBr(b.CreateICmpULT(nextIndex, lhsLength),
                               loopHead, loopEnd);
                b.SetInsertPoint(loopEnd);
                b.CreateBr(simdDone);
                b.SetInsertPoint(simdDone);
                auto* eqResult = b.CreatePHI(b.getInt1Ty(), 2);
                eqResult->addIncoming(vectorEq, simdBody);
                eqResult->addIncoming(nextEq, loopEnd);
                simdResult = b.CreateZExt(eqResult, i32t);
                b.CreateBr(done);
            }
            b.SetInsertPoint(shortCheck);
            llvm::Value* simple = b.CreateNot(b.CreateOr(
                emitIsConsStr(lstr), emitIsConsStr(rstr)));
            llvm::Value* lenA = b.CreateLoad(
                i32t, b.CreateConstGEP1_32(i8t, lstr, kSStrLenOff));
            llvm::Value* lenB = b.CreateLoad(
                i32t, b.CreateConstGEP1_32(i8t, rstr, kSStrLenOff));
            llvm::Value* maxLen = b.CreateSelect(
                b.CreateICmpUGT(lenA, lenB), lenA, lenB);
            llvm::Value* shortEnough = b.CreateAnd(
                b.CreateICmpUGT(maxLen, ci32(0)),
                b.CreateICmpULE(maxLen, ci32(4)));
            b.CreateCondBr(b.CreateAnd(simple, shortEnough),
                           scanHead, slow);
            b.SetInsertPoint(scanHead);
            BasicBlock* scanPreheader = shortCheck;
            auto* index = b.CreatePHI(i32t, 2, "cmp.pair.i");
            auto* allAscii = b.CreatePHI(b.getInt1Ty(), 2, "cmp.pair.ascii");
            auto* ordering = b.CreatePHI(i32t, 2, "cmp.pair.order");
            index->addIncoming(ci32(0), scanPreheader);
            allAscii->addIncoming(b.getTrue(), scanPreheader);
            ordering->addIncoming(ci32(0), scanPreheader);
            llvm::Value* withinA = b.CreateICmpULT(index, lenA);
            llvm::Value* withinB = b.CreateICmpULT(index, lenB);
            // Every flat SStr allocates a trailing NUL code unit. Clamp
            // out-of-bounds indices to zero before *loading*, then mask the
            // values; do not speculatively load beyond the allocation.
            auto readUnit = [&](llvm::Value* str, llvm::Value* within) {
                llvm::Value* safe = b.CreateSelect(within, index, ci32(0));
                llvm::Value* base = b.CreateConstGEP1_32(
                    i8t, str, kSStrDataOff);
                llvm::Value* ptr = b.CreateGEP(i16t, base, safe);
                return b.CreateSelect(within, b.CreateLoad(i16t, ptr),
                                      b.getInt16(0));
            };
            llvm::Value* ca = readUnit(lstr, withinA);
            llvm::Value* cb = readUnit(rstr, withinB);
            llvm::Value* asciiNext = b.CreateAnd(allAscii, b.CreateAnd(
                b.CreateICmpULT(ca, b.getInt16(128)),
                b.CreateICmpULT(cb, b.getInt16(128))));
            auto fold = [&](llvm::Value* ch) {
                llvm::Value* upper = b.CreateAnd(
                    b.CreateICmpUGE(ch, b.getInt16('A')),
                    b.CreateICmpULE(ch, b.getInt16('Z')));
                return b.CreateSelect(upper,
                    b.CreateAdd(ch, b.getInt16('a' - 'A')), ch);
            };
            llvm::Value* fa = fold(ca);
            llvm::Value* fb = fold(cb);
            llvm::Value* compareNow = b.CreateAnd(
                b.CreateAnd(withinA, withinB),
                b.CreateAnd(b.CreateICmpEQ(ordering, ci32(0)),
                            b.CreateICmpNE(fa, fb)));
            llvm::Value* diff = b.CreateSelect(
                b.CreateICmpULT(fa, fb), ci32(-1), ci32(1));
            llvm::Value* orderNext = b.CreateSelect(compareNow, diff, ordering);
            llvm::Value* nextIndex = b.CreateAdd(index, ci32(1));
            index->addIncoming(nextIndex, scanHead);
            allAscii->addIncoming(asciiNext, scanHead);
            ordering->addIncoming(orderNext, scanHead);
            b.CreateCondBr(b.CreateICmpULT(nextIndex, maxLen),
                           scanHead, scanDone);
            b.SetInsertPoint(scanDone);
            llvm::Value* lenOrder = b.CreateSelect(
                b.CreateICmpULT(lenA, lenB), ci32(-1),
                b.CreateSelect(b.CreateICmpUGT(lenA, lenB),
                               ci32(1), ci32(0)));
            llvm::Value* lexOrder = b.CreateSelect(
                b.CreateICmpNE(orderNext, ci32(0)), orderNext, lenOrder);
            b.CreateCondBr(asciiNext, asciiDirect, unicodeCheck);
            CmpInst::Predicate comparison = predicate == CmpInst::FCMP_OLT
                ? CmpInst::ICMP_SLT
                : predicate == CmpInst::FCMP_OGT
                    ? CmpInst::ICMP_SGT : CmpInst::ICMP_EQ;
            b.SetInsertPoint(asciiDirect);
            lexResult = b.CreateZExt(
                b.CreateICmp(comparison, lexOrder, ci32(0)), i32t);
            b.CreateBr(done);
            b.SetInsertPoint(unicodeCheck);
            // Unicode lowercasing can expand or reorder code points. A
            // previously published immutable-string proof is required:
            // either lowercase-stable, or foldable using ASCII A-Z only.
            const char* stableSetting =
                std::getenv("XYO_JIT_STRING_PAIR_UNICODE_STABLE");
            llvm::Value* stableBoth = b.getFalse();
            if (!stableSetting || stableSetting[0] != '0') {
                auto hasSafeFold = [&](llvm::Value* str) {
                    auto* flags = b.CreateLoad(i32t,
                        b.CreateConstGEP1_32(i8t, str, kSStrFlagsOff));
                    flags->setAtomic(AtomicOrdering::Acquire);
                    constexpr uint32_t lowerStable =
                        SStr::kLowerStableKnown | SStr::kLowerStableValue;
                    constexpr uint32_t asciiFold =
                        SStr::kAsciiFoldKnown | SStr::kAsciiFoldValue;
                    return b.CreateOr(
                        b.CreateICmpEQ(b.CreateAnd(flags, ci32(lowerStable)),
                                       ci32(lowerStable)),
                        b.CreateICmpEQ(b.CreateAnd(flags, ci32(asciiFold)),
                                       ci32(asciiFold)));
                };
                stableBoth = b.CreateAnd(hasSafeFold(lstr),
                                         hasSafeFold(rstr));
            }
            b.CreateCondBr(stableBoth, unicodeDirect, slow);
            b.SetInsertPoint(unicodeDirect);
            unicodeResult = b.CreateZExt(
                b.CreateICmp(comparison, lexOrder, ci32(0)), i32t);
            b.CreateBr(done);
        }
        b.SetInsertPoint(slow);
        llvm::Value* call = call2(fallback, i32t, lhs, rhs);
        b.CreateBr(done);
        b.SetInsertPoint(done);
        auto* result = b.CreatePHI(i32t, (inlineAscii ? 4 : 2) +
                                  (useSimd ? 1 : 0));
        result->addIncoming(direct, numeric);
        if (inlineAscii) {
            result->addIncoming(lexResult, asciiDirect);
            result->addIncoming(unicodeResult, unicodeDirect);
            if (useSimd) result->addIncoming(simdResult, simdDone);
        }
        result->addIncoming(call, slow);
        return result;
    }

    /// operator_equals（i32）。xyo_eq と同型:
    /// 1) bits 一致 → 1  2) 両辺 compare 数値 → fcmp
    /// 3) 両 str かつ numState 既知かつ不一致 → 0  4) 他 → helper
    llvm::Value* emitEq(Block* blk) {
        llvm::Value* a = emitInputOf(blk, "OPERAND1");
        llvm::Value* bv = emitInputOf(blk, "OPERAND2");
        // When comparing a numeric producer or promoted numeric shadow with
        // a valid numeric literal, use the cached double and numeric-compare
        // predicate. Other types still require Scratch's string comparator.
        auto numericLiteral = [&](llvm::Value* v) -> std::optional<double> {
            if (auto c = constNumV(v); c && !std::isnan(*c)) return c;
            if (auto it = literalCmpNums.find(v);
                it != literalCmpNums.end() && !std::isnan(it->second))
                return it->second;
            return std::nullopt;
        };
        // Equality is commutative, but evaluating the input reporters is not.
        // Both reporters were evaluated in Scratch's left-to-right order above.
        // Reuse the proven numeric-shadow fast path even when the immutable
        // numeric literal is the *left* operand (previously only RHS worked).
        auto emitNumericLiteralEq = [&](llvm::Value* candidate,
                                        double literal,
                                        const char* inputName) -> llvm::Value* {
            auto di = knownNumD.find(candidate);
            auto oi = knownNumOk.find(candidate);
            if (di == knownNumD.end() || oi == knownNumOk.end())
                return nullptr;
            llvm::Value* valid = oi->second;
            llvm::Value* numeric = di->second;
            const char* setting = std::getenv("XYO_JIT_EQ_STRING_CACHE");
            const char* profileSetting =
                std::getenv("XYO_JIT_EQ_NUMERIC_PROFILE_CACHE");
            // An observed-number-only direct scalar is unlikely to benefit
            // from dynamically reading an SStr numeric-cache state. Skipping
            // that *optimization* is always safe: any later string value
            // still falls back to the exact generic xyo_eq comparison.
            // It does not assume that a Scratch variable stays numeric.
            // Default on. AOT, unprofiled or mixed-type variables retain
            // the original lookup. =0 is a strict legacy-path opt-out.
            const bool skipStringProbe =
                (!profileSetting || profileSetting[0] != '0') &&
                profiledNumber(directVariableInput(blk, inputName));
            if (!knownNum.count(candidate) && !skipStringProbe &&
                (!setting || setting[0] != '0')) {
                // The promoted value's numeric-compare shadow can be stale:
                // an immutable numeric string is parsed by an equality
                // fallback without changing its variable's boxed value.
                // Check the SStr numeric cache *at comparison time* and
                // perform numeric equality directly for states 1/4.
                // Other states retain Scratch's lexical fallback (notably
                // whitespace-only strings and lowercase "infinity").
                BasicBlock* original = b.GetInsertBlock();
                BasicBlock* cached = newBB("eq.cache.string");
                BasicBlock* joined = newBB("eq.cache.join");
                llvm::Value* isStr = b.CreateICmpEQ(
                    b.CreateLShr(candidate, ci64(48)),
                    ci64(val::kTagStr >> 48));
                b.CreateCondBr(b.CreateAnd(isStr, b.CreateNot(valid)),
                               cached, joined);
                b.SetInsertPoint(cached);
                llvm::Value* sp = b.CreateIntToPtr(
                    b.CreateAnd(candidate, ci64(val::kPtrMask)), ptrt);
                llvm::Value* state = loadSStrCache(
                    i32t, sp, kSStrNumStateOff, true);
                llvm::Value* parsed = b.CreateOr(
                    b.CreateICmpEQ(state, ci32(1)),
                    b.CreateICmpEQ(state, ci32(4)));
                llvm::Value* value = loadSStrCache(
                    f64t, sp, kSStrNumCacheOff, false);
                b.CreateBr(joined);
                b.SetInsertPoint(joined);
                auto* cachedOk = b.CreatePHI(b.getInt1Ty(), 2);
                cachedOk->addIncoming(b.getFalse(), original);
                cachedOk->addIncoming(parsed, cached);
                auto* cachedValue = b.CreatePHI(f64t, 2);
                cachedValue->addIncoming(cf64(0.0), original);
                cachedValue->addIncoming(value, cached);
                valid = b.CreateOr(valid, cachedOk);
                numeric = b.CreateSelect(oi->second, di->second, cachedValue);
            }
            llvm::Value* numberEquals =
                b.CreateFCmpOEQ(numeric, cf64(literal));
            if (knownNum.count(candidate))
                return b.CreateZExt(
                    b.CreateAnd(oi->second, numberEquals), i32t);
            BasicBlock* fast = newBB("eq.numeric.fast");
            BasicBlock* slow = newBB("eq.numeric.slow");
            BasicBlock* done = newBB("eq.numeric.merge");
            b.CreateCondBr(valid, fast, slow);
            b.SetInsertPoint(fast);
            llvm::Value* fastResult = b.CreateZExt(numberEquals, i32t);
            b.CreateBr(done);
            b.SetInsertPoint(slow);
            llvm::Value* fallback = call2("xyo_eq", i32t, a, bv);
            b.CreateBr(done);
            b.SetInsertPoint(done);
            PHINode* result = b.CreatePHI(i32t, 2);
            result->addIncoming(fastResult, fast);
            result->addIncoming(fallback, slow);
            return result;
        };
        if (!std::getenv("XYO_JIT_DISABLE_EQ_NUMERIC")) {
            if (auto c = numericLiteral(bv)) {
                if (llvm::Value* result = emitNumericLiteralEq(a, *c, "OPERAND1"))
                    return result;
            }
            // A/B diagnostic switch; default behavior is symmetric.
            if (!std::getenv("XYO_JIT_DISABLE_EQ_SYMMETRIC")) {
                if (auto c = numericLiteral(a)) {
                    if (llvm::Value* result = emitNumericLiteralEq(bv, *c, "OPERAND2"))
                        return result;
                }
            }
        }
        // Always emit BOTH operands, in order, before selecting a fast path.
        if (!genericEq) {
            if (auto word = eqAsciiWord(blk, "OPERAND2"))
                return emitEqAsciiWord(a, bv, a, *word);
            if (auto word = eqAsciiWord(blk, "OPERAND1"))
                return emitEqAsciiWord(a, bv, bv, *word);
        }
        BasicBlock* sameBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* notSameBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* cmpBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strStBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* ret0BB = BasicBlock::Create(lc, "", fn);
        BasicBlock* ret1BB = BasicBlock::Create(lc, "", fn);
        BasicBlock* helperBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(b.CreateICmpEQ(a, bv), sameBB, notSameBB);
        b.SetInsertPoint(sameBB);
        b.CreateBr(merge);
        b.SetInsertPoint(notSameBB);
        llvm::Value *okA, *okB;
        const bool aNum = isNumericOperand(a);
        const bool bNum = isNumericOperand(bv);
        llvm::Value* fa = emitCmpNumF64(a, &okA, bNum);
        llvm::Value* fb = emitCmpNumF64(bv, &okB, aNum);
        b.CreateCondBr(b.CreateAnd(okA, okB), cmpBB, strBB);
        b.SetInsertPoint(cmpBB);
        llvm::Value* c = b.CreateZExt(b.CreateFCmpOEQ(fa, fb), i32t);
        b.CreateBr(merge);
        b.SetInsertPoint(strBB);
        llvm::Value* isStrA = b.CreateICmpEQ(
            b.CreateLShr(a, ci64(48)), ci64(val::kTagStr >> 48));
        llvm::Value* isStrB = b.CreateICmpEQ(
            b.CreateLShr(bv, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(b.CreateAnd(isStrA, isStrB), strStBB, helperBB);
        b.SetInsertPoint(strStBB);
        // str-str: (a) バイト一致 → 真（numState 無関係・大小文字
        // 比較でも等しい） (b) numState 既知かつ不一致 → 偽
        // (c) 他 → helper
        llvm::Value* sp1 = b.CreateIntToPtr(
            b.CreateAnd(a, ci64(val::kPtrMask)), ptrt);
        llvm::Value* sp2 = b.CreateIntToPtr(
            b.CreateAnd(bv, ci64(val::kPtrMask)), ptrt);
        BasicBlock* memBB  = newBB("eq.mc");
        BasicBlock* diffBB = newBB("eq.df");
        // 連結ノードは本文が構造体直後に無いので helper（view が flatten）。
        BasicBlock* flatBB = newBB("eq.flat");
        b.CreateCondBr(
            b.CreateOr(emitIsConsStr(sp1), emitIsConsStr(sp2)),
            helperBB, flatBB);
        b.SetInsertPoint(flatBB);
        llvm::Value* l1 = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, sp1, kSStrLenOff));
        llvm::Value* l2 = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, sp2, kSStrLenOff));
        b.CreateCondBr(b.CreateICmpEQ(l1, l2), memBB, diffBB);
        b.SetInsertPoint(memBB);
        llvm::Value* nBytes = b.CreateShl(
            b.CreateZExt(l1, i64t), ci64(1));
        llvm::Value* mc = b.CreateCall(
            decl("memcmp", i32t, {ptrt, ptrt, i64t}),
            {b.CreateConstGEP1_32(i8t, sp1, kSStrDataOff),
             b.CreateConstGEP1_32(i8t, sp2, kSStrDataOff), nBytes});
        b.CreateCondBr(b.CreateICmpEQ(mc, ci32(0)), ret1BB, diffBB);
        b.SetInsertPoint(diffBB);
        llvm::Value* st1 = loadSStrCache(i32t, sp1, kSStrNumStateOff, true);
        llvm::Value* st2 = loadSStrCache(i32t, sp2, kSStrNumStateOff, true);
        llvm::Value* known = b.CreateAnd(
            b.CreateICmpNE(st1, ci32(0)), b.CreateICmpNE(st2, ci32(0)));
        // state4(Infinity リテラル) は case 違い state2 と等しいため
        // ショートカット不可（xyo_eq のガードと同型）
        llvm::Value* noInf = b.CreateAnd(
            b.CreateICmpNE(st1, ci32(4)), b.CreateICmpNE(st2, ci32(4)));
        llvm::Value* shortcut = b.CreateAnd(
            known, b.CreateAnd(noInf, b.CreateICmpNE(st1, st2)));
        b.CreateCondBr(shortcut, ret0BB, helperBB);
        b.SetInsertPoint(ret0BB);
        b.CreateBr(merge);
        b.SetInsertPoint(helperBB);
        // A boxed numeric constant cannot be a heap string, so the
        // two-string fast path is statically impossible for that shape.
        llvm::Value* h = knownNum.count(a) || knownNum.count(bv) ||
                          constNumV(a) || constNumV(bv)
            ? call2("xyo_eq", i32t, a, bv)
            : emitCachedNumericStringPair(
                a, bv, "xyo_eq", CmpInst::FCMP_OEQ,
                directVariableInput(blk, "OPERAND1") &&
                directVariableInput(blk, "OPERAND2"));
        BasicBlock* helperEnd = b.GetInsertBlock();
        b.CreateBr(merge);
        b.SetInsertPoint(ret1BB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i32t, 5);
        p->addIncoming(ci32(1), sameBB);
        p->addIncoming(c, cmpBB);
        p->addIncoming(ci32(1), ret1BB);
        p->addIncoming(ci32(0), ret0BB);
        p->addIncoming(h, helperEnd);
        return p;
    }

    // --- motion/pen の inline lowering --------------------------------------

    /// Target::setXY の fast path。
    /// !isStage && !dragging && runtime->renderer==null のとき
    /// renderer 依存処理（fencing・visualChange・redrawRequested）が
    /// 全てスキップされるため、座標 store + onTargetMoved hook +
    /// refreshTargets のみで済む。slow path は setXY を通す helper。
    void emitSetXY(llvm::Value* nx, llvm::Value* ny,
                   llvm::function_ref<void()> slow) {
        BasicBlock* fastBB = newBB("sxy.f");
        BasicBlock* hookBB = newBB("sxy.h");
        BasicBlock* slowBB = newBB("sxy.s");
        BasicBlock* merge  = newBB("sxy.m");
        llvm::Value* t = ctxPtr(1);
        llvm::Value* rt = ctxPtr(2);
        llvm::Value* flags = b.CreateOr(
            b.CreateLoad(i8t, fld(t, ly().isStage)),
            b.CreateLoad(i8t, fld(t, ly().dragging)));
        llvm::Value* ren = b.CreateLoad(ptrt, fld(rt, ly().rtRen));
        b.CreateCondBr(
            b.CreateOr(b.CreateICmpNE(flags, ci8(0)),
                       b.CreateIsNotNull(ren)),
            slowBB, fastBB);
        b.SetInsertPoint(slowBB);
        slow();
        b.CreateBr(merge);
        b.SetInsertPoint(fastBB);
        llvm::Value* ox = b.CreateLoad(f64t, fld(t, ly().x));
        llvm::Value* oy = b.CreateLoad(f64t, fld(t, ly().y));
        b.CreateStore(nx, fld(t, ly().x));
        b.CreateStore(ny, fld(t, ly().y));
        llvm::Value* hook = b.CreateLoad(ptrt, fld(t, ly().moved));
        // renderer==null の fast path では penOnTargetMoved も
        // penSkin<0 で即 return する no-op — hook がそれと一致
        // するなら呼出し自体をスキップ（未知 hook のみ呼ぶ）
        llvm::Value* skip = b.CreateOr(
            b.CreateIsNull(hook),
            b.CreateICmpEQ(
                hook, slotPtr(AotSlotKind::MovedHook, "",
                                (const void*)penrt::movedHook())));
        b.CreateCondBr(skip, merge, hookBB);
        b.SetInsertPoint(hookBB);
        b.CreateCall(FunctionType::get(voidt,
                         {ptrt, f64t, f64t, b.getInt1Ty()}, false),
                     hook, {t, ox, oy, b.getInt1(false)});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        b.CreateStore(ci8(1), fld(rt, ly().rtRefresh));
    }

    /// motion_{changexby,changeyby,setx,sety}（kind 0-3）
    void emitMoveXY(Block* blk, const char* in, int kind,
                    const char* helper) {
        llvm::Value* dv = emitInputOf(blk, in);
        llvm::Value* d = emitToNumF64(dv);
        llvm::Value* t = ctxPtr(1);
        llvm::Value* nx = kind == 0
            ? b.CreateFAdd(b.CreateLoad(f64t, fld(t, ly().x)), d)
            : kind == 2 ? d : b.CreateLoad(f64t, fld(t, ly().x));
        llvm::Value* ny = kind == 1
            ? b.CreateFAdd(b.CreateLoad(f64t, fld(t, ly().y)), d)
            : kind == 3 ? d : b.CreateLoad(f64t, fld(t, ly().y));
        emitSetXY(nx, ny, [&] {
            if (boxedMotion) call2(helper, voidt, ctxArg, dv);
            else call3("xyo_mot_setxy_num", voidt, ctxArg, nx, ny);
        });
    }

    /// pen_penUp。penrt::up は penStateOf で遅延生成するが
    /// PenState 初期値が penDown=false のため生成不要 —
    /// penState!=null のときのみ flag/hook を落とす。
    void emitPenUp() {
        BasicBlock* hasBB = newBB("pu.h");
        BasicBlock* merge = newBB("pu.m");
        llvm::Value* t = ctxPtr(1);
        llvm::Value* ps = b.CreateLoad(ptrt, fld(t, ly().penState));
        b.CreateCondBr(b.CreateIsNotNull(ps), hasBB, merge);
        b.SetInsertPoint(hasBB);
        b.CreateStore(ci8(0), fld(ps, ly().psDown));
        b.CreateStore(cnull(), fld(t, ly().moved));
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    /// pen_penDown の headless fast path。
    /// penState 既存 && renderer==null なら penSkin<0 で描画部が
    /// no-op になるため flag/hook のみ。他は penDownCore 相当の
    /// helper（penPoint 描画・penState 生成を含む）。
    void emitPenDown() {
        BasicBlock* fastBB = newBB("pd.f");
        BasicBlock* setBB  = newBB("pd.s");
        BasicBlock* helpBB = newBB("pd.h");
        BasicBlock* merge  = newBB("pd.m");
        llvm::Value* t = ctxPtr(1);
        llvm::Value* rt = ctxPtr(2);
        llvm::Value* ps = b.CreateLoad(ptrt, fld(t, ly().penState));
        llvm::Value* renNull = b.CreateIsNull(
            b.CreateLoad(ptrt, fld(rt, ly().rtRen)));
        b.CreateCondBr(
            b.CreateAnd(b.CreateIsNotNull(ps), renNull), fastBB, helpBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* down = b.CreateLoad(i8t, fld(ps, ly().psDown));
        b.CreateCondBr(b.CreateICmpNE(down, ci8(0)), merge, setBB);
        b.SetInsertPoint(setBB);
        b.CreateStore(ci8(1), fld(ps, ly().psDown));
        b.CreateStore(slotPtr(AotSlotKind::MovedHook, "",
                              (const void*)penrt::movedHook()),
                      fld(t, ly().moved));
        b.CreateBr(merge);
        b.SetInsertPoint(helpBB);
        call1("xyo_pen_down", voidt, ctxArg);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    /// COLOR_PARAM メニューのコンパイル時解決（kind 0-3、不明=-1）
    int penParamKind(Block* blk, const char* name) {
        const BlockInput* in = blk->inputs.find(name);
        if (!in) return -1;
        std::string cid = !in->block.empty() ? in->block : in->shadow;
        Block* mb = cid.empty() ? nullptr : getBlock(cid);
        if (!mb || mb->fields.empty()) return -1;
        const BlockField& f = mb->fields.begin()->second;
        if (!f.value.isString() || !f.value.asStrPtr()) return -1;
        auto sv = f.value.asStrPtr()->view();
        if (sv == u"color") return 0;
        if (sv == u"saturation") return 1;
        if (sv == u"brightness") return 2;
        if (sv == u"transparency") return 3;
        return -1;
    }

    /// pen_{change,set}PenColorParam{By,To}（kind は penParamKind 済み）
    /// penState 既存 → フィールド更新 + recolor。
    /// なければ遅延生成込み helper。
    void emitPenParam(Block* blk, int kind, bool change) {
        llvm::Value* vv = emitToNumF64(emitInputOf(blk, "VALUE"));
        BasicBlock* hasBB  = newBB("pp.h");
        BasicBlock* helpBB = newBB("pp.s");
        BasicBlock* merge  = newBB("pp.m");
        llvm::Value* t = ctxPtr(1);
        llvm::Value* ps = b.CreateLoad(ptrt, fld(t, ly().penState));
        b.CreateCondBr(b.CreateIsNotNull(ps), hasBB, helpBB);
        b.SetInsertPoint(hasBB);
        const uint32_t kOff[4] =
            {ly().psColor, ly().psSat, ly().psBright, ly().psTransp};
        llvm::Value* fp = fld(ps, kOff[kind]);
        llvm::Value* nv = change
            ? b.CreateFAdd(b.CreateLoad(f64t, fp), vv) : vv;
        if (kind == 0) {
            // Match Scratch MathUtil.wrapClamp(v, 0, 100): inclusive
            // endpoints give a period of 101, not 100.
            nv = b.CreateFRem(nv, cf64(101.0));
            nv = b.CreateSelect(
                b.CreateFCmpOLT(nv, cf64(0.0)),
                b.CreateFAdd(nv, cf64(101.0)), nv);
        } else {
            // std::clamp(v,0,100) — NaN は fcmp false で保持
            nv = b.CreateSelect(
                b.CreateFCmpOGT(nv, cf64(100.0)), cf64(100.0), nv);
            nv = b.CreateSelect(
                b.CreateFCmpOLT(nv, cf64(0.0)), cf64(0.0), nv);
        }
        b.CreateStore(nv, fp);
        // updatePenColor は描画時まで遅延（pen.cpp colorDirty）。
        // 色フィールド変更は setcolor メモを無効化（~0 は
        // 有効な Value bits になり得ないタグ）
        b.CreateStore(ci8(1), fld(ps, ly().psDirty));
        b.CreateStore(ci64(~0ULL), fld(ps, ly().psLast));
        b.CreateBr(merge);
        b.SetInsertPoint(helpBB);
        callN("xyo_pen_paramnum", voidt,
              {ctxArg, ci32(kind), ci32(change ? 1 : 0), vv});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    /// pen_setPenColorToColor。COLOR が定数なら
    /// setColorToColorState 相当をコンパイル時計算し PenState
    /// フィールドを直接 store。非定数は PenState* を渡すだけの
    /// helper（toRgbColorObject + RGB→HSV→RGB 往復キャッシュ）へ。
    /// penState==null は target から生成する helper。
    /// 直前と同一 arg bits の再適用は lastSetColor メモでスキップ。
    void emitPenSetColor(Block* blk) {
        llvm::Value* cv = emitInputOf(blk, "COLOR");
        auto* ci = dyn_cast<ConstantInt>(cv);
        BasicBlock* chkBB  = newBB("sc.c");
        BasicBlock* workBB = newBB("sc.w");
        BasicBlock* helpBB = newBB("sc.s");
        BasicBlock* merge  = newBB("sc.m");
        llvm::Value* t = ctxPtr(1);
        llvm::Value* ps = b.CreateLoad(ptrt, fld(t, ly().penState));
        b.CreateCondBr(b.CreateIsNotNull(ps), chkBB, helpBB);
        b.SetInsertPoint(chkBB);
        llvm::Value* lc = b.CreateLoad(i64t, fld(ps, ly().psLast));
        b.CreateCondBr(b.CreateICmpEQ(lc, cv), merge, workBB);
        b.SetInsertPoint(workBB);
        if (ci) {
            // 定数 → コンパイル時計算。rgba は dirty 遅延（次の移動で
            // syncPenColor が 1 回だけ hsvToRgb — memo で 1 回きり）。
            PenState tmp;
            penrt::setColorToColorState(tmp,
                                        Value{ci->getZExtValue()});
            emitPenColorFields(ps, cv, cf64(tmp.color),
                               cf64(tmp.saturation), cf64(tmp.brightness),
                               cf64(tmp.transparency), cf64(tmp.shade));
            b.CreateBr(merge);
        } else {
            // 数値・文字列の判別と decimal 化は helper 側
            // （toRgbColorObject が SStr の rgbCache/numCache を使う）。
            call2("xyo_pen_setcolor_ps", voidt, ps, cv);
            b.CreateBr(merge);
        }
        b.SetInsertPoint(helpBB);
        call2("xyo_pen_setcolor", voidt, ctxArg, cv);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    /// penSetColorToColorCore のフィールド書込み部（rgba は dirty
    /// 遅延）。setColorToColorState と同一の代入列 + memo 更新。
    void emitPenColorFields(llvm::Value* ps, llvm::Value* lastBits,
                            llvm::Value* col, llvm::Value* sat,
                            llvm::Value* bright, llvm::Value* transp,
                            llvm::Value* shade) {
        b.CreateStore(col, fld(ps, ly().psColor));
        b.CreateStore(sat, fld(ps, ly().psSat));
        b.CreateStore(bright, fld(ps, ly().psBright));
        b.CreateStore(transp, fld(ps, ly().psTransp));
        b.CreateStore(shade, fld(ps, ly().psShade));
        b.CreateStore(ci8(1), fld(ps, ly().psDirty));
        b.CreateStore(lastBits, fld(ps, ly().psLast));
    }

    /// 比較（両辺が compare 意味の数値なら fcmp、他は helper）。
    /// helper と同じ i32 を返す。
    llvm::Value* emitCmp(Block* blk, const char* in1, const char* in2,
                         const char* helper, CmpInst::Predicate pred) {
        llvm::Value* a = emitInputOf(blk, in1);
        llvm::Value* bv = emitInputOf(blk, in2);
        // For two dynamic Scratch values, the hot case is two plain IEEE
        // numbers (finite values or infinities). Check their absolute bit
        // patterns against +Infinity. Unlike signed-bits < kNan, this ALSO
        // excludes every noncanonical negative NaN payload. All Scratch
        // NaN-boxed tags and either sign of NaN take the full helper path.
        // A list item changing type never invalidates this guard.
        const char* plainCmp = std::getenv("XYO_JIT_PLAIN_NUMBER_CMP");
        // List-item comparisons dominate many Scratch algorithms. Only this
        // structurally identifiable shape uses the speculation by default;
        // global speculative comparison adds overhead to string/other cases.
        // 0 disables it; 1 opts every dynamic/dynamic comparison into it.
        auto isListItemInput = [&](const char* name) {
            const BlockInput* input = blk->inputs.find(name);
            if (!input) return false;
            const std::string& id = input->block.empty()
                ? input->shadow : input->block;
            Block* source = id.empty() ? nullptr : getBlock(id);
            return source && source->opcode == "data_itemoflist";
        };
        // Only the already type-guarded numeric comparison may use the
        // portable typed SSA. The fallback retains Scratch string ordering.
        auto emitSharedOrdered = [&](llvm::Value* lhs, llvm::Value* rhs) {
            const ir::Op opcode = pred == CmpInst::FCMP_OLT
                ? ir::Op::LessThan : ir::Op::GreaterThan;
            const ir::Function& graph = ir::binaryFunction(opcode);
            struct Lowerer {
                IRBuilder<>& builder;
                llvm::Value* args[2];
                llvm::Value* parameter(uint32_t i) { return args[i]; }
                llvm::Value* binary(ir::Op op, llvm::Value* a,
                                    llvm::Value* b) {
                    return op == ir::Op::LessThan
                        ? builder.CreateFCmpOLT(a, b)
                        : builder.CreateFCmpOGT(a, b);
                }
            } lowerer{b, {lhs, rhs}};
            return ir::lower(graph, lowerer, 2);
        };
        const bool listPair = isListItemInput(in1) && isListItemInput(in2);
        const bool fastCmp = plainCmp ? plainCmp[0] == '1' : listPair;
        if (fastCmp && !isNumericOperand(a) && !isNumericOperand(bv)) {
            BasicBlock* numberBB = newBB("cmp.plain.number");
            BasicBlock* fallbackBB = newBB("cmp.plain.scratch");
            BasicBlock* doneBB = newBB("cmp.plain.done");
            constexpr uint64_t kMagnitudeMask = 0x7fff'ffff'ffff'ffffull;
            constexpr uint64_t kInfinityBits = 0x7ff0'0000'0000'0000ull;
            auto plainNumber = [&](llvm::Value* value) {
                return b.CreateICmpULE(
                    b.CreateAnd(value, ci64(kMagnitudeMask)),
                    ci64(kInfinityBits));
            };
            llvm::Value* fa = emitAsF64(a);
            llvm::Value* fb = emitAsF64(bv);
            // A NaN-boxed non-number is always a NaN when interpreted as
            // IEEE double. Use an ordered floating-point comparison as an
            // alternative to two integer type guards; an unordered input
            // must still use Scratch's string/numeric fallback semantics.
            const char* orderedEnv = std::getenv("XYO_JIT_FP_ORDERED_CMP");
            // The default ordered comparison can reuse the same floating
            // point operands and flags as the actual < or > comparison.
            // The older integer tag guard is available for controlled A/B
            // testing; both reject every NaN-boxed non-number.
            const bool useOrdered = !orderedEnv || orderedEnv[0] != '0';
            llvm::Value* bothNumber = useOrdered
                ? b.CreateFCmpORD(fa, fb)
                : b.CreateAnd(plainNumber(a), plainNumber(bv));
            // A common CFG diamond drives both the LLVM and WASM lowerers.
            // This native case materializes actual LLVM basic blocks + Phi.
            static const ir::Diamond diamond(true, ir::Type::I32);
            struct DiamondLowerer {
                Codegen& cg;
                BasicBlock* yes;
                BasicBlock* no;
                BasicBlock* merge;
                BasicBlock* yesEnd = nullptr;
                BasicBlock* noEnd = nullptr;
                void branch(llvm::Value* cond, ir::BlockId,
                            ir::BlockId, ir::BlockId) {
                    cg.hotBr(cond, yes, no, 1000, 1);
                    cg.b.SetInsertPoint(yes);
                }
                void otherwise(ir::BlockId) {
                    yesEnd = cg.b.GetInsertBlock();
                    cg.b.CreateBr(merge);
                    cg.b.SetInsertPoint(no);
                }
                void finish(ir::BlockId) {
                    noEnd = cg.b.GetInsertBlock();
                    cg.b.CreateBr(merge);
                    cg.b.SetInsertPoint(merge);
                }
                llvm::Value* phi(ir::BlockId, ir::BlockId,
                                 llvm::Value* yesValue, ir::BlockId,
                                 llvm::Value* noValue) {
                    auto* result = cg.b.CreatePHI(cg.i32t, 2);
                    result->addIncoming(yesValue, yesEnd);
                    result->addIncoming(noValue, noEnd);
                    return result;
                }
            } lowered{*this, numberBB, fallbackBB, doneBB};
            return ir::lowerDiamondValue(
                diamond, lowered, bothNumber,
                [&]() -> llvm::Value* {
                    return b.CreateZExt(emitSharedOrdered(fa, fb), i32t);
                },
                [&]() -> llvm::Value* { return call2(helper, i32t, a, bv); });
        }
        // 相手が数値と分かっていれば、リテラル側を数値へ畳める。
        const bool aNum = isNumericOperand(a);
        const bool bNum = isNumericOperand(bv);
        llvm::Value *okA, *okB;
        llvm::Value* fa = emitCmpNumF64(a, &okA, bNum);
        llvm::Value* fb = emitCmpNumF64(bv, &okB, aNum);
        llvm::Value* both = b.CreateAnd(okA, okB);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(both, fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* c1 = emitSharedOrdered(fa, fb);
        llvm::Value* z = b.CreateZExt(c1, i32t);
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* s = knownNum.count(a) || knownNum.count(bv) ||
                          constNumV(a) || constNumV(bv)
            ? call2(helper, i32t, a, bv)
            : emitCachedNumericStringPair(a, bv, helper, pred);
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i32t, 2);
        p->addIncoming(z, fastBB);
        p->addIncoming(s, slowEnd);
        return p;
    }

    /// Value → i1 の真値評価（cast::toBoolean と同型）。
    /// number → d != 0（NaN は unordered で false）、bool → kTrue 一致、
    /// それ以外は xyo_truthy（文字列・色等）。
    llvm::Value* emitTruthyV(llvm::Value* bits) {
        if (auto d = constNumV(bits))
            return b.getInt1(*d != 0.0 && !std::isnan(*d));
        if (auto* ci = dyn_cast<ConstantInt>(bits)) {
            uint64_t vb = ci->getZExtValue();
            if (vb == val::kTrue) return b.getInt1(true);
            if (vb == val::kFalse) return b.getInt1(false);
        }
        // Arithmetic reporters emitted by this code generator carry a
        // canonical numeric result, even when the Scratch value is NaN.
        // Reuse its unboxed SSA shadow rather than redispatching the boxed
        // result through numeric/boolean/string tests and a helper edge.
        // The shadow is cast::toNumber(value), so canonical NaN is already 0.
        if (knownNum.count(bits)) {
            if (auto it = knownNumD.find(bits); it != knownNumD.end())
                return b.CreateFCmpONE(it->second, cf64(0.0));
            return b.CreateFCmpONE(emitAsF64(bits), cf64(0.0));
        }
        // Feedback is a prediction, not a proof. Only plain IEEE-754
        // numbers may enter this specialized branch; a later script/clone
        // writing boolean or string still takes the full Scratch truthiness
        // semantics, including "0", "false" and nonempty numeric strings.
        if (likelyNumLoads.count(bits)) {
            BasicBlock* fast = newBB("truthy.profile.number");
            BasicBlock* slow = newBB("truthy.profile.other");
            BasicBlock* merge = newBB("truthy.profile.done");
            hotBr(b.CreateICmpSLT(bits, ci64(val::kNan)),
                  fast, slow, 200, 1);
            b.SetInsertPoint(fast);
            llvm::Value* result = b.CreateFCmpONE(emitAsF64(bits), cf64(0.0));
            b.CreateBr(merge);
            b.SetInsertPoint(slow);
            llvm::Value* generic = b.CreateICmpNE(
                call1("xyo_truthy", i32t, bits), ci32(0));
            b.CreateBr(merge);
            b.SetInsertPoint(merge);
            PHINode* truthy = b.CreatePHI(b.getInt1Ty(), 2);
            truthy->addIncoming(result, fast);
            truthy->addIncoming(generic, slow);
            return truthy;
        }
        // Comparisons / boolean operators already carry an i1 condition.
        // Recover it before boxing adds tag tests and a false-side helper call.
        if (auto* sel = dyn_cast<SelectInst>(bits)) {
            auto* t = dyn_cast<ConstantInt>(sel->getTrueValue());
            auto* f = dyn_cast<ConstantInt>(sel->getFalseValue());
            if (t && f) {
                if (t->getZExtValue() == val::kTrue && f->getZExtValue() == val::kFalse)
                    return sel->getCondition();
                if (t->getZExtValue() == val::kFalse && f->getZExtValue() == val::kTrue)
                    return b.CreateNot(sel->getCondition());
            }
        }
        llvm::Value* isn = emitIsNum(bits);
        llvm::Value* isTrue = b.CreateICmpEQ(bits, ci64(val::kTrue));
        llvm::Value* isFalse = b.CreateICmpEQ(bits, ci64(val::kFalse));
        BasicBlock* numBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        // Both boxed booleans stay inline. kFalse is a NaN bit pattern,
        // so ordered nonzero is false; only strings/other tags need a helper.
        llvm::Value* goNum = b.CreateOr(isn, b.CreateOr(isTrue, isFalse));
        b.CreateCondBr(goNum, numBB, slowBB);
        b.SetInsertPoint(numBB);
        llvm::Value* tn = b.CreateOr(
            isTrue, b.CreateFCmpONE(emitAsF64(bits), cf64(0.0)));
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* th = call1("xyo_truthy", i32t, bits);
        llvm::Value* ts = b.CreateICmpNE(th, ci32(0));
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(b.getInt1Ty(), 2);
        p->addIncoming(tn, numBB);
        p->addIncoming(ts, slowBB);
        return p;
    }
    /// i1 → kTrue/kFalse の Value bits
    llvm::Value* boolBits(llvm::Value* i1) {
        return b.CreateSelect(i1, ci64(val::kTrue), ci64(val::kFalse));
    }

    // --- リストの fast path -----------------------------------------------
    // SList: {u32 len; u32 cap; bool monitorDirty; pad; Value* items}
    static constexpr uint32_t kListLenOff = ir::kListLengthOffset;
    static constexpr uint32_t kListCapOff = uint32_t(offsetof(SList, cap));
    static constexpr uint32_t kListDirtyOff =
        uint32_t(offsetof(SList, monitorDirty));
    static constexpr uint32_t kListNumericOnlyOff =
        ir::kListNumericOnlyOffset;
    void invalidateNumericList(llvm::Value* list, llvm::Value* item,
                               bool replacement = false) {
        llvm::Value* flag = b.CreateConstGEP1_32(
            i8t, list, kListNumericOnlyOff);
        static const bool lazyInvalidation = [] {
            // Default on for list replacement only. Keep add/append on its
            // established fast path; =0 restores unconditional read/AND/store.
            const char* env = std::getenv("XYO_JIT_LIST_MONOTONE_FLAG");
            return !env || env[0] != '0';
        }();
        if (replacement && lazyInvalidation) {
            // SList::numericOnly is monotonic for push/replace: writing
            // a numeric Value cannot change it. In the nonnumeric case
            // clear the flag exactly once (idempotent if already false).
            // No list code or synchronization rule is otherwise changed.
            if (knownNum.count(item)) return;
            BasicBlock* nonNumeric = newBB("list.numflag.invalidate");
            BasicBlock* done = newBB("list.numflag.done");
            hotBr(emitIsNum(item), done, nonNumeric, 500, 1);
            b.SetInsertPoint(nonNumeric);
            b.CreateStore(b.getInt1(false), flag);
            b.CreateBr(done);
            b.SetInsertPoint(done);
            return;
        }
        llvm::Value* prior = b.CreateLoad(b.getInt1Ty(), flag);
        llvm::Value* valid = b.CreateAnd(prior, emitIsNum(item));
        b.CreateStore(valid, flag);
    }
    static constexpr uint32_t kListItemsOff =
        ir::kListItemsOffset;
    static constexpr uint32_t kListExtraOff =
        uint32_t(offsetof(SList, extra));
    static constexpr uint32_t kListStorageEpochOff =
        uint32_t(offsetof(SList, storageEpoch));

    // In an uninterrupted promotable loop, no operation can relocate an
    // already-contiguous list. Cache items in the loop preheader, refreshing
    // after each scheduler yield. A null cached items pointer means either
    // that the list was absent or that it used a non-contiguous backend.
    // Only this cold state needs to check the storage epoch for transitions.
    void guardAdaptiveContiguous(llvm::Value* list,
                                 const PromotedList* pl,
                                 BasicBlock* fallback) {
        if (xyo_list_storage_mode != 7) return;
        auto* contiguous = BasicBlock::Create(lc, "list.flat", fn);
        if (pl && pl->storageEpoch) {
            auto* cold = BasicBlock::Create(lc, "list.nonflat.cache", fn);
            llvm::Value* cachedItems = b.CreateLoad(ptrt, pl->items);
            hotBr(b.CreateICmpNE(
                      cachedItems,
                      ConstantPointerNull::get(llvm::cast<PointerType>(ptrt))),
                  contiguous, cold);
            b.SetInsertPoint(cold);
            // A list field can be absent. Never access its epoch before
            // validating the actual SList pointer.
            auto* checkEpoch = BasicBlock::Create(lc, "list.epoch.check", fn);
            hotBr(b.CreateICmpNE(
                      list,
                      ConstantPointerNull::get(llvm::cast<PointerType>(ptrt))),
                  checkEpoch, fallback);
            b.SetInsertPoint(checkEpoch);
            llvm::Value* current = b.CreateLoad(
                i64t, b.CreateConstGEP1_32(
                    i8t, list, kListStorageEpochOff));
            llvm::Value* cached = b.CreateLoad(i64t, pl->storageEpoch);
            auto* reload = BasicBlock::Create(lc, "list.epoch.reload", fn);
            // If the cached representation is non-flat and its epoch has
            // not changed, we already know the helper is required.
            hotBr(b.CreateICmpEQ(current, cached), fallback, reload);
            b.SetInsertPoint(reload);
            llvm::Value* items = b.CreateLoad(
                ptrt, b.CreateConstGEP1_32(i8t, list, kListItemsOff));
            b.CreateStore(b.CreateLoad(
                i32t, b.CreateConstGEP1_32(i8t, list, kListLenOff)),
                pl->len);
            b.CreateStore(items, pl->items);
            b.CreateStore(b.CreateLoad(
                b.getInt8Ty(), b.CreateConstGEP1_32(
                    i8t, list, kListNumericOnlyOff)), pl->numericOnly);
            b.CreateStore(current, pl->storageEpoch);
            hotBr(b.CreateICmpNE(
                      items,
                      ConstantPointerNull::get(llvm::cast<PointerType>(ptrt))),
                  contiguous, fallback);
        } else {
            llvm::Value* extra = b.CreateLoad(
                ptrt, b.CreateConstGEP1_32(i8t, list, kListExtraOff));
            hotBr(b.CreateICmpEQ(
                      extra,
                      ConstantPointerNull::get(llvm::cast<PointerType>(ptrt))),
                  contiguous, fallback);
        }
        b.SetInsertPoint(contiguous);
    }

    /// idx(Value) → idx 由来の double を dBB へ feed する分岐を emit。
    /// num → asF64、numState∈{1,3,4} の heap str → numCache を使う。
    /// それ以外（未parse str/NaN str/'last'/'random'/bool/color/""）は
    /// slowBB へ。戻り値は dBB の f64 phi（呼出側で addIncoming）。
    std::pair<BasicBlock*, llvm::Value*> emitListIdxAsF64(
            llvm::Value* idx, BasicBlock* slowBB) {
        // 昇格した数値変数 / 数値リテラルはタグ検査も分岐も不要。
        if (auto it = knownNumD.find(idx); it != knownNumD.end())
            return {b.GetInsertBlock(), it->second};
        if (auto c = constNumV(idx))
            return {b.GetInsertBlock(),
                    cf64(std::isnan(*c) ? 0.0 : *c)};
        BasicBlock* haveBB = b.GetInsertBlock();
        BasicBlock* numDBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* stBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* dBB = BasicBlock::Create(lc, "", fn);
        b.SetInsertPoint(haveBB);
        b.CreateCondBr(emitIsNum(idx), numDBB, strBB);
        b.SetInsertPoint(strBB);
        // heap str のみ（kEmptyStr は tag 0x7FF8 → slowBB）
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(idx, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(isStr, stBB, slowBB);
        b.SetInsertPoint(stBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(idx, ci64(val::kPtrMask)), ptrt);
        llvm::Value* ns = loadSStrCache(i32t, sp, kSStrNumStateOff, true);
        // parse 済み数値（1/3/4）。0=未parse・2=NaN('last'/'random' 含む)
        // → helper。
        llvm::Value* nc = loadSStrCache(f64t, sp, kSStrNumCacheOff, false);
        llvm::Value* cached = b.CreateAnd(
            b.CreateICmpNE(ns, ci32(0)),
            b.CreateICmpNE(ns, ci32(2)));
        b.CreateCondBr(cached, dBB, slowBB);
        b.SetInsertPoint(numDBB);
        llvm::Value* dNum = emitAsF64(idx);
        b.CreateBr(dBB);
        b.SetInsertPoint(dBB);
        PHINode* d = b.CreatePHI(f64t, 2);
        d->addIncoming(dNum, numDBB);
        d->addIncoming(nc, stBB);
        return {dBB, d};
    }

    // Unlike LLVM fptosi, CVTTSD2SI is defined for every input: NaN and
    // overflow produce INT64_MIN. After wrapping subtraction, those become
    // INT64_MAX and fail the unsigned u32-length check, just like negatives.
    // This avoids both poison and the multi-instruction fptosi.sat lowering.
    std::pair<llvm::Value*, llvm::Value*> emitListBounds(
            llvm::Value* d, llvm::Value* len) {
#if defined(__x86_64__) || defined(_M_X64)
        auto* vt = llvm::FixedVectorType::get(f64t, 2);
        llvm::Value* vec = b.CreateInsertElement(
            llvm::UndefValue::get(vt), d, uint64_t(0));
        auto* cvt = llvmc::intrinsicDeclaration(
            &mod, llvm::Intrinsic::x86_sse2_cvttsd2si64);
        llvm::Value* i0 = b.CreateSub(b.CreateCall(cvt, {vec}), ci64(1));
        return {b.CreateICmpULT(i0, b.CreateZExt(len, i64t)), i0};
#else
        llvm::Value* upper = b.CreateFAdd(b.CreateUIToFP(len, f64t), cf64(1.0));
        llvm::Value* inb = b.CreateAnd(b.CreateFCmpOGE(d, cf64(1.0)),
                                      b.CreateFCmpOLT(d, upper));
        return {inb, nullptr}; // Conversion is emitted only after this guard.
#endif
    }

    bool numericListSiteHint(const BlockField* f) {
        if (!f || f->id.empty() || !target || !blocks || aot) return false;
        if (auto it = numericListSiteHints.find(f->id);
            it != numericListSiteHints.end()) return it->second;
        Variable* variable = target->lookupVariableById(f->id);
        // Pre-populated mixed lists must not pay an extra per-read guard.
        bool plausible = variable && variable->list &&
            variable->list->numericOnly;
        // Inspect all visible writers. An unknown ITEM type is treated as
        // potentially nonnumeric. Writers in other targets are not necessarily
        // visible here, so the runtime guard remains essential.
        static const std::unordered_set<std::string_view> numberOps = {
            "math_number", "math_integer", "math_whole",
            "math_positive_number", "operator_add", "operator_subtract",
            "operator_multiply", "operator_divide", "operator_mod",
            "operator_round", "operator_mathop", "operator_random",
            "data_lengthoflist", "sensing_timer", "sensing_dayssince2000",
            "motion_xposition", "motion_yposition", "motion_direction",
        };
        if (plausible) for (const auto& [id, writer] : blocks->allBlocks()) {
            if (writer.opcode != "data_addtolist" &&
                writer.opcode != "data_insertatlist" &&
                writer.opcode != "data_replaceitemoflist") continue;
            const BlockField* listField = findField(
                const_cast<Block*>(&writer), "LIST");
            if (!listField || listField->id != f->id) continue;
            const BlockInput* input = writer.inputs.find("ITEM");
            const std::string source = input
                ? (input->block.empty() ? input->shadow : input->block)
                : std::string();
            Block* value = source.empty() ? nullptr : blocks->getBlock(source);
            if (!value || !numberOps.count(value->opcode)) {
                plausible = false;
                break;
            }
        }
        numericListSiteHints.emplace(f->id, plausible);
        return plausible;
    }

    llvm::Value* emitListItem(Block* blk, const BlockField* f) {
        if (xyo_list_storage_mode >= 4 && xyo_list_storage_mode <= 6) {
            llvm::Value* idx = emitInputOf(blk, "INDEX");
            return callN("xyo_list_item", i64t,
                         {ctxArg, varSlot(f, true), idx});
        }
        llvm::Value* idx = emitInputOf(blk, "INDEX");
        const PromotedList* pl = findPromotedList(f->id);
        llvm::Value* l = varListPtr(f);   // null 可（list 非保有）
        llvm::Value* nonNull = b.CreateICmpNE(
            b.CreatePtrToInt(l, i64t), ci64(0));
        BasicBlock* haveBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* emptyBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        // Null promoted lists have len=0/items=null. The bounds check rejects
        // every numeric index before loading items, so no second null test.
        if (pl && (xyo_list_storage_mode == 7 ||
                   !std::getenv("XYO_LIST_READ_NULL_CHECK")))
            b.CreateBr(haveBB);
        else b.CreateCondBr(nonNull, haveBB, slowBB);
        b.SetInsertPoint(haveBB);
        guardAdaptiveContiguous(l, pl, slowBB);
        auto [dBB, d] = emitListIdxAsF64(idx, slowBB);
        b.SetInsertPoint(dBB);
        llvm::Value* len = pl ? b.CreateLoad(i32t, pl->len) : b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        auto [inb, i0] = emitListBounds(d, len);
        hotBr(inb, fastBB, emptyBB);
        b.SetInsertPoint(fastBB);
        if (!i0) i0 = b.CreateSub(b.CreateFPToSI(d, i64t), ci64(1));
        llvm::Value* items = pl ? b.CreateLoad(ptrt, pl->items) : b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
        llvm::Value* v = b.CreateLoad(i64t, b.CreateGEP(i64t, items, i0));
        struct ListLowerer {
            IRBuilder<>& builder;
            llvm::Type* i8Type;
            llvm::Value* shadowFlag;
            bool useShadow;
            llvm::Value* numericListGuard(llvm::Value* list) {
                llvm::Value* ptr = useShadow
                    ? shadowFlag
                    : builder.CreateConstGEP1_32(
                          builder.getInt8Ty(), list, ir::kListNumericOnlyOffset);
                return builder.CreateICmpNE(
                    builder.CreateLoad(i8Type, ptr),
                    llvm::ConstantInt::get(i8Type, 0));
            }
        } listBackend{b, b.getInt8Ty(),
                      pl ? pl->numericOnly : nullptr,
                      pl && readMemoAllowed};
        llvm::Value* numeric = ir::lowerNumericListGuard(listBackend, l);
        b.CreateBr(merge);
        b.SetInsertPoint(emptyBB);
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* s = callN("xyo_list_item", i64t,
                               {ctxArg, varSlot(f, true), idx});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* phi = b.CreatePHI(i64t, 3);
        phi->addIncoming(v, fastBB);
        phi->addIncoming(ci64(Value::emptyStr().bits), emptyBB);
        phi->addIncoming(s, slowBB);
        // Only emit the extra list-wide type guard when compile-time feedback
        // suggests numeric reads. Slow/empty reads still use Scratch coercion.
        if (const char* env = std::getenv("XYO_JIT_NUMERIC_LIST");
            env && env[0] == '1' && numericListSiteHint(f)) {
            auto* numericGuard = b.CreatePHI(b.getInt1Ty(), 3);
            numericGuard->addIncoming(numeric, fastBB);
            numericGuard->addIncoming(b.getFalse(), emptyBB);
            numericGuard->addIncoming(b.getFalse(), slowBB);
            numericListReadGuards.emplace(phi, numericGuard);
        }
        // A storage transition moves Value slots but never changes the
        // values in this read-only promotable loop. Numeric coercion memo
        // remains valid even when adaptive storage is demoted to flat.
        if (promotingLoop && readMemoAllowed && readMemoPreheader)
            memoizableListReads.insert(phi);
        return phi;
    }

    void emitListReplace(Block* blk, const BlockField* f) {
        if (xyo_list_storage_mode >= 4 && xyo_list_storage_mode <= 6) {
            llvm::Value* idx = emitInputOf(blk, "INDEX");
            llvm::Value* item = emitInputOf(blk, "ITEM");
            callN("xyo_list_replace", voidt,
                  {ctxArg, varSlot(f, true), idx, item});
            return;
        }
        llvm::Value* idx = emitInputOf(blk, "INDEX");
        llvm::Value* item = emitInputOf(blk, "ITEM");
        llvm::Value* l = varListPtr(f);
        const PromotedList* pl = findPromotedList(f->id);
        llvm::Value* nonNull = b.CreateICmpNE(
            b.CreatePtrToInt(l, i64t), ci64(0));
        BasicBlock* haveBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        // For a promoted, existing list, the preheader and yield reloads
        // cache len=0/items=null on a missing SList*. Numeric indices then
        // fail the bounds check; special string indices use the helper.
        //
        // A malformed Scratch project may reuse a scalar variable ID for a
        // list field. lookupOrCreateList() then returns the scalar and the
        // slow helper must create its missing SList on first access. Do not
        // eliminate this observable lazy initialization in that case.
        const Variable* knownList = target && !f->id.empty()
            ? target->lookupVariableById(f->id) : nullptr;
        const bool alreadyList = knownList &&
            knownList->type == Variable::kListType && knownList->list;
        // XYO_JIT_REPLACE_NO_NULL=0 restores the historical null branch.
        const char* noNull = std::getenv("XYO_JIT_REPLACE_NO_NULL");
        const bool noNullFast = !noNull || noNull[0] != '0';
        if (pl && (xyo_list_storage_mode == 7 ||
                   (noNullFast && alreadyList)))
            b.CreateBr(haveBB);
        else if (const char* likely = std::getenv("XYO_JIT_REPLACE_LIKELY_NONNULL");
                 likely && likely[0] == '1')
            hotBr(nonNull, haveBB, slowBB, 1000, 1);
        else
            b.CreateCondBr(nonNull, haveBB, slowBB);
        b.SetInsertPoint(haveBB);
        guardAdaptiveContiguous(l, pl, slowBB);
        auto [dBB, d] = emitListIdxAsF64(idx, slowBB);
        b.SetInsertPoint(dBB);
        llvm::Value* len = pl ? b.CreateLoad(i32t, pl->len) : b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        // INDEX and ITEM have already been evaluated in their original order.
        auto [inb, i0] = emitListBounds(d, len);
        hotBr(inb, fastBB, merge);
        b.SetInsertPoint(fastBB);
        if (!i0) i0 = b.CreateSub(b.CreateFPToSI(d, i64t), ci64(1));
        llvm::Value* items = pl ? b.CreateLoad(ptrt, pl->items) : b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
        invalidateNumericList(l, item, true);
        b.CreateStore(item, b.CreateGEP(i64t, items, i0));
        b.CreateStore(b.getInt8(1),
                      b.CreateConstGEP1_32(i8t, l, kListDirtyOff));
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        callN("xyo_list_replace", voidt,
              {ctxArg, varSlot(f, true), idx, item});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    llvm::GlobalVariable* listLimitGlobal() {
        if (auto* g = mod.getGlobalVariable("xyo_list_limit")) return g;
        return new GlobalVariable(mod, i32t, false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "xyo_list_limit");
    }

    /// data_addtolist — l!=null && len<cap なら store/len++/dirty のみ。
    /// null・満杯（要 ensure）は helper（SList 生成/realloc 込み）。
    /// par-for の本体では「base+iter 番目のスロット書き込み」に置き換える
    /// （len は並列領域の前に確保済み）。
    void emitListAdd(Block* blk, const BlockField* f) {
        if (xyo_list_storage_mode >= 4 && xyo_list_storage_mode <= 6) {
            llvm::Value* item = emitInputOf(blk, "ITEM");
            callN("xyo_list_add", voidt,
                  {ctxArg, varSlot(f, true), item});
            return;
        }
        if (parFor) {
            auto k = parFor->index.find(f->id);
            if (k != parFor->index.end()) {
                llvm::Value* item = emitInputOf(blk, "ITEM");
                llvm::Value* l = b.CreateLoad(
                    ptrt, b.CreateGEP(ptrt, parFor->lists, ci64(k->second)));
                llvm::Value* base = b.CreateLoad(
                    i64t, b.CreateGEP(i64t, parFor->bases, ci64(k->second)));
                llvm::Value* idx = b.CreateAdd(base, parFor->iter);
                // The par-for runtime has already checked bounds and
                // reserved [base, base+trip) before invoking this body.
                // Its Gc::DeferCollect pins the list and items storage for
                // the duration of the batch. Skip a C ABI call per element.
                // Storage mode >=4 never enters this body (runtime fallback).
                // Default on after parity and paired A/B validation;
                // XYO_PARFOR_INLINE_SLOT=0 keeps the original checked helper.
                static const bool inlineParSlot = [] {
                    const char* env = std::getenv("XYO_PARFOR_INLINE_SLOT");
                    return !env || env[0] != '0';
                }();
                if (inlineParSlot) {
                    llvm::Value* items = b.CreateLoad(
                        ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
                    b.CreateStore(item, b.CreateGEP(i64t, items, idx));
                } else {
                    callN("xyo_par_slot", voidt, {l, idx, item});
                }
                return;
            }
        }
        llvm::Value* item = emitInputOf(blk, "ITEM");
        llvm::Value* l = varListPtr(f);
        llvm::Value* nonNull = b.CreateICmpNE(
            b.CreatePtrToInt(l, i64t), ci64(0));
        BasicBlock* haveBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(nonNull, haveBB, slowBB);
        b.SetInsertPoint(haveBB);
        guardAdaptiveContiguous(l, nullptr, slowBB);
        llvm::Value* len = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        llvm::Value* cap = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListCapOff));
        // リスト長上限（0 = 無制限）。上限に達していたら helper 経由にして
        // 追加を落とす（SList::push が上限を見る）。
        llvm::Value* lim = b.CreateLoad(i32t, listLimitGlobal());
        llvm::Value* room = b.CreateOr(b.CreateICmpEQ(lim, ci32(0)),
                                       b.CreateICmpULT(len, lim));
        b.CreateCondBr(
            b.CreateAnd(b.CreateICmpULT(len, cap), room), fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* items = b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
        invalidateNumericList(l, item);
        b.CreateStore(item,
                      b.CreateGEP(i64t, items,
                                  b.CreateZExt(len, i64t)));
        b.CreateStore(b.CreateAdd(len, ci32(1)),
                      b.CreateConstGEP1_32(i8t, l, kListLenOff));
        b.CreateStore(b.getInt8(1),
                      b.CreateConstGEP1_32(i8t, l, kListDirtyOff));
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        callN("xyo_list_add", voidt,
              {ctxArg, varSlot(f, true), item});
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
    }

    llvm::Value* emitListLen(const BlockField* f) {
        llvm::Value* l = varListPtr(f);
        llvm::Value* nonNull = b.CreateICmpNE(
            b.CreatePtrToInt(l, i64t), ci64(0));
        BasicBlock* haveBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* zeroBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(nonNull, haveBB, zeroBB);
        b.SetInsertPoint(haveBB);
        llvm::Value* len = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        // len は u32 — uitofp は常に有限値（NaN にならない）
        llvm::Value* dn = b.CreateBitCast(b.CreateUIToFP(len, f64t), i64t);
        b.CreateBr(merge);
        b.SetInsertPoint(zeroBB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* phi = b.CreatePHI(i64t, 2);
        phi->addIncoming(dn, haveBB);
        phi->addIncoming(ci64(Value::num(0).bits), zeroBB);
        return phi;
    }


    bool isLoweredStackOp(const std::string& op) {
        static const std::set<std::string> k = {
            "control_if", "control_if_else", "control_repeat",
            "control_forever", "control_while", "control_repeat_until",
            "control_for_each", "control_all_at_once", "control_wait",
            "control_wait_until", "control_stop", "data_setvariableto",
            "data_changevariableby", "data_replaceitemoflist",
            "procedures_call", "procedures_return",
            "event_broadcast", "event_broadcastandwait",
            "pen_penUp", "pen_penDown", "pen_clear", "pen_stamp",
            "pen_setPenColorToColor", "pen_changePenColorParamBy",
            "pen_setPenColorParamTo", "pen_setPenSizeTo",
            "pen_changePenSizeBy",
        };
        return k.count(op) != 0;
    }

    bool hasSubstackInput(const Block* blk) {
        return blk->inputs.has("SUBSTACK") || blk->inputs.has("SUBSTACK2");
    }

    /// compat 可能なスタックオペか（分岐・制御フローでない）
    bool isCompatStackOp(const Block* blk) {
        if (hasSubstackInput(blk)) return false;
        const std::string& op = blk->opcode;
        // これらは必ず lowering が必要
        if (op == "procedures_call" || op == "procedures_return")
            return false;
        return true;
    }

    Block* getBlock(const std::string& id) {
        if (id.empty()) return nullptr;
        Block* blk = blocks->getBlock(id);
        if (!blk && runtime.flyoutBlocks)
            blk = runtime.flyoutBlocks->getBlock(id);
        return blk;
    }

    /// チェーン構築（next リンクを辿る）
    std::vector<Block*> chainOf(const std::string& firstId) {
        std::vector<Block*> out;
        std::set<std::string> seen;
        for (std::string id = firstId; !id.empty();) {
            Block* blk = getBlock(id);
            if (!blk || seen.count(id)) break;
            seen.insert(id);
            out.push_back(blk);
            id = blk->next;
        }
        return out;
    }

    /// スキャン済み手続き（proccode）。手続きは emitProcedureCall で
    /// 個別 Function として emit されるため再帰呼出しはそのまま動く。
    /// ここでのセットは再帰によるスキャンの無限ループ防止のみ。
    std::set<std::string> scannedProcs;

    /// 手続き Function 本体を emit 中か（stop/return の巻き戻し先が変わる）
    bool inProc = false;

    /// スクリプト全体がコンパイル可能か（事前スキャン）
    bool scanChain(const std::vector<Block*>& chain) {
        for (Block* blk : chain) {
            const std::string& op = blk->opcode;
            if (isLoweredStackOp(op)) {
                for (const char* sn : {"SUBSTACK", "SUBSTACK2"}) {
                    const BlockInput* in = blk->inputs.find(sn);
                    if (in && !in->block.empty()) {
                        std::vector<Block*> sub = chainOf(in->block);
                        if (!scanChain(sub)) return false;
                    }
                }
                if (op == "procedures_return") {
                    // TurboWarp 拡張: reporter ABI・waitingReporter 等の
                    // 完全な意味論は未実装 — interpreter に任せる
                    if (verbose) std::fprintf(stderr,
                        "xyo-jit: bail: procedures_return\n");
                    return false;
                }
                if (op == "procedures_call" && blk->mutation) {
                    const std::string pc = blk->mutation->attr("proccode");
                    if (verbose) std::fprintf(stderr,
                        "xyo-jit: scan proc '%s'\n", pc.c_str());
                    if (!scannedProcs.insert(pc).second) continue;
                    std::string def =
                        blocks->getProcedureDefinition(pc);
                    // 未定義手続き呼び出しは no-op（TW irgen の
                    // getProcedureInfo が NOP を返すのと同じ —
                    // interp の procCall も prototype 無しで早期
                    // return）。emit 側で何も出力しない。
                    Block* defBlk = def.empty() ? nullptr : getBlock(def);
                    if (!defBlk) {
                        if (verbose) std::fprintf(stderr,
                            "xyo-jit: no proc def for '%s' -> nop\n",
                            pc.c_str());
                        continue;
                    }
                    std::vector<Block*> body = chainOf(defBlk->next);
                    if (verbose) {
                        std::fprintf(stderr,
                            "xyo-jit: def '%s' -> %s body:", pc.c_str(),
                            def.c_str());
                        for (Block* x : body)
                            std::fprintf(stderr, " %s", x->opcode.c_str());
                        std::fprintf(stderr, "\n");
                    }
                    if (!scanChain(body)) return false;
                }
            } else if (!isCompatStackOp(blk)) {
                if (verbose) std::fprintf(stderr,
                    "xyo-jit: bail: non-compat op %s\n", op.c_str());
                return false;
            }
        }
        return true;
    }

    // --- 入力 emit ---------------------------------------------------------

    /// 定数値（shadow field）を i64 に。文字列は JIT なら pin、
    /// AOT なら slot 経由でロード時に intern する。
    llvm::Value* constValue(Value v) {
        if (v.isString()) {
            SStr* s = v.asStrPtr();
            llvm::Value* bits;
            if (aot) {
                std::u16string u =
                    s ? std::u16string(s->view()) : std::u16string();
                bits = strLitBits(u, s);
            } else {
                if (s) pinned.strings.push_back(s);
                bits = ci64(v.bits);
            }
            // This explicit immutable literal boundary also covers defaults and
            // static fields, not reporter evaluation. Preserve its boxed string
            // for string/list/compare uses; only numeric conversion may fold.
            // Shared cast semantics include empty strings, NaN -> 0 and -0/Inf.
            if (!genericLiteralNum) {
                literalNums[bits] = cast::toNumber(v);
                // Scratch comparison must distinguish a failed numeric
                // parse from numeric zero. cast::toNumber maps NaN to 0
                // for arithmetic, but compare("abc", 0) is string-based.
                literalCmpNums[bits] =
                    cast::isWhiteSpace(v)
                        ? std::numeric_limits<double>::quiet_NaN()
                        : cast::toNumberOrNaN(v);
            }
            return bits;
        }
        return ci64(v.bits);
    }

    /// shadow ブロック → フィールド値定数（なければ nullptr）
    llvm::Value* shadowConstant(Block* blk) {
        if (!blk || !blk->shadow) return nullptr;
        if (blk->fields.empty()) return nullptr;
        const BlockField& f = blk->fields.begin()->second;
        if (blk->opcode == "event_broadcast_menu") {
            // JS irgen: コンパイル時に broadcastMsg を名前解決し定数化
            std::u16string nm;
            if (f.value.isString() && f.value.asStrPtr())
                nm = std::u16string(f.value.asStrPtr()->view());
            Variable* bv = target->lookupBroadcastMsg(f.id, nm);
            if (!bv) return ci64(Value::emptyStr().bits);
            if (aot) return strLitBits(bv->name, nullptr);
            SStr* s = SStr::create(bv->name);
            pinned.strings.push_back(s);
            return ci64(Value::string(s).bits);
        }
        return constValue(f.value);
    }

    /// parent.inputs[name] の評価コードを生成
    /// （JS descendInputOfBlock: 欠落入力は createConstantInput(0) = 数値0）
    llvm::Value* emitInputOf(Block* parent, const std::string& name) {
        const BlockInput* in = parent->inputs.find(name);
        if (!in) return ci64(Value::num(0).bits);
        std::string childId = !in->block.empty() ? in->block : in->shadow;
        if (childId.empty()) {
            if (!in->shadow.empty()) childId = in->shadow;
            else return ci64(Value::num(0).bits);
        }
        return emitInputBlock(childId);
    }

    llvm::Value* hoistedNestedOsrArg(const std::u16string& name) {
        if (auto it = hoistedNestedOsrArgs.find(name);
            it != hoistedNestedOsrArgs.end())
            return it->second;

        // Insert into the standalone entry, never inside the repeat body:
        // procedure arguments cannot mutate within the selected safe region.
        // No Scratch reporter is evaluated and no block is counted here.
        auto& entry = entryFn->getEntryBlock();
        IRBuilder<> init(&entry, entry.begin());
        auto* peek = init.CreateCall(
            decl("xyo_rt_nested_osr_arg_peek",
                 i64t, {ptrt, ptrt, i32t}),
            {ctxArg, u16g(name), ci32(uint32_t(name.size()))},
            "osr.arg.rooted");
        hoistedNestedOsrArgs.emplace(name, peek);
        return peek;
    }

    llvm::Value* emitInputBlock(const std::string& id) {
        Block* blk = getBlock(id);
        if (!blk) return ci64(Value::undefined().bits);
        // Count at the end of the generated expression, after its children.
        // The interpreter owns counting when we delegate a reporter to it.
        const auto* cached = runtime.blockCountEnabled
            ? blocks->getCachedExecute(id) : nullptr;
        ReporterCountGuard countGuard{*this, cached && cached->blockFunction};
        // data_variable は shadow 位置でもライブ評価（JS blocks-execute-cache
        // と同じ — ドロップダウン内変数は実行時に値を返す）
        if (blk->opcode == "data_variable") {
            const BlockField* f = findField(blk, "VARIABLE");
            if (!f) return call3("xyo_var_get", i64t, ctxArg, varIdC(f),
                                 varNameU16(f));
            return emitVarGet(f);
        }
        if (blk->shadow) {
            llvm::Value* c = shadowConstant(blk);
            if (c) return c;
            return ci64(Value::emptyStr().bits);
        }
        const std::string& op = blk->opcode;
        // Blocks::getCachedExecute treats field-only primitive reporters as
        // constant shadow values even when their JSON shadow bit is false.
        // Mirror that rule: falling back to xyo_rt_reporter here can return
        // undefined, notably for a connected non-shadow colour_picker.
        if (blk->inputs.empty() && blk->fields.size() == 1 &&
            (op == "colour_picker" || op == "text" ||
             op == "math_number" || op == "math_integer" ||
             op == "math_whole_number" || op == "math_positive_number" ||
             op == "math_angle" || op == "math_boolean"))
            return constValue(blk->fields.begin()->second.value);

        if (op == "argument_reporter_string_number" ||
            op == "argument_reporter_boolean" ||
            op == "argument_reporter" ||
            op.rfind("argument_", 0) == 0) {
            // フィールド VALUE が引数名
            const BlockField* f = blk->fields.empty()
                ? nullptr : &blk->fields.begin()->second;
            std::u16string pname;
            if (f && f->value.isString() && f->value.asStrPtr())
                pname = f->value.asStrPtr()->view();
            auto it = argIndex.find(pname);
            if (it == argIndex.end()) {
                // Only an independently validated standalone OSR loop can
                // read the original interpreter procedure frame directly.
                // Missing/undefined arguments fall back to the exact normal
                // reporter implementation (including special names).
                countGuard.active = false;
                if (directNestedOsrArgs &&
                    (op == "argument_reporter_string_number" ||
                     op == "argument_reporter_boolean")) {
                    if (hoistNestedOsrArgs) {
                        llvm::Value* rooted = hoistedNestedOsrArg(pname);
                        // Undefined arguments (including special built-in
                        // names) must be re-evaluated at their actual reporter
                        // PC, not at native entry or once per entire loop.
                        auto* fast = BasicBlock::Create(lc, "osr.arg.fast", fn);
                        auto* slow = BasicBlock::Create(lc, "osr.arg.slow", fn);
                        auto* done = BasicBlock::Create(lc, "osr.arg.done", fn);
                        b.CreateCondBr(b.CreateICmpNE(
                            rooted, ci64(Value::undefined().bits)), fast, slow);
                        b.SetInsertPoint(fast);
                        if (cached && cached->blockFunction)
                            emitBlockCount();
                        b.CreateBr(done);
                        b.SetInsertPoint(slow);
                        llvm::Value* dynamic = call2(
                            "xyo_rt_reporter", i64t, ctxArg,
                            b.CreateGlobalString(id));
                        b.CreateBr(done);
                        b.SetInsertPoint(done);
                        auto* value = b.CreatePHI(i64t, 2, "osr.arg.value");
                        value->addIncoming(rooted, fast);
                        value->addIncoming(dynamic, slow);
                        return value;
                    }
                    return callN("xyo_rt_nested_osr_arg_get", i64t,
                        {ctxArg, b.CreateGlobalString(id), u16g(pname),
                         ci32(uint32_t(pname.size()))});
                }
                return call2("xyo_rt_reporter", i64t, ctxArg,
                             b.CreateGlobalString(id));
            }
            return emitArgGet(it->second);
        }

        // --- 演算 ---
        // 注意: call1/call2/call3 の引数式に emitInputOf を複数書くと
        // C++ 引数評価順（未規定・実際は右→左）に IR emit 順が依存し、
        // reporter 副作用（手続き呼出しの yield 等）の順序が壊れる。
        // 複数入力は必ず callOp 系で左→右に逐次 emit する。
        if (op == "operator_add")
            return emitArith(blk, "NUM1", "NUM2", AOp::Add);
        if (op == "operator_subtract")
            return emitArith(blk, "NUM1", "NUM2", AOp::Sub);
        if (op == "operator_multiply")
            return emitArith(blk, "NUM1", "NUM2", AOp::Mul);
        if (op == "operator_divide")
            return emitArith(blk, "NUM1", "NUM2", AOp::Div);
        if (op == "operator_mod")
            return emitArith(blk, "NUM1", "NUM2", AOp::Mod);
        if (op == "operator_lt")
            return boolToValue(emitCmp(blk, "OPERAND1", "OPERAND2",
                                       "xyo_lt", CmpInst::FCMP_OLT));
        if (op == "operator_gt")
            return boolToValue(emitCmp(blk, "OPERAND1", "OPERAND2",
                                       "xyo_gt", CmpInst::FCMP_OGT));
        if (op == "operator_equals")
            return boolToValue(emitEq(blk));
        if (op == "operator_and") {
            llvm::Value* a = emitTruthyV(emitInputOf(blk, "OPERAND1"));
            llvm::Value* bv = emitTruthyV(emitInputOf(blk, "OPERAND2"));
            return boolBits(b.CreateAnd(a, bv));
        }
        if (op == "operator_or") {
            llvm::Value* a = emitTruthyV(emitInputOf(blk, "OPERAND1"));
            llvm::Value* bv = emitTruthyV(emitInputOf(blk, "OPERAND2"));
            return boolBits(b.CreateOr(a, bv));
        }
        if (op == "operator_not")
            return boolBits(
                b.CreateNot(emitTruthyV(emitInputOf(blk, "OPERAND"))));
        if (op == "operator_random") {
            return callOpCtx("xyo_random", i64t, blk, "FROM", "TO");
        }
        if (op == "operator_round") {
            llvm::Value* x = emitToNumF64(emitInputOf(blk, "NUM"));
            // JS Math.round は .5 を +Infinity 方向へ丸め、-0 を保持する。
            // llvm.round + 0.0 は負の .5 と -0 で Scratch と違う。
            // Baseline AOT must not emit an external roundeven libcall:
            // the intrinsic needs SSE4.1 for an inlined lowering, but that
            // instruction set is not guaranteed on portable x86-64 targets
            // and the roundeven symbol is absent on some Windows runtimes.
            // Keep the portable, already-registered helper for AOT; JIT
            // retains the fast native inline lowering.
            llvm::Value* rounded = inlineJsRound && !aot
                ? emitJsRound(x) : call1("xyo_round", f64t, x);
            llvm::Value* result = pureMathFacts
                ? emitCanonKnownNonNaN(rounded) : emitCanon(rounded);
            // Math.round always produces an exact integral double or +/-inf;
            // canonical NaN repair also produces an exact integer zero.
            noteIntegralBox(result);
            return result;
        }
        if (op == "operator_mathop") {
            static const std::map<std::string, int32_t> ops = {
                {"abs", 0}, {"floor", 1}, {"ceiling", 2}, {"sqrt", 3},
                {"sin", 4}, {"cos", 5}, {"tan", 6}, {"asin", 7},
                {"acos", 8}, {"atan", 9}, {"ln", 10}, {"log", 11},
                {"e ^", 12}, {"10 ^", 13},
            };
            const BlockField* f = findField(blk, "OPERATOR");
            std::string fname;
            if (f && f->value.isString() && f->value.asStrPtr())
                fname = SStr::toUtf8(f->value.asStrPtr()->view());
            // Scratch は menu 値を toLowerCase して照合し、未知の値は 0 を返す
            // （scratch3_operators.mathop の default）。大文字始まりの値
            // （"Sqrt" 等）で abs にフォールバックしていたのを修正。
            for (char& c : fname)
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            int32_t fn = -1;
            auto it = ops.find(fname);
            if (it != ops.end()) fn = it->second;
            llvm::Value* x = emitToNumF64(emitInputOf(blk, "NUM"));
            if (fn < 0) {
                llvm::Value* zero = cf64(0.0);
                return genericMathBox ? call1("xyo_num", i64t, zero)
                                      : emitCanon(zero);
            }
            llvm::Value* r = nullptr;
            if ((fn == 4 || fn == 5) && trigHoistAllowed &&
                promotingLoop && readMemoPreheader &&
                readMemoPreheader->getTerminator()) {
                const BlockInput* input = blk->inputs.find("NUM");
                Block* source = input && !input->block.empty()
                    ? getBlock(input->block) : nullptr;
                const BlockField* variable = source &&
                    source->opcode == "data_variable"
                    ? findField(source, "VARIABLE") : nullptr;
                PromotedVar* promotedAngle = variable
                    ? findPromoted(variable) : nullptr;
                if (promotedAngle && promotedAngle->numeric &&
                    !promotedAngle->written) {
                    auto existing = std::find_if(
                        trigHoists.begin(), trigHoists.end(),
                        [&](const TrigHoist& h) {
                            return h.variableId == variable->id &&
                                h.mathop == fn;
                        });
                    if (existing == trigHoists.end()) {
                        AllocaInst* slot = entryAlloca(f64t, 1, "trig.hoisted");
                        auto saved = b.saveIP();
                        b.SetInsertPoint(readMemoPreheader->getTerminator());
                        llvm::Value* initial = b.CreateLoad(
                            f64t, promotedAngle->dslot);
                        b.CreateStore(emitMathop(fn, initial), slot);
                        b.restoreIP(saved);
                        trigHoists.push_back({variable->id, fn, slot});
                        r = b.CreateLoad(f64t, slot);
                    } else {
                        r = b.CreateLoad(f64t, existing->value);
                    }
                }
            }
            if (!r) r = emitMathop(fn, x);
            // xyo_num is exactly Value::numCanonical. Keep its NaN/Inf/±0
            // semantics inline, and expose the numeric result to consumers so
            // immediate arithmetic need not re-test bool/string tags.
            // These functions cannot produce NaN from a non-NaN number.
            // Scratch's emitToNumF64 already repairs all NaN inputs.
            // sqrt/log/sin/cos and domain-restricted inverse trig are excluded.
            const bool nonNanFn = fn == 0 || fn == 1 || fn == 2 ||
                                  fn == 9 || fn == 12 || fn == 13;
            llvm::Value* result = genericMathBox
                ? call1("xyo_num", i64t, r)
                : pureMathFacts && nonNanFn
                    ? emitCanonKnownNonNaN(r) : emitCanon(r);
            if (!genericMathBox && (fn == 1 || fn == 2))
                noteIntegralBox(result); // floor and ceiling
            return result;
        }

        // --- sensing 軽量 ---
        // The original reporter samples the system clock for every read.
        // Returning f64 directly avoids a redundant Value tag classification
        // without caching/reordering clock reads (which would change Scratch).
        // Set XYO_JIT_DAYS_F64=0 to restore the historical boxed slow path.
        static const bool daysF64 = [] {
            const char* env = std::getenv("XYO_JIT_DAYS_F64");
            return !env || env[0] != '0';
        }();
        if (op == "sensing_dayssince2000" && daysF64 &&
            !sharedDaysClock) {
            // rt::daysSince2000 returns finite milliseconds-derived days.
            // Keep IEEE-754 arithmetic exact; no fast-math assumptions.
            llvm::Value* days = call1(
                "xyo_sns_dayssince2000_f64", f64t, ctxArg);
            return emitCanonKnownNonNaN(days);
        }
        if (op == "sensing_timer") {
            llvm::Value* d = call1("xyo_timer", f64t, ctxArg);
            return call1("xyo_num", i64t, d);
        }
        if (op == "sensing_dayssince2000" && sharedDaysClock) {
            auto* slot = b.CreateConstGEP1_32(
                i8t, ctxArg, uint32_t(boxedDaysClock
                    ? offsetof(JitCtx, sampledUnixDaysBits)
                    : offsetof(JitCtx, sampledUnixMs)));
            auto* clock = b.CreateLoad(ptrt, slot);
            BasicBlock* sampled = newBB("days.sampled");
            BasicBlock* fallback = newBB("days.fallback");
            BasicBlock* merge = newBB("days.merge");
            hotBr(b.CreateICmpNE(clock, cnull()), sampled, fallback, 1000, 1);
            b.SetInsertPoint(sampled);
            auto* sample = b.CreateLoad(i64t, clock);
            sample->setAtomic(AtomicOrdering::Monotonic);
            llvm::Value* cachedBits = nullptr;
            if (boxedDaysClock) {
                // The watchdog already performed the exact integer-ms
                // Scratch conversion and NaN-boxing for this sample.
                cachedBits = sample;
            } else {
                // Match rt::daysSince2000's operation order and Date.now's
                // millisecond precision, including OS wall-clock changes.
                auto* delta = b.CreateFSub(
                    b.CreateSIToFP(sample, f64t), cf64(946684800000.0));
                auto* days = b.CreateFDiv(delta, cf64(86400000.0));
                cachedBits = emitCanonKnownNonNaN(days);
            }
            b.CreateBr(merge);
            b.SetInsertPoint(fallback);
            auto* freshBits = call1("xyo_sns_dayssince2000", i64t, ctxArg);
            b.CreateBr(merge);
            b.SetInsertPoint(merge);
            auto* result = b.CreatePHI(i64t, 2);
            result->addIncoming(cachedBits, sampled);
            result->addIncoming(freshBits, fallback);
            return result;
        }
        if (op == "sensing_userid")
            return ci64(Value::undefined().bits);   // interp も noop=undefined

        // --- リスト系レポータ（id+name で実行時解決）---
        if (op == "data_itemoflist" || op == "data_itemnumoflist" ||
            op == "data_lengthoflist" || op == "data_listcontainsitem" ||
            op == "data_listcontents") {
            const BlockField* f = findField(blk, "LIST");
            if (!f) {
                // フィールド欠落 = インタプリタの obj()==nullptr 経路
                if (op == "data_itemoflist" || op == "data_listcontents")
                    return ci64(Value::emptyStr().bits);
                if (op == "data_listcontainsitem")
                    return ci64(val::kFalse);
                return ci64(Value::num(0).bits);
            }
            if (op == "data_itemoflist")
                return emitListItem(blk, f);
            if (op == "data_lengthoflist")
                return emitListLen(f);
            llvm::Value* v = varSlot(f, true);
            if (op == "data_itemnumoflist")
                return callN("xyo_list_itemnum", i64t,
                    {ctxArg, v, emitInputOf(blk, "ITEM")});
            if (op == "data_listcontainsitem")
                return callN("xyo_list_contains", i64t,
                    {ctxArg, v, emitInputOf(blk, "ITEM")});
            return callN("xyo_list_contents", i64t, {ctxArg, v});
        }

        // --- sensing_mousedown — runtime->ioDevices.mouse.isDown_ 直接
        if (op == "sensing_mousedown") {
            llvm::Value* rt = ctxPtr(2);
            llvm::Value* v = b.CreateLoad(i8t, fld(rt, ly().mseDown));
            return b.CreateSelect(b.CreateICmpNE(v, ci8(0)),
                                  ci64(val::kTrue), ci64(val::kFalse));
        }

        // Fold only actual text shadows selected by the normal input lookup.
        // Never substitute a covered shadow for a live reporter, or stringify
        // numeric shadows at compile time.
        if (op == "operator_join") {
            auto textShadow = [&](const char* name) -> const BlockField* {
                const BlockInput* in = blk->inputs.find(name);
                if (!in) return nullptr;
                Block* child = getBlock(in->block.empty() ? in->shadow : in->block);
                if (!child || !child->shadow || child->opcode != "text") return nullptr;
                const BlockField* f = findField(child, "TEXT");
                return f && f->value.isString() ? f : nullptr;
            };
            const BlockField* left = textShadow("STRING1");
            const BlockField* right = textShadow("STRING2");
            // General associative constant folding of nested joins:
            // join(join(x, "a"), "b") becomes join(x, "ab").
            // Only the suffixes are literal shadows; x is evaluated exactly
            // once and may be any reporter/value. No loop pattern is required.
            // A bounded fold prevents unreasonable compile-time allocations.
            std::vector<const BlockField*> suffixes;
            Block* inner = blk;
            Block* lastJoin = nullptr;
            size_t totalSuffixUnits = 0;
            for (unsigned i = 0; i < 8 && inner &&
                                 inner->opcode == "operator_join"; ++i) {
                const BlockInput* rhsInput = inner->inputs.find("STRING2");
                const BlockInput* lhsInput = inner->inputs.find("STRING1");
                if (!rhsInput || !lhsInput) break;
                Block* rhsBlock = getBlock(rhsInput->block.empty()
                                              ? rhsInput->shadow : rhsInput->block);
                if (!rhsBlock || !rhsBlock->shadow || rhsBlock->opcode != "text")
                    break;
                const BlockField* field = findField(rhsBlock, "TEXT");
                if (!field || !field->value.isString()) break;
                const SStr* str = field->value.asStrPtr();
                const size_t size = str ? str->len : 0;
                if (totalSuffixUnits + size > 512) break;
                suffixes.push_back(field);
                totalSuffixUnits += size;
                lastJoin = inner;
                if (lhsInput->block.empty()) break;
                inner = getBlock(lhsInput->block);
            }
            if (suffixes.size() >= 2 && !runtime.blockCountEnabled) {
                std::u16string combined;
                combined.reserve(totalSuffixUnits);
                for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it)
                    if (SStr* str = (*it)->value.asStrPtr())
                        combined.append(str->view());
                llvm::Value* dynamicBase = emitInputOf(lastJoin, "STRING1");
                return call2("xyo_join", i64t, dynamicBase,
                             constValue(Value::string(SStr::create(combined))));
            }

            if (left && right) {
                std::u16string joined;
                if (SStr* s = left->value.asStrPtr()) joined.append(s->view());
                if (SStr* s = right->value.asStrPtr()) joined.append(s->view());
                // Reuse the JIT pin / relocatable AOT StrLit boundary.
                return constValue(Value::string(SStr::create(joined)));
            }
            if (right && !std::getenv("XYO_JIT_DISABLE_JOIN_SUFFIX")) {
                // Joining a live value to an immutable text literal needs
                // no generic dispatch for the common string/string case.
                // Keep the dynamic left reporter's evaluation unchanged.
                llvm::Value* dynamic = emitInputOf(blk, "STRING1");
                llvm::Value* suffix = constValue(right->value);
                return call2("xyo_join_suffix", i64t, dynamic, suffix);
            }
        }

#if defined(XYO_STRING_JOIN_SPECIALIZED)
        // Dynamically guard the string/string domain once in JIT code. The
        // typed helper shares the runtime implementation but skips its generic
        // value-tag dispatch; all other tags retain the original helper path.
        if (op == "operator_join") {
            llvm::Value* lhs = emitInputOf(blk, "STRING1");
            llvm::Value* rhs = emitInputOf(blk, "STRING2");
            auto isStringBits = [&](llvm::Value* bits) {
                llvm::Value* empty = b.CreateICmpEQ(bits, ci64(val::kEmptyStr));
                llvm::Value* heap = b.CreateICmpEQ(
                    b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48));
                return b.CreateOr(empty, heap);
            };
            llvm::Value* bothStrings = b.CreateAnd(
                isStringBits(lhs), isStringBits(rhs));
            BasicBlock* stringBB = BasicBlock::Create(lc, "join.str", fn);
            BasicBlock* genericBB = BasicBlock::Create(lc, "join.generic", fn);
            BasicBlock* mergeBB = BasicBlock::Create(lc, "join.merge", fn);
            b.CreateCondBr(bothStrings, stringBB, genericBB);
            b.SetInsertPoint(stringBB);
            llvm::Value* strings = call2("xyo_join_strings", i64t, lhs, rhs);
            b.CreateBr(mergeBB);
            b.SetInsertPoint(genericBB);
            llvm::Value* generic = call2("xyo_join", i64t, lhs, rhs);
            b.CreateBr(mergeBB);
            b.SetInsertPoint(mergeBB);
            llvm::PHINode* result = b.CreatePHI(i64t, 2);
            result->addIncoming(strings, stringBB);
            result->addIncoming(generic, genericBB);
            return result;
        }
#endif

        // --- 直接ヘルパ（レポータ）---
        if (op == "operator_letter_of") return emitLetterOf(blk);
        if (op == "operator_length") return emitLength(blk);
        if (op == "operator_contains") {
            // A literal ASCII haystack with a short alphabet can be lowered
            // into a compile-time character set (contiguous interval or
            // bitset), provided the needle is a flat one-unit ASCII string.
            // This works equally for digits, operators, vowels, etc.; never
            // match a project-specific literal or identify a benchmark.
            // Other values retain the full Scratch substring semantics.
            const BlockInput* input = blk->inputs.find("STRING1");
            const std::string literalId = input
                ? (input->block.empty() ? input->shadow : input->block) : "";
            Block* literal = literalId.empty() ? nullptr : getBlock(literalId);
            const BlockField* field = literal && literal->shadow &&
                literal->opcode == "text" ? findField(literal, "TEXT") : nullptr;
            uint64_t asciiLo = 0;
            uint64_t asciiHi = 0;
            bool asciiSet = false;
            uint16_t rangeMin = 127, rangeMax = 0;
            const char* setting = std::getenv("XYO_JIT_ASCII_SET_CONTAINS");
            if ((!setting || setting[0] != '0') && field &&
                field->value.isString() && field->value.asStrPtr()) {
                const auto chars = field->value.asStrPtr()->view();
                if (!chars.empty() && chars.size() <= 32) {
                    asciiSet = true;
                    for (char16_t c : chars) {
                        if (c >= 128) { asciiSet = false; break; }
                        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                        rangeMin = std::min<uint16_t>(rangeMin, c);
                        rangeMax = std::max<uint16_t>(rangeMax, c);
                        if (c < 64)
                            asciiLo |= uint64_t(1) << c;
                        else
                            asciiHi |= uint64_t(1) << (c - 64);
                    }
                }
            }
            if (asciiSet) {
                llvm::Value* hay = emitInputOf(blk, "STRING1");
                llvm::Value* needle = emitInputOf(blk, "STRING2");
                BasicBlock* taggedBB = newBB("ascii.set.tagged");
                BasicBlock* flatBB = newBB("ascii.set.flat");
                BasicBlock* asciiBB = newBB("ascii.set.ascii");
                BasicBlock* fastBB = newBB("ascii.set.fast");
                BasicBlock* slowBB = newBB("ascii.set.slow");
                BasicBlock* doneBB = newBB("ascii.set.done");
                llvm::Value* isStr = b.CreateICmpEQ(
                    b.CreateLShr(needle, ci64(48)),
                    ci64(val::kTagStr >> 48));
                b.CreateCondBr(isStr, taggedBB, slowBB);
                b.SetInsertPoint(taggedBB);
                llvm::Value* str = b.CreateIntToPtr(
                    b.CreateAnd(needle, ci64(val::kPtrMask)), ptrt);
                b.CreateCondBr(emitIsConsStr(str), slowBB, flatBB);
                b.SetInsertPoint(flatBB);
                llvm::Value* len = b.CreateLoad(i32t, fld(str, kSStrLenOff));
                b.CreateCondBr(b.CreateICmpEQ(len, ci32(1)), asciiBB, slowBB);
                b.SetInsertPoint(asciiBB);
                llvm::Value* code = b.CreateLoad(i16t, fld(str, kSStrDataOff));
                b.CreateCondBr(b.CreateICmpULT(code, b.getInt16(128)),
                               fastBB, slowBB);
                b.SetInsertPoint(fastBB);
                llvm::Value* uppercase = b.CreateAnd(
                    b.CreateICmpUGE(code, b.getInt16('A')),
                    b.CreateICmpULE(code, b.getInt16('Z')));
                llvm::Value* folded = b.CreateSelect(
                    uppercase,
                    b.CreateAdd(code, b.getInt16('a' - 'A')), code);
                llvm::Value* member;
                const unsigned numMembers =
                    std::popcount(asciiLo) + std::popcount(asciiHi);
                if (numMembers == rangeMax - rangeMin + 1) {
                    // General interval detection: digit classes and any
                    // other contiguous ASCII alphabet compile to two bounds.
                    member = b.CreateAnd(
                        b.CreateICmpUGE(folded, b.getInt16(rangeMin)),
                        b.CreateICmpULE(folded, b.getInt16(rangeMax)));
                } else {
                    // A 128-bit constant character class represented by
                    // two 64-bit masks. No loop or heap allocation at runtime.
                    llvm::Value* bit = b.CreateShl(
                        ci64(1), b.CreateAnd(b.CreateZExt(folded, i64t),
                                            ci64(63)));
                    llvm::Value* mask = b.CreateSelect(
                        b.CreateICmpULT(folded, b.getInt16(64)),
                        ci64(asciiLo), ci64(asciiHi));
                    member = b.CreateICmpNE(
                        b.CreateAnd(mask, bit), ci64(0));
                }
                llvm::Value* fastResult = boolBits(member);
                b.CreateBr(doneBB);
                b.SetInsertPoint(slowBB);
                llvm::Value* slowResult = call2("xyo_strcontains", i64t,
                                               hay, needle);
                b.CreateBr(doneBB);
                b.SetInsertPoint(doneBB);
                auto* result = b.CreatePHI(i64t, 2);
                result->addIncoming(fastResult, fastBB);
                result->addIncoming(slowResult, slowBB);
                return result;
            }
        }
        {
            auto it = directInputs().find(op);
            if (it != directInputs().end()) {
                SmallVector<llvm::Value*, 4> args;
                if (it->second.ctx) args.push_back(ctxArg);
                for (const char* n : it->second.inputs)
                    args.push_back(emitInputOf(blk, n));
                for (const char* n : it->second.fields)
                    args.push_back(fieldBits(blk, n));
                SmallVector<Type*, 4> ts;
                for (llvm::Value* a : args) ts.push_back(a->getType());
                noteCall(it->second.fn);
                return b.CreateCall(
                    decl(it->second.fn, i64t, ArrayRef<Type*>(ts)), args);
            }
        }

        // --- compat 層（インタプリタ評価）---
        countGuard.active = false;
        return call2("xyo_rt_reporter", i64t, ctxArg,
                     b.CreateGlobalString(id));
    }

    /// input 優先・無ければ field（sensing_setdragmode 等の両対応）
    llvm::Value* emitInputOrField(Block* blk, const char* name) {
        if (blk->inputs.has(name)) return emitInputOf(blk, name);
        return fieldBits(blk, name);
    }

    const BlockField* findField(Block* blk, const std::string& name) {
        auto it = blk->fields.find(name);
        return it == blk->fields.end() ? nullptr : &it->second;
    }

    /// フィールド値を i64 定数に（フィールド無しは undefined）
    llvm::Value* fieldBits(Block* blk, const char* name) {
        const BlockField* f = findField(blk, name);
        if (!f) return ci64(Value::undefined().bits);
        return constValue(f->value);
    }

    /// 直接ヘルパ呼出し可能なオペ（emit は inputs→fields の順で引数渡し）
    /// ctx=false は JitCtx* を取らないヘルパ（純粋な値→値関数）。
    struct DirectOp {
        const char* fn;
        std::vector<const char*> inputs;
        std::vector<const char*> fields;
        bool ctx = true;
    };

    static const std::map<std::string, DirectOp>& directInputs() {
        static const std::map<std::string, DirectOp> m = {
            {"operator_join",      {"xyo_join",      {"STRING1","STRING2"}, {}, false}},
            {"operator_letter_of", {"xyo_letterof",  {"LETTER","STRING"}, {}, false}},
            {"operator_length",    {"xyo_strlen",    {"STRING"}, {}, false}},
            {"operator_contains",  {"xyo_strcontains",{"STRING1","STRING2"}, {}, false}},
            {"control_get_counter",{"xyo_ctl_getcounter",{},{}}},
            {"motion_xposition",   {"xyo_mot_getx",  {},{}}},
            {"motion_yposition",   {"xyo_mot_gety",  {},{}}},
            {"motion_direction",   {"xyo_mot_getdir",{},{}}},
            {"looks_size",         {"xyo_looks_size",{},{}}},
            {"looks_costumenumbername",
             {"xyo_looks_costumenumname",{}, {"NUMBER_NAME"}}},
            {"looks_backdropnumbername",
             {"xyo_looks_backdropnumname",{}, {"NUMBER_NAME"}}},
            {"sensing_mousex",     {"xyo_sns_mousex",{},{}}},
            {"sensing_mousey",     {"xyo_sns_mousey",{},{}}},
            {"sensing_mousedown",  {"xyo_sns_mousedown",{},{}}},
            {"sensing_keypressed", {"xyo_sns_keypressed",{"KEY_OPTION"},{}}},
            {"sensing_touchingobject",
             {"xyo_sns_touchingobj",{"TOUCHINGOBJECTMENU"},{}}},
            {"sensing_touchingcolor",
             {"xyo_sns_touchingcolor",{"COLOR"},{}}},
            {"sensing_coloristouchingcolor",
             {"xyo_sns_coloristouching",{"COLOR2","COLOR"},{}}},
            {"sensing_distanceto", {"xyo_sns_distanceto",{"DISTANCETOMENU"},{}}},
            {"sensing_loudness",   {"xyo_sns_loudness",{},{}}},
            {"sensing_loud",       {"xyo_sns_loud",{},{}}},
            {"sensing_answer",     {"xyo_sns_answer",{},{}}},
            {"sensing_username",   {"xyo_sns_username",{},{}}},
            {"sensing_online",     {"xyo_sns_online",{},{}}},
            {"sensing_current",    {"xyo_sns_current",{},{"CURRENTMENU"}}},
            {"sensing_dayssince2000",{"xyo_sns_dayssince2000",{},{}}},
            {"sensing_of",         {"xyo_sns_of",{"OBJECT"},{"PROPERTY"}}},
            {"sound_volume",       {"xyo_snd_getvolume",{},{}}},
        };
        return m;
    }

    static const std::map<std::string, DirectOp>& directStack() {
        static const std::map<std::string, DirectOp> m = {
            {"motion_movesteps",   {"xyo_mot_movesteps",{"STEPS"},{}}},
            {"motion_goto",        {"xyo_mot_goto",{"TO"},{}}},
            {"motion_turnright",   {"xyo_mot_turnright",{"DEGREES"},{}}},
            {"motion_turnleft",    {"xyo_mot_turnleft",{"DEGREES"},{}}},
            {"motion_pointindirection",{"xyo_mot_pointdir",{"DIRECTION"},{}}},
            {"motion_pointtowards",{"xyo_mot_pointtowards",{"TOWARDS"},{}}},
            {"motion_glidesecstoxy",{"xyo_mot_glidexy",{"SECS","X","Y"},{}}},
            {"motion_glideto",     {"xyo_mot_glideto",{"SECS","TO"},{}}},
            {"motion_ifonedgebounce",{"xyo_mot_edgebounce",{},{}}},
            {"motion_setrotationstyle",{"xyo_mot_setrotstyle",{},{"STYLE"}}},
            {"looks_say",          {"xyo_looks_say",{"MESSAGE"},{}}},
            {"looks_think",        {"xyo_looks_think",{"MESSAGE"},{}}},
            {"looks_sayforsecs",   {"xyo_looks_saysecs",{"MESSAGE","SECS"},{}}},
            {"looks_thinkforsecs", {"xyo_looks_thinksecs",{"MESSAGE","SECS"},{}}},
            {"looks_show",         {"xyo_looks_show",{},{}}},
            {"looks_hide",         {"xyo_looks_hide",{},{}}},
            {"looks_switchcostumeto",{"xyo_looks_switchcostume",{"COSTUME"},{}}},
            {"looks_nextcostume",  {"xyo_looks_nextcostume",{},{}}},
            {"looks_switchbackdropto",
             {"xyo_looks_switchbackdrop",{"BACKDROP"},{}}},
            {"looks_nextbackdrop", {"xyo_looks_nextbackdrop",{},{}}},
            {"looks_switchbackdroptoandwait",
             {"xyo_looks_switchbackdropwait",{"BACKDROP"},{}}},
            {"looks_changeeffectby",
             {"xyo_looks_changeeffect",{"CHANGE"},{"EFFECT"}}},
            {"looks_seteffectto",  {"xyo_looks_seteffect",{"VALUE"},{"EFFECT"}}},
            {"looks_cleargraphiceffects",
             {"xyo_looks_cleareffects",{},{}}},
            {"looks_changesizeby", {"xyo_looks_changesize",{"CHANGE"},{}}},
            {"looks_setsizeto",    {"xyo_looks_setsize",{"SIZE"},{}}},
            {"looks_gotofrontback",{"xyo_looks_frontback",{},{"FRONT_BACK"}}},
            {"looks_goforwardbackwardlayers",
             {"xyo_looks_fwdbacklayers",{"NUM"},{"FORWARD_BACKWARD"}}},
            {"sensing_setdragmode",{"xyo_sns_setdragmode",{"DRAG_MODE"},{}}},
            {"sensing_resettimer", {"xyo_sns_resettimer",{},{}}},
            {"sensing_askandwait", {"xyo_sns_askwait",{"QUESTION"},{}}},
            {"control_incr_counter",{"xyo_ctl_incrcounter",{},{}}},
            {"control_clear_counter",{"xyo_ctl_clearcounter",{},{}}},
            {"control_create_clone_of",{"xyo_ctl_createclone",{"CLONE_OPTION"},{}}},
            {"control_delete_this_clone",{"xyo_ctl_deleteclone",{},{}}},
            {"sound_play",         {"xyo_snd_play",{"SOUND_MENU"},{}}},
            {"sound_playuntildone",{"xyo_snd_playwait",{"SOUND_MENU"},{}}},
            {"sound_stopallsounds",{"xyo_snd_stopall",{},{}}},
            {"sound_seteffectto",  {"xyo_snd_seteffect",{"VALUE"},{"EFFECT"}}},
            {"sound_changeeffectby",{"xyo_snd_changeeffect",{"VALUE"},{"EFFECT"}}},
            {"sound_cleareffects", {"xyo_snd_cleareffects",{},{}}},
            {"sound_setvolumeto",  {"xyo_snd_setvolume",{"VOLUME"},{}}},
            {"sound_changevolumeby",{"xyo_snd_changevolume",{"VOLUME"},{}}},
            // 実害のない no-op（インタプリタも noop 登録）
            {"looks_hideallsprites",{nullptr,{},{}}},
            {"looks_changestretchby",{nullptr,{},{}}},
            {"looks_setstretchto", {nullptr,{},{}}},
            {"motion_scroll_right",{nullptr,{},{}}},
            {"motion_scroll_up",   {nullptr,{},{}}},
            {"motion_align_scene", {nullptr,{},{}}},
            {"motion_xscroll",     {nullptr,{},{}}},
            {"motion_yscroll",     {nullptr,{},{}}},
        };
        return m;
    }

    /// DirectOp を emit。入力を先に評価（JS の入力評価順）。
    /// fn==nullptr は no-op。
    void emitDirect(Block* blk, const DirectOp& op) {
        if (!op.fn) return;
        SmallVector<llvm::Value*, 6> args;
        args.push_back(ctxArg);
        for (const char* n : op.inputs) args.push_back(emitInputOrField(blk, n));
        for (const char* n : op.fields) args.push_back(fieldBits(blk, n));
        SmallVector<Type*, 6> ts;
        for (llvm::Value* a : args) ts.push_back(a->getType());
        noteCall(op.fn);
        b.CreateCall(decl(op.fn, voidt, ArrayRef<Type*>(ts)), args);
    }

    // General bounded partial unrolling for literal-count Scratch repeats.
    // The source body is arbitrary straight-line stack code from this safe set,
    // not a recognized benchmark or string-concatenation idiom. We retain a
    // yield checkpoint after *every* logical iteration, including inside each
    // unrolled group. Nested control flow and opaque/compat instructions use
    // the ordinary counted loop instead.
    struct RepeatUnrollPlan {
        uint64_t trips = 0;
        unsigned factor = 0;
        std::vector<Block*> body;
    };
    unsigned unrollCodeBudget = 2048;

    std::optional<RepeatUnrollPlan> repeatUnrollPlan(Block* loop) {
        const char* enabled = std::getenv("XYO_REPEAT_UNROLL");
        if (!enabled || enabled[0] != '1') return std::nullopt;
        const BlockInput* times = loop->inputs.find("TIMES");
        const BlockInput* body = loop->inputs.find("SUBSTACK");
        if (!times || !body || body->block.empty()) return std::nullopt;
        const std::string& source =
            times->block.empty() ? times->shadow : times->block;
        Block* literal = getBlock(source);
        if (!literal || !literal->shadow || literal->fields.empty() ||
            literal->opcode.rfind("math_", 0) != 0)
            return std::nullopt;
        const double n = cast::toNumber(literal->fields.begin()->second.value);
        // Large values may stop decrementing by 1 in double precision.
        if (!std::isfinite(n) || n < 2 || n > 1000000 ||
            std::trunc(n) != n)
            return std::nullopt;

        std::vector<Block*> chain = chainOf(body->block);
        if (chain.empty() || chain.size() > 8) return std::nullopt;
        static const std::set<std::string> simple = {
            "data_setvariableto", "data_changevariableby",
            "data_addtolist", "data_deleteoflist", "data_deletealloflist",
            "data_insertatlist", "data_replaceitemoflist",
            "motion_movesteps", "motion_changexby", "motion_changeyby",
            "motion_setx", "motion_sety", "motion_turnright",
            "motion_turnleft", "looks_changesizeby", "looks_setsizeto",
            "looks_say", "looks_think"
        };
        for (Block* stmt : chain) {
            if (!simple.count(stmt->opcode) ||
                stmt->inputs.has("SUBSTACK") ||
                stmt->inputs.has("SUBSTACK2"))
                return std::nullopt;
        }
        unsigned limit = std::min<unsigned>(
            512u / unsigned(chain.size()),
            unrollCodeBudget / unsigned(chain.size()));
        if (limit < 2) return std::nullopt;
        unsigned factor = std::min(limit, unsigned(n));
        return RepeatUnrollPlan{uint64_t(n), factor, std::move(chain)};
    }

    void emitUnrolledRepeat(Block* loop, const RepeatUnrollPlan& plan) {
        unrollCodeBudget -= plan.factor * unsigned(plan.body.size());
        const uint64_t groups = plan.trips / plan.factor;
        const uint64_t tail = plan.trips % plan.factor;
        beginLoopYieldScope(loop);
        const bool promotedLoop = beginLoopPromotion(loop, false);
        // Repeated body evaluation uses the ordinary stack lowerer, so all
        // variable type, reporter-order, and spill/reload rules are shared.
        auto emitOne = [&]() {
            emitChainBlocks(plan.body);
            if (!bbDone(b.GetInsertBlock())) emitLoopYield();
        };
        if (groups == 1) {
            for (unsigned i = 0; i < plan.factor && !fail; ++i)
                emitOne();
        } else {
            AllocaInst* remaining = entryAlloca(i64t, 1, "rep.unroll.groups");
            b.CreateStore(ci64(groups), remaining);
            BasicBlock* head = newBB("rep.unroll.head");
            BasicBlock* body = newBB("rep.unroll.body");
            BasicBlock* done = newBB("rep.unroll.done");
            b.CreateBr(head);
            b.SetInsertPoint(head);
            llvm::Value* count = b.CreateLoad(i64t, remaining);
            b.CreateCondBr(b.CreateICmpNE(count, ci64(0)), body, done);
            b.SetInsertPoint(body);
            for (unsigned i = 0; i < plan.factor && !fail; ++i)
                emitOne();
            if (!bbDone(b.GetInsertBlock())) {
                b.CreateStore(b.CreateSub(
                    b.CreateLoad(i64t, remaining), ci64(1)), remaining);
                b.CreateBr(head);
            }
            b.SetInsertPoint(done);
        }
        for (uint64_t i = 0; i < tail && !fail; ++i)
            emitOne();
        endLoopYieldScope(true);
        endLoopPromotion(promotedLoop);
    }

    // --- スタック emit ------------------------------------------------------

    void emitStopped() {
        // stopScript で suspend しなかった場合の安全弁
        b.CreateRetVoid();
    }

    int nestedBranchDepth = 0;

    void emitChainBlocks(const std::vector<Block*>& chain, bool branch = false) {
        if (branch) ++nestedBranchDepth;
        for (size_t i = 0; i < chain.size(); ++i) {
            Block* blk = chain[i];
            if (fail) break;
            // 現在位置が既に終端済み（stop 等）なら残りは到達不能 — 打ち切る
            if (bbDone(b.GetInsertBlock())) break;
            if (!hoistedSetters.empty() && hoistedSetters.count(blk))
                continue;   // ループ外へホイスト済み
            emitStackBlock(blk);
        }
        if (branch) --nestedBranchDepth;
    }

    // Scratch-specific escape proof for repeat-loop transient strings.
    // Before every possible handoff, the pending prefix is materialized as
    // an immutable GC-rooted snapshot. Only pure local variable mutations
    // are allowed among the appends; side effects/observers are rejected.
    struct TransientPiece {
        std::u16string text;
        Block* dynamic = nullptr;
    };
    struct TransientAction {
        Block* statement = nullptr;  // null only for an impossible plan
        bool isAppend = false;
        std::vector<TransientPiece> pieces; // RHS join flattened into leaves
    };
    struct TransientRepeat {
        const BlockField* variable = nullptr;
        std::vector<TransientAction> actions;
        std::optional<std::u16string> batchText;
    };

    Block* transientChild(Block* block, const char* name) {
        if (!block) return nullptr;
        const BlockInput* input = block->inputs.find(name);
        return input
            ? getBlock(input->block.empty() ? input->shadow : input->block)
            : nullptr;
    }

    // Prove that evaluating a reporter neither writes Scratch-visible state
    // nor reads the transient destination's unpublished value. Do NOT add
    // sensing, random, procedures, lists or custom extensions here.
    bool safeTransientReporter(Block* rhs, const std::string& destination,
                               unsigned depth = 0) {
        if (!rhs || depth > 12) return false;
        const auto& op = rhs->opcode;
        if (rhs->shadow) {
            if (op == "text") {
                const auto* f = findField(rhs, "TEXT");
                return f && f->value.isString();
            }
            return op == "math_number" || op == "math_integer" ||
                   op == "math_whole_number" || op == "math_positive_number" ||
                   op == "math_angle";
        }
        if (op == "data_variable") {
            const auto* field = findField(rhs, "VARIABLE");
            return field && !field->id.empty() && field->id != destination;
        }
        if (op == "argument_reporter_string_number" ||
            op == "argument_reporter_boolean")
            return true; // immutable parameters of the containing procedure
        if (op == "operator_add" || op == "operator_subtract" ||
            op == "operator_multiply" || op == "operator_divide" ||
            op == "operator_mod") {
            return safeTransientReporter(
                       transientChild(rhs, "NUM1"), destination, depth + 1) &&
                   safeTransientReporter(
                       transientChild(rhs, "NUM2"), destination, depth + 1);
        }
        if (op == "operator_join") {
            return safeTransientReporter(
                       transientChild(rhs, "STRING1"), destination, depth + 1) &&
                   safeTransientReporter(
                       transientChild(rhs, "STRING2"), destination, depth + 1);
        }
        if (op == "operator_lt" || op == "operator_gt" ||
            op == "operator_equals" || op == "operator_and" ||
            op == "operator_or") {
            return safeTransientReporter(
                       transientChild(rhs, "OPERAND1"), destination, depth + 1) &&
                   safeTransientReporter(
                       transientChild(rhs, "OPERAND2"), destination, depth + 1);
        }
        if (op == "operator_not")
            return safeTransientReporter(
                transientChild(rhs, "OPERAND"), destination, depth + 1);
        if (op == "operator_round")
            return safeTransientReporter(
                transientChild(rhs, "NUM"), destination, depth + 1);
        if (op == "operator_length")
            return safeTransientReporter(
                transientChild(rhs, "STRING"), destination, depth + 1);
        if (op == "operator_letter_of")
            return safeTransientReporter(
                       transientChild(rhs, "LETTER"), destination, depth + 1) &&
                   safeTransientReporter(
                       transientChild(rhs, "STRING"), destination, depth + 1);
        return false;
    }

    // Recursively decompose RHS joins. This removes intermediate SStr
    // allocations for join(s, join("prefix", v)) and preserves evaluation
    // order, numeric casts, UTF-16 indexing and empty-string handling.
    bool collectTransientPieces(Block* rhs, const std::string& destination,
                                std::vector<TransientPiece>& out,
                                unsigned depth = 0) {
        if (!rhs || depth > 12 || out.size() > 16) return false;
        if (rhs->opcode == "operator_join" && !rhs->shadow) {
            return collectTransientPieces(
                       transientChild(rhs, "STRING1"), destination, out,
                       depth + 1) &&
                   collectTransientPieces(
                       transientChild(rhs, "STRING2"), destination, out,
                       depth + 1);
        }
        if (rhs->opcode == "text" && rhs->shadow) {
            const BlockField* text = findField(rhs, "TEXT");
            if (!text || !text->value.isString()) return false;
            const SStr* str = text->value.asStrPtr();
            if (str && str->len)
                out.push_back({std::u16string(str->view()), nullptr});
            return true;
        }
        // Preformat constant numeric additions: no formatter on the loop
        // hot path, even when the expression is written as two shadows.
        if (rhs->opcode == "operator_add" && !rhs->shadow) {
            auto numShadow = [&](const char* input) -> std::optional<double> {
                Block* child = transientChild(rhs, input);
                if (!child || !child->shadow) return std::nullopt;
                if (child->opcode != "math_number" &&
                    child->opcode != "math_integer" &&
                    child->opcode != "math_whole_number" &&
                    child->opcode != "math_positive_number")
                    return std::nullopt;
                const BlockField* field = findField(child, "NUM");
                return field ? std::optional(cast::toNumber(field->value))
                             : std::nullopt;
            };
            auto a = numShadow("NUM1");
            auto b = numShadow("NUM2");
            if (a && b) {
                out.push_back({jsnum::toString16(*a + *b), nullptr});
                return true;
            }
        }
        if (!safeTransientReporter(rhs, destination, depth))
            return false;
        out.push_back({{}, rhs});
        return true;
    }

    std::optional<TransientRepeat> transientRepeatPattern(Block* loop) {
        // Strict flat storage must not be bypassed by the transient
        // append-log loop optimization. Keep the existing rope/default
        // optimization unchanged for normal production workloads.
        if (aot || !curFnWarp || activeTransient ||
            stringStorageMode() == StringStorageMode::Flat ||
            (std::getenv("XYO_JIT_TRANSIENT_STRINGS") &&
             std::getenv("XYO_JIT_TRANSIENT_STRINGS")[0] == '0'))
            return std::nullopt;
        const BlockInput* bodyIn = loop->inputs.find("SUBSTACK");
        if (!bodyIn || bodyIn->block.empty()) return std::nullopt;
        auto body = chainOf(bodyIn->block);
        if (body.empty() || body.size() > 16 || !body.back()->next.empty())
            return std::nullopt;

        TransientRepeat plan;
        size_t appendCount = 0;
        for (Block* statement : body) {
            const std::string& op = statement->opcode;
            if (op != "data_setvariableto" &&
                op != "data_changevariableby")
                return std::nullopt;
            const BlockField* field = findField(statement, "VARIABLE");
            if (!field || field->id.empty()) return std::nullopt;

            const BlockInput* valueIn = statement->inputs.find("VALUE");
            Block* value = valueIn ? transientChild(statement, "VALUE") : nullptr;
            if (!value) return std::nullopt;
            if (op == "data_setvariableto" &&
                value->opcode == "operator_join" && !value->shadow) {
                Block* lhs = transientChild(value, "STRING1");
                Block* rhs = transientChild(value, "STRING2");
                const BlockField* read = lhs && lhs->opcode == "data_variable"
                    ? findField(lhs, "VARIABLE") : nullptr;
                if (read && read->id == field->id) {
                    if (!plan.variable)
                        plan.variable = field;
                    if (plan.variable->id != field->id)
                        return std::nullopt;  // distinct transient variables
                    TransientAction action;
                    action.statement = statement;
                    action.isAppend = true;
                    if (!collectTransientPieces(rhs, field->id, action.pieces))
                        return std::nullopt;
                    if (action.pieces.size() > 16) return std::nullopt;
                    plan.actions.push_back(std::move(action));
                    ++appendCount;
                    continue;
                }
            }
            // A regular variable assignment can read/update itself (e.g.
            // change i by 1). Once the transient destination is identified
            // below, prove that its expression never reads that destination.
            plan.actions.push_back({statement, false, {}});
        }
        if (!plan.variable || !appendCount ||
            (findPromoted(plan.variable) && findPromoted(plan.variable)->numeric))
            return std::nullopt;

        for (const auto& action : plan.actions) {
            if (action.isAppend) continue;
            const BlockField* field = findField(action.statement, "VARIABLE");
            if (!field || field->id == plan.variable->id ||
                !safeTransientReporter(
                    transientChild(action.statement, "VALUE"),
                    plan.variable->id))
                return std::nullopt;
        }

        // Pure literal append chains can be collapsed into a single constant
        // repeated for every logical iteration. Even multiple set-variable
        // blocks can be combined because none is observable until a yield.
        std::u16string combined;
        bool allLiteral = true;
        for (const auto& action : plan.actions) {
            if (!action.isAppend) { allLiteral = false; break; }
            for (const auto& piece : action.pieces) {
                if (piece.dynamic || combined.size() + piece.text.size() > 4096) {
                    allLiteral = false;
                    break;
                }
                combined += piece.text;
            }
            if (!allLiteral) break;
        }
        if (allLiteral && !combined.empty())
            plan.batchText = std::move(combined);
        return plan;
    }

    struct TransientEmittedPiece {
        Block* dynamic = nullptr;
        llvm::Value* literal = nullptr;
        uint64_t packed = 0;
        uint32_t packedLength = 0;
    };
    TransientEmittedPiece makeTransientPiece(const TransientPiece& piece,
                                            bool allowPacked) {
        TransientEmittedPiece result;
        result.dynamic = piece.dynamic;
        if (piece.dynamic || piece.text.empty()) return result;
        const bool packEnabled =
            !std::getenv("XYO_JIT_TRANSIENT_PACKED") ||
            std::getenv("XYO_JIT_TRANSIENT_PACKED")[0] != '0';
        if (allowPacked && packEnabled && piece.text.size() <= 4) {
            result.packedLength = uint32_t(piece.text.size());
            for (size_t i = 0; i < piece.text.size(); ++i)
                result.packed |= uint64_t(piece.text[i]) << (16 * i);
        } else {
            result.literal = constValue(
                Value::string(SStr::create(piece.text)));
        }
        return result;
    }

    void emitTransientPiece(llvm::Value* state,
                            const TransientEmittedPiece& piece) {
        if (piece.dynamic) {
            llvm::Value* value = emitInputBlock(piece.dynamic->id);
            call2("xyo_transient_append_value", voidt, state, value);
        } else if (piece.packedLength) {
            call3("xyo_transient_append_packed", voidt, state,
                  ci64(piece.packed), ci32(piece.packedLength));
        } else if (piece.literal) {
            call2("xyo_transient_append", voidt, state, piece.literal);
        }
    }

    void emitTransientRepeat(Block* loop, TransientRepeat plan) {
        if (verbose) std::fprintf(stderr,
            "xyo-jit: transient MATCH %s actions=%zu aggregate=%zu\n",
            loop->id.c_str(), plan.actions.size(),
            plan.batchText ? plan.batchText->size() : 0);
        const bool counted = loop->opcode == "control_repeat";
        const bool foreach = loop->opcode == "control_for_each";
        const bool until = loop->opcode == "control_repeat_until";
        llvm::Value* count = counted
            ? emitToNumF64(emitInputOf(loop, "TIMES")) : nullptr;
        beginLoopYieldScope(loop);
        AllocaInst* remaining = counted
            ? entryAlloca(f64t, 1, "str.rep.left") : nullptr;
        if (counted) b.CreateStore(count, remaining);
        AllocaInst* index = foreach
            ? entryAlloca(f64t, 1, "str.foreach.index") : nullptr;
        const BlockField* indexVar = foreach
            ? findField(loop, "VARIABLE") : nullptr;
        if (foreach) b.CreateStore(cf64(0.0), index);
        AllocaInst* currentLog = entryAlloca(ptrt, 1, "str.transient");
        b.CreateStore(cnull(), currentLog);
        llvm::Value* varPtr = varValuePtr(plan.variable);

        // If the entire body is one pure reporter append, its RHS is
        // invariant between scheduling checkpoints and can also be batched.
        // The reporter must be evaluated anew after each possible yield.
        const bool dynamicBatch = counted && !plan.batchText &&
            plan.actions.size() == 1 && plan.actions[0].isAppend &&
            plan.actions[0].pieces.size() == 1 &&
            plan.actions[0].pieces[0].dynamic != nullptr;
        const bool batching =
            counted && (plan.batchText || dynamicBatch) &&
            (!std::getenv("XYO_JIT_TRANSIENT_BATCH") ||
             std::getenv("XYO_JIT_TRANSIENT_BATCH")[0] != '0');
        TransientEmittedPiece batchPiece;
        if (batching) {
            batchPiece = dynamicBatch
                ? makeTransientPiece(plan.actions[0].pieces[0], true)
                : makeTransientPiece(
                    TransientPiece{*plan.batchText, nullptr}, true);
        }
        std::vector<std::vector<TransientEmittedPiece>> emitted;
        if (!batching) {
            emitted.reserve(plan.actions.size());
            for (const auto& action : plan.actions) {
                std::vector<TransientEmittedPiece> pieces;
                for (const auto& piece : action.pieces)
                    pieces.push_back(makeTransientPiece(piece, true));
                emitted.push_back(std::move(pieces));
            }
        }

        BasicBlock* head = newBB("str.rep.head");
        BasicBlock* body = newBB("str.rep.body");
        BasicBlock* start = newBB("str.rep.start");
        BasicBlock* continuing = newBB("str.rep.cont");
        BasicBlock* push = newBB("str.rep.push");
        BasicBlock* done = newBB("str.rep.done");
        BasicBlock* publish = newBB("str.rep.publish");
        BasicBlock* leave = newBB("str.rep.leave");
        b.CreateBr(head);
        b.SetInsertPoint(head);
        llvm::Value* i = counted
            ? b.CreateLoad(f64t, remaining) : nullptr;
        llvm::Value* indexValue = foreach
            ? b.CreateLoad(f64t, index) : nullptr;
        llvm::Value* go;
        if (counted) {
            go = b.CreateFCmpOGE(i, cf64(0.5));
        } else if (foreach) {
            llvm::Value* bound =
                emitToNumF64(emitInputOf(loop, "VALUE"));
            go = b.CreateFCmpOLT(indexValue, bound);
        } else {
            llvm::Value* cond =
                emitTruthyV(emitInputOf(loop, "CONDITION"));
            go = until ? b.CreateNot(cond) : cond;
        }
        b.CreateCondBr(go, body, done);
        b.SetInsertPoint(body);
        if (foreach) {
            llvm::Value* nextIndex = b.CreateFAdd(indexValue, cf64(1.0));
            emitVarSet(indexVar, emitCanon(nextIndex));
        }
        llvm::Value* existing = b.CreateLoad(ptrt, currentLog);
        b.CreateCondBr(b.CreateIsNull(existing), start, continuing);
        b.SetInsertPoint(start);
        llvm::Value* initial = b.CreateLoad(i64t, varPtr);
        llvm::Value* fresh = call2("xyo_transient_begin", ptrt, initial, varPtr);
        b.CreateStore(fresh, currentLog);
        b.CreateBr(push);
        b.SetInsertPoint(continuing);
        b.CreateBr(push);
        b.SetInsertPoint(push);
        PHINode* state = b.CreatePHI(ptrt, 2);
        state->addIncoming(fresh, start);
        state->addIncoming(existing, continuing);

        llvm::Value* logicalCount = ci64(1);
        if (batching) {
            llvm::Value* poll = b.CreateLoad(i64t, curYieldCnt);
            llvm::Value* toCheckpoint =
                b.CreateSub(ci64(64), b.CreateAnd(poll, ci64(63)));
            llvm::Value* enough = b.CreateFCmpOGE(
                i, b.CreateFSub(b.CreateUIToFP(toCheckpoint, f64t), cf64(0.5)));
            llvm::Value* safe = b.CreateAnd(
                b.CreateFCmpOLE(i, cf64(4503599627370496.0)),
                b.CreateICmpNE(curYieldWarp, ci32(0)));
            logicalCount = b.CreateSelect(
                b.CreateAnd(enough, safe), toCheckpoint, ci64(1));
            if (batchPiece.dynamic) {
                llvm::Value* rhs = emitInputBlock(batchPiece.dynamic->id);
                call3("xyo_transient_append_repeat_value", voidt,
                      state, rhs, b.CreateTrunc(logicalCount, i32t));
            } else if (batchPiece.packedLength) {
                llvm::Value* meta = b.CreateOr(
                    ci64(uint64_t(batchPiece.packedLength) << 32),
                    logicalCount);
                call3("xyo_transient_append_repeat_packed", voidt,
                      state, ci64(batchPiece.packed), meta);
            } else {
                call3("xyo_transient_append_repeat_string", voidt,
                      state, batchPiece.literal,
                      b.CreateTrunc(logicalCount, i32t));
            }
            b.CreateStore(
                b.CreateAdd(poll, b.CreateSub(logicalCount, ci64(1))),
                curYieldCnt);
        } else {
            // Preserve the exact Scratch order of every independent variable
            // update and append. Only the destination's repeated SStr node
            // allocations are elided.
            for (size_t step = 0; step < plan.actions.size(); ++step) {
                const auto& action = plan.actions[step];
                if (action.isAppend) {
                    for (const auto& piece : emitted[step])
                        emitTransientPiece(state, piece);
                } else {
                    emitStackBlock(action.statement);
                }
            }
        }

        activeTransient = currentLog;
        activeTransientVariable = varPtr;
        emitLoopYield();
        activeTransient = nullptr;
        activeTransientVariable = nullptr;
        if (counted) {
            llvm::Value* next = b.CreateLoad(f64t, remaining);
            b.CreateStore(b.CreateFSub(
                next, b.CreateUIToFP(logicalCount, f64t)), remaining);
        } else if (foreach) {
            b.CreateStore(b.CreateFAdd(
                b.CreateLoad(f64t, index), cf64(1.0)), index);
        }
        b.CreateBr(head);

        b.SetInsertPoint(done);
        llvm::Value* last = b.CreateLoad(ptrt, currentLog);
        b.CreateCondBr(b.CreateIsNull(last), leave, publish);
        b.SetInsertPoint(publish);
        call2("xyo_transient_finish", voidt, last, varPtr);
        b.CreateBr(leave);
        b.SetInsertPoint(leave);
        endLoopYieldScope(true);
    }

    // Multiple independent destination strings can be batched together when
    // every instruction in the repeat body is a constant self-append. There
    // are no intermediate observers, so each variable can have its own
    // immutable prefix snapshot and its own copy-on-write log.
    struct MultiTransientVar {
        const BlockField* field = nullptr;
        std::u16string text;
    };
    std::optional<std::vector<MultiTransientVar>>
    multiTransientPattern(Block* loop) {
        // Strict flat storage must not be bypassed by the transient
        // append-log loop optimization. Keep the existing rope/default
        // optimization unchanged for normal production workloads.
        if (aot || !curFnWarp || activeTransient ||
            stringStorageMode() == StringStorageMode::Flat ||
            (std::getenv("XYO_JIT_TRANSIENT_STRINGS") &&
             std::getenv("XYO_JIT_TRANSIENT_STRINGS")[0] == '0') ||
            (std::getenv("XYO_JIT_TRANSIENT_MULTI") &&
             std::getenv("XYO_JIT_TRANSIENT_MULTI")[0] == '0'))
            return std::nullopt;
        const BlockInput* substack = loop->inputs.find("SUBSTACK");
        if (!substack || substack->block.empty()) return std::nullopt;
        auto body = chainOf(substack->block);
        if (body.size() < 2 || body.size() > 16 || !body.back()->next.empty())
            return std::nullopt;
        std::vector<MultiTransientVar> vars;
        for (Block* statement : body) {
            if (statement->opcode != "data_setvariableto") return std::nullopt;
            const BlockField* field = findField(statement, "VARIABLE");
            Block* value = transientChild(statement, "VALUE");
            if (!field || field->id.empty() || !value ||
                value->opcode != "operator_join") return std::nullopt;
            Block* left = transientChild(value, "STRING1");
            Block* right = transientChild(value, "STRING2");
            const BlockField* source =
                left && left->opcode == "data_variable"
                    ? findField(left, "VARIABLE") : nullptr;
            if (!source || source->id != field->id) return std::nullopt;
            std::vector<TransientPiece> pieces;
            if (!collectTransientPieces(right, field->id, pieces))
                return std::nullopt;
            size_t index = 0;
            while (index < vars.size() && vars[index].field->id != field->id)
                ++index;
            if (index == vars.size()) {
                if (vars.size() == 4) return std::nullopt;
                for (const auto& other : vars) {
                    // Conservative fallback for malformed SB3s with distinct
                    // variable IDs but identical fallback names.
                    if (other.field->value.isString() && field->value.isString() &&
                        other.field->value.asStrPtr() &&
                        field->value.asStrPtr() &&
                        other.field->value.asStrPtr()->view() ==
                            field->value.asStrPtr()->view())
                        return std::nullopt;
                }
                vars.push_back({field, {}});
            }
            for (const auto& piece : pieces) {
                if (piece.dynamic ||
                    vars[index].text.size() + piece.text.size() > 4096)
                    return std::nullopt;
                vars[index].text += piece.text;
            }
        }
        if (vars.size() < 2) return std::nullopt;
        for (const auto& var : vars) {
            if (var.text.empty()) return std::nullopt;
            if (auto* promoted = findPromoted(var.field);
                promoted && promoted->numeric)
                return std::nullopt;
        }
        return vars;
    }

    void emitTransientMultiRepeat(
        Block* loop, const std::vector<MultiTransientVar>& vars) {
        if (verbose)
            std::fprintf(stderr, "xyo-jit: transient MULTI %s vars=%zu\n",
                         loop->id.c_str(), vars.size());
        llvm::Value* count = emitToNumF64(emitInputOf(loop, "TIMES"));
        beginLoopYieldScope(loop);
        AllocaInst* remaining = entryAlloca(f64t, 1, "str.multi.left");
        b.CreateStore(count, remaining);
        std::vector<AllocaInst*> logs;
        std::vector<llvm::Value*> variablePointers;
        std::vector<TransientEmittedPiece> data;
        logs.reserve(vars.size());
        variablePointers.reserve(vars.size());
        data.reserve(vars.size());
        for (const auto& var : vars) {
            AllocaInst* state = entryAlloca(ptrt, 1, "str.multi.log");
            b.CreateStore(cnull(), state);
            logs.push_back(state);
            variablePointers.push_back(varValuePtr(var.field));
            data.push_back(makeTransientPiece(
                TransientPiece{var.text, nullptr}, true));
        }
        const bool batching =
            !std::getenv("XYO_JIT_TRANSIENT_BATCH") ||
            std::getenv("XYO_JIT_TRANSIENT_BATCH")[0] != '0';

        BasicBlock* head = newBB("str.multi.head");
        BasicBlock* body = newBB("str.multi.body");
        BasicBlock* done = newBB("str.multi.done");
        b.CreateBr(head);
        b.SetInsertPoint(head);
        llvm::Value* n = b.CreateLoad(f64t, remaining);
        b.CreateCondBr(b.CreateFCmpOGE(n, cf64(0.5)), body, done);
        b.SetInsertPoint(body);

        llvm::Value* logicalCount = ci64(1);
        if (batching) {
            llvm::Value* poll = b.CreateLoad(i64t, curYieldCnt);
            llvm::Value* untilPoll =
                b.CreateSub(ci64(64), b.CreateAnd(poll, ci64(63)));
            llvm::Value* fits = b.CreateFCmpOGE(
                n, b.CreateFSub(
                    b.CreateUIToFP(untilPoll, f64t), cf64(0.5)));
            llvm::Value* safe = b.CreateAnd(
                b.CreateFCmpOLE(n, cf64(4503599627370496.0)),
                b.CreateICmpNE(curYieldWarp, ci32(0)));
            logicalCount = b.CreateSelect(
                b.CreateAnd(fits, safe), untilPoll, ci64(1));
            b.CreateStore(
                b.CreateAdd(poll, b.CreateSub(logicalCount, ci64(1))),
                curYieldCnt);
        }

        for (size_t i = 0; i < vars.size(); ++i) {
            llvm::Value* initial = b.CreateLoad(ptrt, logs[i]);
            BasicBlock* create = newBB("str.multi.start");
            BasicBlock* reuse = newBB("str.multi.reuse");
            BasicBlock* ready = newBB("str.multi.ready");
            b.CreateCondBr(b.CreateIsNull(initial), create, reuse);
            b.SetInsertPoint(create);
            llvm::Value* bits = b.CreateLoad(i64t, variablePointers[i]);
            llvm::Value* log = call2("xyo_transient_begin", ptrt, bits,
                                      variablePointers[i]);
            b.CreateStore(log, logs[i]);
            b.CreateBr(ready);
            b.SetInsertPoint(reuse);
            b.CreateBr(ready);
            b.SetInsertPoint(ready);
            PHINode* state = b.CreatePHI(ptrt, 2);
            state->addIncoming(log, create);
            state->addIncoming(initial, reuse);
            if (batching) {
                if (data[i].packedLength) {
                    llvm::Value* meta = b.CreateOr(
                        ci64(uint64_t(data[i].packedLength) << 32),
                        logicalCount);
                    call3("xyo_transient_append_repeat_packed", voidt,
                          state, ci64(data[i].packed), meta);
                } else {
                    call3("xyo_transient_append_repeat_string", voidt,
                          state, data[i].literal,
                          b.CreateTrunc(logicalCount, i32t));
                }
            } else {
                emitTransientPiece(state, data[i]);
            }
        }

        activeTransient = logs[0];
        activeTransientVariable = variablePointers[0];
        for (size_t i = 1; i < vars.size(); ++i)
            activeExtraTransients.push_back({logs[i], variablePointers[i]});
        emitLoopYield();
        activeExtraTransients.clear();
        activeTransient = nullptr;
        activeTransientVariable = nullptr;
        llvm::Value* old = b.CreateLoad(f64t, remaining);
        b.CreateStore(b.CreateFSub(
            old, b.CreateUIToFP(logicalCount, f64t)), remaining);
        b.CreateBr(head);

        b.SetInsertPoint(done);
        for (size_t i = 0; i < vars.size(); ++i) {
            BasicBlock* publish = newBB("str.multi.finish");
            BasicBlock* next = newBB("str.multi.next");
            llvm::Value* log = b.CreateLoad(ptrt, logs[i]);
            b.CreateCondBr(b.CreateIsNull(log), next, publish);
            b.SetInsertPoint(publish);
            call2("xyo_transient_finish", voidt, log,
                  variablePointers[i]);
            b.CreateBr(next);
            b.SetInsertPoint(next);
        }
        endLoopYieldScope(true);
    }

    // Aggregate affine, effect-free scalar updates up to the next warp
    // deadline sample. Never change the behavior of non-warp loops or of
    // fractional, nonnumeric, potentially inexact or unproven inputs.
    struct WarpInductionUpdate {
        const BlockField* field;
        int64_t delta;
    };
    struct WarpInductionPlan {
        const BlockField* index = nullptr;
        const BlockField* boundVariable = nullptr;
        int64_t boundLiteral = 0;
        int64_t stride = 0;
        int direction = 0;
        bool inclusive = false;
        std::vector<WarpInductionUpdate> updates;
    };

    // Literal number shadows only. Expressions and strings can have different
    // Scratch coercion or evaluation semantics and cannot be hoisted blindly.
    std::optional<double> literalNumber(Block* parent, const char* input) {
        const BlockInput* in = parent ? parent->inputs.find(input) : nullptr;
        if (!in) return std::nullopt;
        const auto& id = in->block.empty() ? in->shadow : in->block;
        Block* value = getBlock(id);
        if (!value || !value->shadow || value->fields.size() != 1)
            return std::nullopt;
        static const std::unordered_set<std::string_view> kNumeric = {
            "math_number", "math_integer", "math_whole_number",
            "math_positive_number", "math_angle"
        };
        if (!kNumeric.contains(value->opcode)) return std::nullopt;
        const Value literal = value->fields.begin()->second.value;
        if (cast::isWhiteSpace(literal)) return std::nullopt;
        const double number = cast::toNumberOrNaN(literal);
        if (!std::isfinite(number)) return std::nullopt;
        return number;
    }

    const BlockField* directVariableInput(Block* node, const char* input) {
        const BlockInput* in = node ? node->inputs.find(input) : nullptr;
        if (!in) return nullptr;
        Block* child = getBlock(in->block.empty() ? in->shadow : in->block);
        return child && child->opcode == "data_variable"
            ? findField(child, "VARIABLE") : nullptr;
    }

    // Resolve-by-name fallback can alias two different Scratch IDs. Rather
    // than assuming they are distinct, reject ambiguous equal-name fields.
    static bool sameNamedField(const BlockField* a, const BlockField* c) {
        if (!a || !c || !a->value.isString() || !c->value.isString() ||
            !a->value.asStrPtr() || !c->value.asStrPtr()) return false;
        return a->value.asStrPtr()->view() == c->value.asStrPtr()->view();
    }

    std::optional<WarpInductionPlan> warpInductionPlan(Block* loop) {
        if (promotingLoop || !promoted.empty()) return std::nullopt;
        const char* enabled = std::getenv("XYO_JIT_WARP_INDUCTION");
        if (enabled && enabled[0] == '0') return std::nullopt;
        const BlockInput* condition = loop->inputs.find("CONDITION");
        const BlockInput* bodyInput = loop->inputs.find("SUBSTACK");
        if (!condition || !bodyInput || bodyInput->block.empty())
            return std::nullopt;

        WarpInductionPlan plan;
        // All statements must be independent constant integer increments,
        // either `change x by N` or `set x to x +/- N`. Allow several distinct
        // variables but not repeated writes to one (intermediate conversions).
        Block* body = getBlock(bodyInput->block);
        for (unsigned count = 0; body; ++count) {
            if (count == 8 ||
                (body->opcode != "data_changevariableby" &&
                 body->opcode != "data_setvariableto")) return std::nullopt;
            const BlockField* written = findField(body, "VARIABLE");
            if (!written || written->id.empty()) return std::nullopt;
            std::optional<double> delta;
            if (body->opcode == "data_changevariableby") {
                delta = literalNumber(body, "VALUE");
            } else {
                const BlockInput* in = body->inputs.find("VALUE");
                Block* arithmetic = in
                    ? getBlock(in->block.empty() ? in->shadow : in->block)
                    : nullptr;
                if (!arithmetic ||
                    (arithmetic->opcode != "operator_add" &&
                     arithmetic->opcode != "operator_subtract"))
                    return std::nullopt;
                const BlockField* lhs = directVariableInput(arithmetic, "NUM1");
                const BlockField* rhs = directVariableInput(arithmetic, "NUM2");
                auto isSelf = [&](const BlockField* f) {
                    return f && f->id == written->id &&
                           sameNamedField(f, written);
                };
                if (isSelf(lhs)) {
                    delta = literalNumber(arithmetic, "NUM2");
                    if (delta && arithmetic->opcode == "operator_subtract")
                        *delta = -*delta;
                } else if (arithmetic->opcode == "operator_add" &&
                           isSelf(rhs)) {
                    delta = literalNumber(arithmetic, "NUM1");
                }
            }
            if (!delta ||
                std::trunc(*delta) != *delta || std::abs(*delta) > 65536.0)
                return std::nullopt;
            for (const auto& previous : plan.updates) {
                if (previous.field->id == written->id ||
                    sameNamedField(previous.field, written))
                    return std::nullopt;
            }
            plan.updates.push_back({written, static_cast<int64_t>(*delta)});
            if (body->next.empty()) break;
            body = getBlock(body->next);
            if (!body) return std::nullopt;
        }
        if (plan.updates.empty()) return std::nullopt;

        const std::string& conditionId = condition->block.empty()
            ? condition->shadow : condition->block;
        Block* test = getBlock(conditionId);
        bool negated = loop->opcode == "control_repeat_until";
        for (unsigned i = 0; test && test->opcode == "operator_not" && i < 4; ++i) {
            const BlockInput* child = test->inputs.find("OPERAND");
            if (!child) return std::nullopt;
            test = getBlock(child->block.empty() ? child->shadow : child->block);
            negated = !negated;
        }
        if (!test || (test->opcode != "operator_lt" &&
                      test->opcode != "operator_gt")) return std::nullopt;
        const BlockField* left = directVariableInput(test, "OPERAND1");
        const BlockField* right = directVariableInput(test, "OPERAND2");
        auto isUpdated = [&](const BlockField* field) {
            return field && std::any_of(
                plan.updates.begin(), plan.updates.end(),
                [&](const WarpInductionUpdate& entry) {
                    return entry.field->id == field->id ||
                           sameNamedField(entry.field, field);
                });
        };
        const bool lhsIsIndex = isUpdated(left);
        const bool rhsIsIndex = isUpdated(right);
        if (lhsIsIndex == rhsIsIndex) return std::nullopt;
        const BlockField* index = lhsIsIndex ? left : right;
        const BlockField* other = lhsIsIndex ? right : left;
        if (!index || index->id.empty()) return std::nullopt;
        const auto it = std::find_if(
            plan.updates.begin(), plan.updates.end(),
            [&](const WarpInductionUpdate& entry) {
                return entry.field->id == index->id &&
                       sameNamedField(entry.field, index);
            });
        if (it == plan.updates.end() || it->delta == 0)
            return std::nullopt;
        // If the same name has an unresolved/different ID, it may alias
        // another variable at runtime; do not use the aggregate path.
        for (const auto& entry : plan.updates)
            if (entry.field->id != index->id &&
                sameNamedField(entry.field, index)) return std::nullopt;

        const char* boundInput = lhsIsIndex ? "OPERAND2" : "OPERAND1";
        auto literal = literalNumber(test, boundInput);
        constexpr double kExact = 281474976710656.0; // 2^48
        if (literal && std::trunc(*literal) == *literal &&
            std::abs(*literal) <= kExact) {
            plan.boundLiteral = static_cast<int64_t>(*literal);
        } else if (other && !other->id.empty() && !isUpdated(other)) {
            for (const auto& entry : plan.updates)
                if (sameNamedField(entry.field, other)) return std::nullopt;
            plan.boundVariable = other;
        } else return std::nullopt;

        // Rewrite an RHS induction variable into the normal LHS orientation.
        const bool less = (test->opcode == "operator_lt") == lhsIsIndex;
        plan.direction = (less != negated) ? 1 : -1;
        plan.inclusive = negated;
        if ((it->delta > 0 ? 1 : -1) != plan.direction)
            return std::nullopt;
        plan.index = it->field;
        plan.stride = std::abs(it->delta);
        return plan;
    }

    // The acceleration writes nothing until all runtime guards pass. Each
    // chunk ends at exactly the next original 64-iteration warp poll; after
    // yielding, we reread BOTH index and dynamic bound/accumulator variables.
    void emitWarpInduction(const WarpInductionPlan& plan,
                           const Block* sourceLoop,
                           BasicBlock* fallback, BasicBlock* fastDone) {
        BasicBlock* prepare = newBB("induction.prepare");
        BasicBlock* head = newBB("induction.head");
        BasicBlock* valid = newBB("induction.valid");
        BasicBlock* update = newBB("induction.update");
        BasicBlock* perform = newBB("induction.perform");
        BasicBlock* abandon = newBB("induction.abandon");
        BasicBlock* finish = newBB("induction.finish");
        b.CreateCondBr(b.CreateICmpNE(loadIsWarp(), ci32(0)), prepare, fallback);
        b.SetInsertPoint(prepare);
        beginLoopYieldScope(sourceLoop);
        llvm::Value* real = realVarValuePtr(plan.index);
        llvm::Value* boundPtr = plan.boundVariable
            ? realVarValuePtr(plan.boundVariable) : nullptr;
        b.CreateBr(head);
        b.SetInsertPoint(head);
        llvm::Value* indexBits = b.CreateLoad(i64t, real);
        llvm::Value* indexOk = nullptr;
        llvm::Value* indexNumber = emitCmpNumF64(indexBits, &indexOk, true);
        llvm::Value* boundOk = b.getTrue();
        llvm::Value* boundNumber = cf64(static_cast<double>(plan.boundLiteral));
        if (boundPtr) {
            llvm::Value* boundBits = b.CreateLoad(i64t, boundPtr);
            boundNumber = emitCmpNumF64(boundBits, &boundOk, true);
        }
        constexpr double kExact = 281474976710656.0;
        auto safeIntegral = [&](llvm::Value* v) {
            return b.CreateAnd(
                b.CreateAnd(b.CreateFCmpOGE(v, cf64(-kExact)),
                            b.CreateFCmpOLE(v, cf64(kExact))),
                b.CreateFCmpOEQ(v, b.CreateUnaryIntrinsic(Intrinsic::trunc, v)));
        };
        llvm::Value* accepted = b.CreateAnd(
            b.CreateAnd(indexOk, boundOk),
            b.CreateAnd(safeIntegral(indexNumber), safeIntegral(boundNumber)));
        llvm::Value* index = b.CreateFPToSI(
            b.CreateSelect(accepted, indexNumber, cf64(0.0)), i64t);
        llvm::Value* bound = b.CreateFPToSI(
            b.CreateSelect(accepted, boundNumber, cf64(0.0)), i64t);
        b.CreateCondBr(accepted, valid, abandon);
        b.SetInsertPoint(valid);
        llvm::Value* distance = plan.direction > 0
            ? b.CreateSub(bound, index) : b.CreateSub(index, bound);
        llvm::Value* go = plan.inclusive
            ? b.CreateICmpSGE(distance, ci64(0))
            : b.CreateICmpSGT(distance, ci64(0));
        llvm::Value* safeDistance = b.CreateSelect(go, distance, ci64(0));
        llvm::Value* remaining = plan.inclusive
            ? b.CreateAdd(b.CreateUDiv(safeDistance, ci64(plan.stride)), ci64(1))
            : b.CreateUDiv(b.CreateAdd(safeDistance, ci64(plan.stride - 1)),
                           ci64(plan.stride));
        b.CreateCondBr(go, update, finish);
        b.SetInsertPoint(update);
        llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
        llvm::Value* toPoll = b.CreateSub(
            ci64(64), b.CreateAnd(ticks, ci64(63)));
        llvm::Value* chunk = b.CreateSelect(
            b.CreateICmpULT(remaining, toPoll), remaining, toPoll);

        // Validate secondary scalars BEFORE any store. Exact integral
        // arithmetic is associative in this bounded domain, so a batch
        // produces the same bits as one Scratch change-variable-by at a time.
        std::vector<std::pair<llvm::Value*, llvm::Value*>> finalValues;
        std::vector<llvm::Value*> initialValues;
        llvm::Value* allOk = b.getTrue();
        for (const auto& entry : plan.updates) {
            llvm::Value* ptr = entry.field->id == plan.index->id
                ? real : realVarValuePtr(entry.field);
            llvm::Value* current;
            if (entry.field->id == plan.index->id) {
                current = index;
            } else {
                llvm::Value* bits = b.CreateLoad(i64t, ptr);
                llvm::Value* isNumeric = nullptr;
                llvm::Value* value = emitCmpNumF64(bits, &isNumeric, true);
                allOk = b.CreateAnd(allOk, b.CreateAnd(isNumeric,
                                                        safeIntegral(value)));
                current = b.CreateFPToSI(
                    b.CreateSelect(safeIntegral(value), value, cf64(0.0)),
                    i64t);
            }
            llvm::Value* next = b.CreateAdd(
                current, b.CreateMul(chunk, ci64(entry.delta)));
            finalValues.emplace_back(ptr, next);
            initialValues.push_back(current);
        }
        b.CreateCondBr(allOk, perform, abandon);
        b.SetInsertPoint(perform);
        const char* scevMode = std::getenv("XYO_JIT_WARP_SCEV");
        const bool emitScevLoop = !scevMode || scevMode[0] != '0';
        if (emitScevLoop) {
            // LLVM receives a standard single-block SSA induction loop.
            // LLVM's normal O2/O3 loop passes analyze the AddRec and fold
            // the bounded microloop. Scratch's guards and warp poll remain.
            BasicBlock* preheader = b.GetInsertBlock();
            BasicBlock* nativeHead = newBB("xyo.affine.header");
            BasicBlock* nativeExit = newBB("xyo.affine.exit");
            b.CreateBr(nativeHead);
            b.SetInsertPoint(nativeHead);
            auto* iter = b.CreatePHI(i64t, 2, "xyo.affine.iter");
            iter->addIncoming(ci64(0), preheader);
            std::vector<llvm::PHINode*> states;
            states.reserve(initialValues.size());
            for (size_t i = 0; i < initialValues.size(); ++i) {
                auto* phi = b.CreatePHI(i64t, 2, "xyo.affine.value");
                phi->addIncoming(initialValues[i], preheader);
                states.push_back(phi);
            }
            std::vector<llvm::Value*> stepped;
            for (size_t i = 0; i < states.size(); ++i) {
                // Runtime proof bounds values to ±2^48 and 64*65536
                // extra, well within the exact integer range of IEEE f64.
                llvm::Value* next = b.CreateNSWAdd(
                    states[i], ci64(plan.updates[i].delta));
                stepped.push_back(next);
                states[i]->addIncoming(next, nativeHead);
            }
            llvm::Value* nextIter = b.CreateAdd(iter, ci64(1),
                                               "xyo.affine.next", true, true);
            iter->addIncoming(nextIter, nativeHead);
            b.CreateCondBr(
                b.CreateICmpULT(nextIter, chunk), nativeHead, nativeExit);
            b.SetInsertPoint(nativeExit);
            // LLVM requires all PHI instructions at the top of their block.
            // Keeping them contiguous also lets SCEV collect every affine
            // result before CFG rewriting, not just the first accumulator.
            std::vector<llvm::PHINode*> exitValues;
            for (llvm::Value* next : stepped) {
                auto* exitPhi = b.CreatePHI(i64t, 1, "xyo.affine.result");
                exitPhi->addIncoming(next, nativeHead);
                exitValues.push_back(exitPhi);
            }
            for (size_t i = 0; i < exitValues.size(); ++i) {
                llvm::Value* nextNumber = b.CreateSIToFP(exitValues[i], f64t);
                b.CreateStore(b.CreateBitCast(nextNumber, i64t),
                              finalValues[i].first);
            }
        } else {
            // Pre-SCEV implementation for isolated A/B measurements.
            for (const auto& [ptr, value] : finalValues) {
                llvm::Value* nextNumber = b.CreateSIToFP(value, f64t);
                b.CreateStore(b.CreateBitCast(nextNumber, i64t), ptr);
            }
        }
        emitSsaChunkScheduler(ticks, chunk, toPoll);
        b.CreateBr(head);
        BasicBlock* leave = newBB("induction.leave");
        b.SetInsertPoint(abandon);
        b.CreateBr(leave);
        b.SetInsertPoint(finish);
        b.CreateBr(leave);
        b.SetInsertPoint(leave);
        auto* completed = b.CreatePHI(b.getInt1Ty(), 2);
        completed->addIncoming(b.getFalse(), abandon);
        completed->addIncoming(b.getTrue(), finish);
        endLoopYieldScope(true);
        b.CreateCondBr(completed, fastDone, fallback);
        b.SetInsertPoint(fallback);
    }

    struct RepeatCopyOp {
        unsigned dst;
        std::optional<unsigned> src; // nullopt => constant numeric increment
        int64_t delta = 0;
    };
    struct RepeatCopyPlan {
        struct Variable {
            const BlockField* field;
            bool written = false;
        };
        std::vector<Variable> vars;
        std::vector<RepeatCopyOp> ops;
    };

    // Recognize a straight-line, pure sequence of scalar assignments and
    // constant increments (not a particular benchmark's variable names).
    // No intermediate value is visible to Scratch without a yield. The
    // runtime proof below requires all values to be plain, integral doubles.
    std::optional<RepeatCopyPlan> repeatCopyPlan(Block* loop) {
        const char* flag = std::getenv("XYO_JIT_REPEAT_COPY_SSA");
        if (flag && flag[0] == '0') return std::nullopt;
        const BlockInput* input = loop->inputs.find("SUBSTACK");
        if (!input || input->block.empty()) return std::nullopt;
        RepeatCopyPlan plan;
        auto resolve = [&](const BlockField* f) -> std::optional<unsigned> {
            if (!f || f->id.empty() || !f->value.isString() ||
                !f->value.asStrPtr()) return std::nullopt;
            for (unsigned i = 0; i < plan.vars.size(); ++i) {
                const auto* existing = plan.vars[i].field;
                if (existing->id == f->id) {
                    if (!sameNamedField(existing, f)) return std::nullopt;
                    return i;
                }
                if (sameNamedField(existing, f)) return std::nullopt;
            }
            if (plan.vars.size() == 8) return std::nullopt;
            plan.vars.push_back({f, false});
            return unsigned(plan.vars.size() - 1);
        };
        Block* statement = getBlock(input->block);
        for (unsigned count = 0; statement && count < 12; ++count) {
            const bool assign = statement->opcode == "data_setvariableto";
            if (!assign && statement->opcode != "data_changevariableby")
                return std::nullopt;
            const auto dst = resolve(findField(statement, "VARIABLE"));
            if (!dst) return std::nullopt;
            if (assign) {
                Block* rhs = transientChild(statement, "VALUE");
                if (!rhs || rhs->opcode != "data_variable")
                    return std::nullopt;
                const auto src = resolve(findField(rhs, "VARIABLE"));
                if (!src) return std::nullopt;
                plan.ops.push_back({*dst, *src, 0});
            } else {
                const auto amount = literalNumber(statement, "VALUE");
                if (!amount || !std::isfinite(*amount) ||
                    *amount != std::trunc(*amount) ||
                    std::abs(*amount) > 65536.0) return std::nullopt;
                plan.ops.push_back({*dst, std::nullopt, int64_t(*amount)});
            }
            plan.vars[*dst].written = true;
            if (statement->next.empty()) break;
            statement = getBlock(statement->next);
            if (!statement) return std::nullopt;
        }
        if (statement && !statement->next.empty()) return std::nullopt;
        if (plan.ops.size() < 2 || plan.ops.size() > 12) return std::nullopt;
        return plan;
    }

    void emitRepeatCopyChunk(const RepeatCopyPlan& plan,
                             llvm::Value* remaining32, AllocaInst* countdown,
                             BasicBlock* loopHead) {
        BasicBlock* fast = newBB("rep.copy.fast");
        BasicBlock* slow = newBB("rep.copy.slow");
        constexpr double kLimit = 281474976710656.0; // 2^48
        llvm::Value* valid = b.CreateICmpNE(loadIsWarp(), ci32(0));
        std::vector<llvm::Value*> starts;
        for (const auto& var : plan.vars) {
            llvm::Value* bits = emitVarGet(var.field);
            // A numeric string must retain its original boxed type if a
            // copy happens before the next addition. Do not speculate on it.
            llvm::Value* plain = b.CreateICmpSLT(bits, ci64(val::kNan));
            llvm::Value* value = b.CreateBitCast(
                b.CreateSelect(plain, bits, ci64(0)), f64t);
            llvm::Value* inRange = b.CreateAnd(
                b.CreateFCmpOGE(value, cf64(-kLimit)),
                b.CreateFCmpOLE(value, cf64(kLimit)));
            llvm::Value* integral = b.CreateFCmpOEQ(
                value, b.CreateUnaryIntrinsic(Intrinsic::trunc, value));
            // Copying -0 through an integer recurrence would turn it into
            // +0. Scratch's set-variable preserves the exact sign bit.
            llvm::Value* notNegativeZero = b.CreateICmpNE(
                bits, ci64(0x8000000000000000ULL));
            valid = b.CreateAnd(valid, b.CreateAnd(
                plain, b.CreateAnd(inRange,
                    b.CreateAnd(integral, notNegativeZero))));
            // fptosi on infinity/out-of-range doubles is LLVM poison.
            // Guard before converting, even though the fast CFG is cold.
            starts.push_back(b.CreateFPToSI(
                b.CreateSelect(inRange, value, cf64(0.0)), i64t));
        }
        struct GuardLowerer {
            IRBuilder<>& builder;
            BasicBlock* success;
            BasicBlock* failure;
            void require(llvm::Value* condition, const ir::Guard& guard) {
                assert(guard.failure == ir::GuardFailure::Fallback);
                builder.CreateCondBr(condition, success, failure);
                builder.SetInsertPoint(success);
            }
        } guardLowerer{b, fast, slow};
        ir::lowerGuard(guardLowerer,
                       {ir::GuardReason::IntegralRange,
                        ir::GuardFailure::Fallback}, valid);
        llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
        llvm::Value* toPoll = b.CreateSub(
            ci64(64), b.CreateAnd(ticks, ci64(63)));
        llvm::Value* available = b.CreateZExt(remaining32, i64t);
        llvm::Value* chunk = b.CreateSelect(
            b.CreateICmpULT(available, toPoll), available, toPoll);
        BasicBlock* preheader = b.GetInsertBlock();
        BasicBlock* ssaHead = newBB("rep.copy.header");
        BasicBlock* ssaExit = newBB("rep.copy.exit");
        b.CreateBr(ssaHead);
        b.SetInsertPoint(ssaHead);
        auto* iter = b.CreatePHI(i64t, 2, "rep.copy.iter");
        iter->addIncoming(ci64(0), preheader);
        std::vector<llvm::PHINode*> carries;
        std::vector<llvm::Value*> state;
        for (unsigned i = 0; i < starts.size(); ++i) {
            auto* phi = b.CreatePHI(i64t, 2, "rep.copy.var");
            phi->addIncoming(starts[i], preheader);
            carries.push_back(phi);
            state.push_back(phi);
        }
        for (const auto& op : plan.ops) {
            if (op.src) state[op.dst] = state[*op.src];
            else state[op.dst] = b.CreateNSWAdd(state[op.dst], ci64(op.delta));
        }
        llvm::Value* nextIter = b.CreateAdd(iter, ci64(1));
        iter->addIncoming(nextIter, ssaHead);
        for (unsigned i = 0; i < carries.size(); ++i)
            carries[i]->addIncoming(state[i], ssaHead);
        b.CreateCondBr(b.CreateICmpULT(nextIter, chunk), ssaHead, ssaExit);
        b.SetInsertPoint(ssaExit);
        // LLVM IR PHIs must be contiguous at the beginning of their block.
        std::vector<llvm::PHINode*> results;
        for (unsigned i = 0; i < carries.size(); ++i) {
            auto* phi = b.CreatePHI(i64t, 1, "rep.copy.result");
            phi->addIncoming(state[i], ssaHead);
            results.push_back(phi);
        }
        for (unsigned i = 0; i < results.size(); ++i) {
            if (!plan.vars[i].written) continue;
            llvm::Value* numeric = b.CreateSIToFP(results[i], f64t);
            llvm::Value* boxed = emitCanonKnownNonNaN(numeric);
            const auto* field = plan.vars[i].field;
            if (PromotedVar* pv = findPromoted(field); pv && pv->numeric)
                storeNumericPair(*pv, boxed, numeric, b.getInt1(true));
            else emitVarSet(field, boxed);
        }
        b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)),
                      countdown);
        emitSsaChunkScheduler(ticks, chunk, toPoll);
        b.CreateBr(loopHead);
        b.SetInsertPoint(slow);
    }

    struct RepeatAffinePlan {
        const BlockField* accumulator;
        int64_t delta;
    };

    // Single pure constant-increment body. The private repeat counter is
    // immutable to Scratch and already has an exact i32 trip count. If the
    // accumulator stops being a bounded integer after a warp yield, the
    // next chunk falls back to the ordinary per-iteration Scratch path.
    std::optional<RepeatAffinePlan> repeatAffinePlan(Block* loop) {
        const char* flag = std::getenv("XYO_JIT_REPEAT_SSA");
        if (flag && flag[0] == '0') return std::nullopt;
        const BlockInput* body = loop->inputs.find("SUBSTACK");
        if (!body || body->block.empty()) return std::nullopt;
        Block* statement = getBlock(body->block);
        if (!statement || !statement->next.empty() ||
            (statement->opcode != "data_changevariableby" &&
             statement->opcode != "data_setvariableto"))
            return std::nullopt;
        const BlockField* accumulator = findField(statement, "VARIABLE");
        if (!accumulator || accumulator->id.empty()) return std::nullopt;
        std::optional<double> constant;
        if (statement->opcode == "data_changevariableby") {
            constant = literalNumber(statement, "VALUE");
        } else {
            // `set acc to acc + constant` and `set acc to acc - constant`
            // are just as affine as the Scratch change-variable block.
            Block* expression = transientChild(statement, "VALUE");
            if (!expression || (expression->opcode != "operator_add" &&
                                expression->opcode != "operator_subtract"))
                return std::nullopt;
            const BlockField* lhs = directVariableInput(expression, "NUM1");
            const BlockField* rhs = directVariableInput(expression, "NUM2");
            auto isSelf = [&](const BlockField* f) {
                return f && f->id == accumulator->id &&
                       sameNamedField(f, accumulator);
            };
            if (isSelf(lhs)) {
                constant = literalNumber(expression, "NUM2");
                if (constant && expression->opcode == "operator_subtract")
                    *constant = -*constant;
            } else if (expression->opcode == "operator_add" && isSelf(rhs)) {
                constant = literalNumber(expression, "NUM1");
            }
        }
        if (!constant ||
            std::trunc(*constant) != *constant ||
            std::abs(*constant) > 65536.0 || *constant == 0.0)
            return std::nullopt;
        return RepeatAffinePlan{accumulator, static_cast<int64_t>(*constant)};
    }

    void emitRepeatAffineChunk(const RepeatAffinePlan& plan,
                               llvm::Value* remaining32,
                               AllocaInst* countdown,
                               BasicBlock* loopHead) {
        BasicBlock* fast = newBB("rep.ssa.fast");
        BasicBlock* slow = newBB("rep.ssa.slow");
        llvm::Value* bits = emitVarGet(plan.accumulator);
        llvm::Value* numericOk = nullptr;
        llvm::Value* current = emitCmpNumF64(bits, &numericOk, true);
        constexpr double kExactLimit = 281474976710656.0;
        llvm::Value* inRange = b.CreateAnd(
            b.CreateFCmpOGE(current, cf64(-kExactLimit)),
            b.CreateFCmpOLE(current, cf64(kExactLimit)));
        llvm::Value* isIntegral = b.CreateFCmpOEQ(
            current, b.CreateUnaryIntrinsic(Intrinsic::trunc, current));
        llvm::Value* valid = b.CreateAnd(
            b.CreateICmpNE(loadIsWarp(), ci32(0)),
            b.CreateAnd(numericOk, b.CreateAnd(inRange, isIntegral)));
        b.CreateCondBr(valid, fast, slow);
        b.SetInsertPoint(fast);
        llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
        llvm::Value* toPoll = b.CreateSub(
            ci64(64), b.CreateAnd(ticks, ci64(63)));
        llvm::Value* remaining = b.CreateZExt(remaining32, i64t);
        llvm::Value* chunk = b.CreateSelect(
            b.CreateICmpULT(remaining, toPoll), remaining, toPoll);
        llvm::Value* initial = b.CreateFPToSI(
            b.CreateSelect(valid, current, cf64(0.0)), i64t);
        BasicBlock* preheader = b.GetInsertBlock();
        BasicBlock* ssaHead = newBB("rep.ssa.header");
        BasicBlock* ssaExit = newBB("rep.ssa.exit");
        // Both native LLVM and WASM consume the same loop/backedge contract.
        // Native values stay in SSA: no load/store is inserted for the Phi.
        struct LoopLowerer {
            IRBuilder<>& builder;
            BasicBlock* head;
            BasicBlock* exit;
            void begin(ir::BlockId) {
                builder.CreateBr(head);
                builder.SetInsertPoint(head);
            }
            void latch(llvm::Value* condition, ir::BlockId, ir::BlockId) {
                builder.CreateCondBr(condition, head, exit);
            }
            void finish(ir::BlockId) { builder.SetInsertPoint(exit); }
        } lowerer{b, ssaHead, ssaExit};
        static const ir::BackedgeLoop loop(ir::Type::I64, 2);
        llvm::Value* next = nullptr;
        ir::lowerBackedgeLoop(loop, lowerer, [&]() -> llvm::Value* {
            auto* iteration = b.CreatePHI(i64t, 2, "rep.ssa.iter");
            auto* accumulator = b.CreatePHI(i64t, 2, "rep.ssa.value");
            iteration->addIncoming(ci64(0), preheader);
            accumulator->addIncoming(initial, preheader);
            next = b.CreateNSWAdd(accumulator, ci64(plan.delta));
            llvm::Value* nextIteration = b.CreateAdd(iteration, ci64(1));
            iteration->addIncoming(nextIteration, ssaHead);
            accumulator->addIncoming(next, ssaHead);
            return b.CreateICmpULT(nextIteration, chunk);
        });
        auto* finalValue = b.CreatePHI(i64t, 1, "rep.ssa.result");
        finalValue->addIncoming(next, ssaHead);
        llvm::Value* result = b.CreateSIToFP(finalValue, f64t);
        llvm::Value* resultBits = emitCanonKnownNonNaN(result);
        if (PromotedVar* pv = findPromoted(plan.accumulator); pv && pv->numeric)
            storeNumericPair(*pv, resultBits, result, b.getInt1(true));
        else
            emitVarSet(plan.accumulator, resultBits);
        b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)),
                      countdown);
        emitSsaChunkScheduler(ticks, chunk, toPoll, true);
        b.CreateBr(loopHead);
        b.SetInsertPoint(slow);
    }

    struct ForEachSumPlan {
        const BlockField* index;
        const BlockField* accumulator;
        int64_t scale;
    };

    // A for-each body consisting of a single linear reduction is safe to
    // lower to a typed SSA loop, provided the induction index is not written
    // by the body and the accumulator is an independent scalar. The loop's
    // private iteration count and Scratch-visible index are kept distinct.
    std::optional<ForEachSumPlan> forEachSumPlan(Block* loop) {
        const char* enabled = std::getenv("XYO_JIT_FOREACH_SSA");
        if (enabled && enabled[0] == '0') return std::nullopt;
        const BlockField* index = findField(loop, "VARIABLE");
        if (!index || index->id.empty()) return std::nullopt;
        const BlockInput* input = loop->inputs.find("SUBSTACK");
        if (!input || input->block.empty()) return std::nullopt;
        Block* stmt = getBlock(input->block);
        if (!stmt || !stmt->next.empty() ||
            (stmt->opcode != "data_changevariableby" &&
             stmt->opcode != "data_setvariableto"))
            return std::nullopt;
        const BlockField* acc = findField(stmt, "VARIABLE");
        if (!acc || acc->id.empty() || acc->id == index->id ||
            sameNamedField(acc, index)) return std::nullopt;
        Block* delta = transientChild(stmt, "VALUE");
        if (!delta) return std::nullopt;
        if (stmt->opcode == "data_setvariableto") {
            // `set acc to acc + index` is equivalent to an accumulator
            // increment under the same numeric-range guard. Its reporters
            // must be direct variables, with no unknown side effects.
            if (delta->opcode != "operator_add") return std::nullopt;
            const BlockField* lhs = directVariableInput(delta, "NUM1");
            const BlockField* rhs = directVariableInput(delta, "NUM2");
            auto isAcc = [&](const BlockField* f) {
                return f && f->id == acc->id && sameNamedField(f, acc);
            };
            if (isAcc(lhs))
                delta = transientChild(delta, "NUM2");
            else if (isAcc(rhs))
                delta = transientChild(delta, "NUM1");
            else
                return std::nullopt;
            if (!delta) return std::nullopt;
        }
        const BlockField* source = nullptr;
        int64_t scale = 1;
        if (delta->opcode == "data_variable") {
            source = findField(delta, "VARIABLE");
        } else if (delta->opcode == "operator_multiply") {
            const BlockField* lhs = directVariableInput(delta, "NUM1");
            const BlockField* rhs = directVariableInput(delta, "NUM2");
            std::optional<double> factor;
            if (lhs && lhs->id == index->id && sameNamedField(lhs, index)) {
                source = lhs;
                factor = literalNumber(delta, "NUM2");
            } else if (rhs && rhs->id == index->id &&
                       sameNamedField(rhs, index)) {
                source = rhs;
                factor = literalNumber(delta, "NUM1");
            }
            if (!factor || !std::isfinite(*factor) ||
                std::trunc(*factor) != *factor ||
                std::abs(*factor) > 1024.0) return std::nullopt;
            scale = static_cast<int64_t>(*factor);
        }
        if (!source || source->id != index->id ||
            !sameNamedField(source, index) || scale == 0)
            return std::nullopt;
        return ForEachSumPlan{index, acc, scale};
    }

    // Called from the regular control_for_each body. The slow branch starts
    // exactly at the same private counter, so deoptimization *after* a yield
    // cannot restart already executed iterations. A batch always ends at the
    // original 64-iteration clock poll, including an early final iteration.
    void emitForEachSumChunk(const ForEachSumPlan& plan,
                             llvm::Value* zeroIndex,
                             uint32_t tripCount, AllocaInst* counter,
                             BasicBlock* loopHead) {
        BasicBlock* fast = newBB("fe.ssa.fast");
        BasicBlock* slow = newBB("fe.ssa.slow");
        llvm::Value* initialBits = emitVarGet(plan.accumulator);
        llvm::Value* numericOk = nullptr;
        llvm::Value* current = emitCmpNumF64(initialBits, &numericOk, true);
        constexpr double kExactLimit = 281474976710656.0; // 2^48
        llvm::Value* inRange = b.CreateAnd(
            b.CreateFCmpOGE(current, cf64(-kExactLimit)),
            b.CreateFCmpOLE(current, cf64(kExactLimit)));
        llvm::Value* integral = b.CreateFCmpOEQ(
            current, b.CreateUnaryIntrinsic(Intrinsic::trunc, current));
        llvm::Value* valid = b.CreateAnd(
            b.CreateICmpNE(loadIsWarp(), ci32(0)),
            b.CreateAnd(numericOk, b.CreateAnd(inRange, integral)));
        b.CreateCondBr(valid, fast, slow);
        b.SetInsertPoint(fast);
        llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
        llvm::Value* untilPoll = b.CreateSub(
            ci64(64), b.CreateAnd(ticks, ci64(63)));
        llvm::Value* left = b.CreateZExt(
            b.CreateSub(ci32(tripCount), zeroIndex), i64t);
        llvm::Value* chunk = b.CreateSelect(
            b.CreateICmpULT(left, untilPoll), left, untilPoll);
        llvm::Value* firstIndex = b.CreateAdd(
            b.CreateZExt(zeroIndex, i64t), ci64(1));
        llvm::Value* safeCurrent = b.CreateSelect(valid, current, cf64(0.0));
        llvm::Value* startAcc = b.CreateFPToSI(safeCurrent, i64t);

        BasicBlock* preheader = b.GetInsertBlock();
        BasicBlock* ssaHead = newBB("fe.ssa.header");
        BasicBlock* ssaExit = newBB("fe.ssa.exit");
        b.CreateBr(ssaHead);
        b.SetInsertPoint(ssaHead);
        auto* step = b.CreatePHI(i64t, 2, "fe.ssa.step");
        step->addIncoming(ci64(0), preheader);
        auto* acc = b.CreatePHI(i64t, 2, "fe.ssa.acc");
        acc->addIncoming(startAcc, preheader);
        llvm::Value* nthIndex = b.CreateAdd(firstIndex, step);
        llvm::Value* delta = b.CreateMul(nthIndex, ci64(plan.scale));
        llvm::Value* nextAcc = b.CreateNSWAdd(acc, delta);
        llvm::Value* nextStep = b.CreateAdd(step, ci64(1), "fe.ssa.next");
        step->addIncoming(nextStep, ssaHead);
        acc->addIncoming(nextAcc, ssaHead);
        b.CreateCondBr(b.CreateICmpULT(nextStep, chunk), ssaHead, ssaExit);
        b.SetInsertPoint(ssaExit);
        auto* result = b.CreatePHI(i64t, 1, "fe.ssa.result");
        result->addIncoming(nextAcc, ssaHead);
        llvm::Value* finalAcc = b.CreateSIToFP(result, f64t);
        llvm::Value* lastIndex = b.CreateAdd(
            b.CreateZExt(zeroIndex, i64t), chunk);
        llvm::Value* finalIndex = b.CreateUIToFP(lastIndex, f64t);
        llvm::Value* accBits = emitCanonKnownNonNaN(finalAcc);
        llvm::Value* indexBits = emitCanonKnownNonNaN(finalIndex);
        if (PromotedVar* p = findPromoted(plan.accumulator); p && p->numeric)
            storeNumericPair(*p, accBits, finalAcc, b.getInt1(true));
        else
            emitVarSet(plan.accumulator, accBits);
        if (PromotedVar* p = findPromoted(plan.index); p && p->numeric)
            storeNumericPair(*p, indexBits, finalIndex, b.getInt1(true));
        else
            emitVarSet(plan.index, indexBits);
        b.CreateStore(b.CreateTrunc(lastIndex, i32t), counter);
        // Account for chunk-1 completed iterations; emitLoopYield handles
        // the final iteration, preserving the exact warp deadline phase.
        emitSsaChunkScheduler(ticks, chunk, untilPoll);
        b.CreateBr(loopHead);
        b.SetInsertPoint(slow);
    }

#include "conditional_ssa.inc"

    void emitStackBlock(Block* blk) {
        if (portableCallFrames() && !aot) {
            // Expire any completed source checkpoint BEFORE this statement
            // can run reporter inputs, side effects, or a new yield. This
            // executes only in opt-in CallFrame mode, not normal JIT.
            callN("xyo_proc_frame_statement_started", voidt,
                  {ctxArg, b.CreateGlobalString(blk->id)});
        }
        if (localReadForwardEnabled) {
            // Load-derived facts are an expression-local optimization. A
            // separate Scratch stack statement may call helpers (including
            // allocation/GC paths), so never carry a read cache between
            // statements even when LLVM keeps them in the same basic block.
            // Store-forwarding and branch-PHI facts are separate and stay.
            std::erase_if(localVarFacts,
                          [](const auto& entry) {
                              return entry.second.fromLoad;
                          });
        }
        const std::string& op = blk->opcode;
        const bool repeatedControl =
            op == "control_repeat" || op == "control_forever" ||
            op == "control_while" || op == "control_repeat_until" ||
            op == "control_for_each" || op == "control_wait_until";
        if (!repeatedControl && runtime.blockCountEnabled) {
            const BlockCached* cached = blocks->getCachedExecute(blk->id);
            if (cached && cached->blockFunction) emitBlockCount();
        }

        if (op == "control_if" || op == "control_if_else") {
            const char* ifPhiSetting = std::getenv("XYO_JIT_IF_PHI");
            // Keep this optional until an independent workload corpus
            // establishes a speedup beyond startup/compile-time noise.
            const bool ifPhi = op == "control_if" &&
                ifPhiSetting && ifPhiSetting[0] == '1' &&
                localForwardEnabled && localFactsAliasSafe && promotingLoop &&
                readonlyCondition(blk->inputs.find("CONDITION"));
            // The fact is defined BEFORE evaluating the condition. A
            // read-only condition can split into arbitrarily many BBs without
            // invalidating the incoming SSA value or its dominance.
            BasicBlock* conditionStart = b.GetInsertBlock();
            const auto priorFacts = ifPhi ? localVarFacts
                                         : decltype(localVarFacts){};
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* isT = emitTruthyV(cond);
            BasicBlock* thenBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* elseBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            // Only merge values known to originate in both successor paths.
            // The incoming typed values are SSA producers; PHIs below do not
            // make any global claims about a mutable Scratch variable's type.
            const bool mergeFacts = localForwardEnabled &&
                                    localFactsAliasSafe && promotingLoop;
            // In a plain IF, the false edge is a pass-through. A previous
            // same-basic-block store can be carried along that edge without
            // reloading the variable. Capture it only AFTER evaluating the
            // condition: all its reporter side effects have happened.
            if (mergeFacts) localVarFacts.clear();
            b.CreateCondBr(isT, thenBB, elseBB);
            b.SetInsertPoint(thenBB);
            if (op == "control_if" && promotingLoop) {
                const char* e = std::getenv("XYO_BRANCH_IF");
                if (!e || e[0] != '0') {
                    // Do not turn a conditional numeric update into an FP
                    // blend on the loop-carried value's dependency chain.
                    b.CreateCall(llvmc::intrinsicDeclaration(&mod, Intrinsic::sideeffect));
                }
            }
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block), true);
            }
            BasicBlock* thenEnd = b.GetInsertBlock();
            auto thenFacts = mergeFacts ? std::move(localVarFacts)
                                        : decltype(localVarFacts){};
            localVarFacts.clear();
            const bool thenReachesMerge = !bbDone(thenEnd);
            if (thenReachesMerge) b.CreateBr(merge);
            b.SetInsertPoint(elseBB);
            if (op == "control_if_else") {
                const BlockInput* in = blk->inputs.find("SUBSTACK2");
                if (in) emitChainBlocks(chainOf(in->block), true);
            }
            BasicBlock* elseEnd = b.GetInsertBlock();
            auto elseFacts = mergeFacts ? std::move(localVarFacts)
                                        : decltype(localVarFacts){};
            if (ifPhi && op == "control_if") {
                // This false edge preserves the value from before the IF.
                // No unknown effects exist along the condition's lowering;
                // its SSA producer dominates this edge, even if the
                // expression itself created intermediate basic blocks.
                for (const auto& [id, prior] : priorFacts) {
                    if (prior.block == conditionStart)
                        elseFacts[id] = {elseEnd, prior.bits};
                }
            }
            localVarFacts.clear();
            const bool elseReachesMerge = !bbDone(elseEnd);
            if (elseReachesMerge) b.CreateBr(merge);
            b.SetInsertPoint(merge);
            const char* demandSetting = std::getenv("XYO_JIT_PHI_DEMAND");
            const bool hasMerge = mergeFacts && thenReachesMerge &&
                                  elseReachesMerge;
            // Do not run the bounded Scratch reporter walk when there is
            // no eligible PHI to filter. Most script branches never enter
            // loop promotion, and this analysis is strictly a cost hint.
            bool hasCandidate = false;
            const bool demandRequested =
                demandSetting && demandSetting[0] == '1';
            if (demandRequested && hasMerge && !thenFacts.empty() &&
                !elseFacts.empty()) {
                for (const auto& [id, fromThen] : thenFacts) {
                    const auto other = elseFacts.find(id);
                    if (other != elseFacts.end() &&
                        fromThen.block == thenEnd &&
                        other->second.block == elseEnd) {
                        hasCandidate = true;
                        break;
                    }
                }
            }
            const bool demandEnabled = demandRequested && hasCandidate;
            const auto postUses = demandEnabled
                ? phiDemandAfter(blk)
                : std::unordered_map<std::string, PhiDemand>{};
            if (hasMerge) {
                ++phiMergeBlocks;
                if (demandEnabled) ++phiDemandScans;
                for (const auto& [variableId, fromThen] : thenFacts) {
                    auto fromElse = elseFacts.find(variableId);
                    if (fromElse == elseFacts.end() ||
                        fromThen.block != thenEnd ||
                        fromElse->second.block != elseEnd)
                        continue;
                    ++phiCandidates;
                    if (demandEnabled &&
                        (!postUses.contains(variableId) ||
                         !postUses.at(variableId).boxed)) {
                        ++phiBoxedSkipped;
                        continue;
                    }
                    ++phiBoxedEmitted;
                    // Both incoming SSA values are already materialized;
                    // casting/number conversion at the join would change
                    // Scratch's string/number comparison semantics.
                    auto* phi = b.CreatePHI(
                        i64t, 2, ifPhi ? "typed.if.bits" : "typed.merge.bits");
                    phi->addIncoming(fromThen.bits, thenEnd);
                    phi->addIncoming(fromElse->second.bits, elseEnd);
                    localVarFacts[variableId] = {merge, phi};
                    // Only forward the numeric shadow when *both* incoming
                    // SSA producers already carry an exact Scratch toNumber
                    // result AND its numeric-comparison eligibility. Mixing
                    // these independently would mis-handle whitespace
                    // strings, NaN and boolean coercion.
                    auto tn = knownNumD.find(fromThen.bits);
                    auto en = knownNumD.find(fromElse->second.bits);
                    auto tc = knownNumOk.find(fromThen.bits);
                    auto ec = knownNumOk.find(fromElse->second.bits);
                    const bool provenNumeric =
                        tn != knownNumD.end() && en != knownNumD.end() &&
                        tc != knownNumOk.end() && ec != knownNumOk.end();
                    if (provenNumeric && demandEnabled &&
                        !postUses.at(variableId).numeric)
                        ++phiNumericSkipped;
                    if (provenNumeric &&
                        (!demandEnabled || postUses.at(variableId).numeric)) {
                        ++phiNumericEmitted;
                        auto* numeric = b.CreatePHI(
                            f64t, 2,
                            ifPhi ? "typed.if.number" : "typed.merge.number");
                        numeric->addIncoming(tn->second, thenEnd);
                        numeric->addIncoming(en->second, elseEnd);
                        llvm::Value* eligible = tc->second;
                        if (tc->second != ec->second) {
                            // If both paths already proved the SAME
                            // comparison eligibility, no per-read type tag
                            // guard or PHI is needed. Otherwise preserve
                            // the full dynamic result from each edge.
                            auto* ok = b.CreatePHI(
                                b.getInt1Ty(), 2,
                                ifPhi ? "typed.if.cmpok" : "typed.merge.cmpok");
                            ok->addIncoming(tc->second, thenEnd);
                            ok->addIncoming(ec->second, elseEnd);
                            eligible = ok;
                        }
                        knownNumD[phi] = numeric;
                        knownNumOk[phi] = eligible;
                    }
                }
            }
            return;
        }

        if (op == "control_repeat") {
            const bool osrFirstLoop = blk == osrStartLoop;
            const bool boundedNativeInner =
                nestedOsrInFlightGuard && nestedOsrBoundedInner &&
                !nestedOsrInFlightInnerYield && !osrFirstLoop;
            // Real inner yields must not be hidden by repeat batching,
            // GPU/par-for delegation, or scalar strip mining.
            const bool protectedNativeInner = boundedNativeInner ||
                (nestedOsrInFlightGuard &&
                 nestedOsrInFlightInnerYield && !osrFirstLoop);
            const auto conditionalRepeat = (osrFirstLoop ||
                protectedNativeInner || runtime.blockCountEnabled)
                ? std::optional<ConditionalRepeatPlan>{}
                : conditionalRepeatPlan(blk);
            const auto repeatCopies = (osrFirstLoop || protectedNativeInner || runtime.blockCountEnabled)
                ? std::nullopt : repeatCopyPlan(blk);
            if (verbose && repeatCopies)
                std::fprintf(stderr, "xyo-jit: repeat SSA copies %s\n",
                             blk->id.c_str());
            const auto repeatAffine = (osrFirstLoop || protectedNativeInner || runtime.blockCountEnabled)
                ? std::nullopt : repeatAffinePlan(blk);
            if (verbose && repeatAffine)
                std::fprintf(stderr, "xyo-jit: repeat SSA affine %s\n",
                             blk->id.c_str());
            if (!osrFirstLoop && !protectedNativeInner && !runtime.blockCountEnabled) {
                if (auto plan = multiTransientPattern(blk)) {
                    emitTransientMultiRepeat(blk, *plan);
                    return;
                }
                if (auto plan = transientRepeatPattern(blk)) {
                    emitTransientRepeat(blk, *plan);
                    return;
                }
            }
            // for (i = toNum(TIMES); i >= 0.5; i--) { body; loopYield }
            // Fixed-count straight-line bodies are eligible for bounded
            // strip-mining into groups of at most 512 logical iterations.
            if (auto plan = (osrFirstLoop || protectedNativeInner || runtime.blockCountEnabled)
                    ? std::optional<RepeatUnrollPlan>{}
                    : repeatUnrollPlan(blk)) {
                // Evaluate TIMES once, exactly as in the ordinary lowering.
                (void)emitToNumF64(emitInputOf(blk, "TIMES"));
                if (verbose)
                    std::fprintf(stderr,
                        "xyo-jit: repeat unroll trips=%llu factor=%u blocks=%zu\\n",
                        static_cast<unsigned long long>(plan->trips),
                        plan->factor, plan->body.size());
                emitUnrolledRepeat(blk, *plan);
                return;
            }
            llvm::Value* n = nullptr;
            // Scratch evaluates TIMES on every control check, even though
            // only the first result initializes the private repeat counter.
            // In count mode, evaluate later checks at the loop header too.
            // This counts the real reporters and preserves their side effects.
            if (osrFirstLoop) {
                auto* pointer = b.CreateConstGEP1_32(
                    i8t, ctxArg, uint32_t(offsetof(JitCtx, osrRepeatRemaining)));
                n = b.CreateLoad(f64t, pointer, "osr.remaining");
            } else {
                n = emitToNumF64(emitInputOf(blk, "TIMES"));
            }
            // An empty warp repeat is just a sequence of loop-yield checks.
            // Advance the 64-iteration sampling counter in bulk, retaining
            // the 500 ms warp budget checks and the non-warp fallback.
            BasicBlock* emptyDone = nullptr;
            const BlockInput* repeatBody = blk->inputs.find("SUBSTACK");
            if ((!repeatBody || repeatBody->block.empty()) &&
                !runtime.blockCountEnabled &&
                !(osrFirstLoop && nestedOsrInFlightGuard)) {
                BasicBlock* emptyFast = BasicBlock::Create(lc, "rep.empty.fast", fn);
                BasicBlock* emptySlow = BasicBlock::Create(lc, "rep.empty.slow", fn);
                emptyDone = BasicBlock::Create(lc, "rep.empty.done", fn);
                llvm::Value* valid = b.CreateAnd(
                    b.CreateAnd(
                        b.CreateICmpNE(loadIsWarp(), ci32(0)),
                        b.CreateFCmpOGE(n, cf64(0.0))),
                    b.CreateAnd(
                        b.CreateFCmpOLE(n, cf64(1099511627776.0)),
                        b.CreateFCmpOEQ(
                            n, b.CreateUnaryIntrinsic(Intrinsic::floor, n))));
                b.CreateCondBr(valid, emptyFast, emptySlow);
                b.SetInsertPoint(emptyFast);
                const char* inlineScheduler = std::getenv(
                    "XYO_JIT_INLINE_EMPTY_SCHED");
                // Nested loops use the global counter in the original
                // helper. Only the direct deadline-poll lowering may be
                // used there: emitLoopYield() would mistakenly advance the
                // enclosing loop's private counter instead.
                const char* directPoll = std::getenv(
                    "XYO_JIT_INLINE_EMPTY_DIRECT");
                const bool directNested = directPoll && directPoll[0] == '1';
                if ((!curYieldCnt || directNested) &&
                    (!inlineScheduler || inlineScheduler[0] != '0') &&
                    !runtime.blockCountEnabled && !std::getenv("XYO_PROF"))
                    emitInlineEmptyWarpRepeat(n);
                else
                    call2("xyo_empty_repeat_warp", voidt, ctxArg, n);
                b.CreateBr(emptyDone);
                b.SetInsertPoint(emptySlow);
            }
            // ループ内スカラー変数をレジスタへ昇格する（可能なら）。
            // 昇格中は GPU 委譲しない（GPU 側はメモリを直接更新するため、
            // 戻り値の shadow と二重になる）。
            beginLoopYieldScope(blk);
            const bool promotedLoop = beginLoopPromotion(blk, false);
            // 本体が GPU 並列実行可能なら xyo_gpu_repeat へ委譲し、
            // 失敗時のみ従来の CPU ループへ進む分岐を emit する。
            const gpu::Program* gp =
                (nestedOsrInFlightGuard && (osrFirstLoop || protectedNativeInner)) ||
                promotedLoop
                    ? nullptr : tryGpuLoop(blk);
            // With a compile-time fixed nonnegative count <= INT32_MAX, the
            // existing IEEE test i >= 0.5 followed by i -= 1 performs exactly
            // round-to-nearest with half ties upward. Replacing the private
            // (never Scratch-visible) counter with an i32 countdown removes
            // the floating loop carry and compare. At any yield, the local
            // alloca remains live; variable spill/reload semantics are shared.
            std::optional<uint32_t> exactTrips;
            if (repeatIntegerCounter) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantFP>(n)) {
                    const double value = c->getValueAPF().convertToDouble();
                    if (std::isfinite(value) && value >= 0.0 &&
                        value <= 2147483647.0) {
                        const double integer = std::floor(value);
                        const double fractional = value - integer;
                        const uint64_t trips =
                            static_cast<uint64_t>(integer) +
                            static_cast<unsigned>(fractional >= 0.5);
                        if (trips <= uint64_t(INT32_MAX))
                            exactTrips = static_cast<uint32_t>(trips);
                    }
                }
            }
            AllocaInst* iv = exactTrips ? nullptr
                : entryAlloca(f64t, 1, "rep_i");
            AllocaInst* intIv = exactTrips
                ? entryAlloca(i32t, 1, "rep_i32") : nullptr;
            if (intIv)
                b.CreateStore(ci32(*exactTrips), intIv);
            else
                b.CreateStore(n, iv);
            // The only counted-loop form that has a directly recoverable
            // private scalar at this checkpoint. Keep it in the lexical
            // scope so nested loops never overwrite the outer snapshot.
            curYieldRemainingF64 = iv;
            curYieldRemainingI32 = intIv;
            auto* repeatFirst = runtime.blockCountEnabled && !osrFirstLoop
                ? entryAlloca(i8t, 1, "repeat.count.first") : nullptr;
            if (repeatFirst) b.CreateStore(ci8(1), repeatFirst);
            // 本体先頭の不変 pen setter をループ外へ（本体が 0 回なら実行しない）
            emitHoistedSetters(hoistableSetters(blk),
                               b.CreateFCmpOGE(n, cf64(0.5)));
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            // 「カウンタ + append」ループなら par-for を試す（warp 実行中のみ）。
            // 失敗（0 返却）時は従来の CPU ループへ落ちる。
            const bool triedParFor =
                nestedOsrInFlightGuard && (osrFirstLoop || boundedNativeInner)
                    ? false : tryEmitParFor(blk, n, head, merge);
            if (!triedParFor) {
                if (gp) {
                    llvm::Value* done =
                        callN("xyo_gpu_repeat", i32t,
                              {ctxArg, gpuProgPtr(gp, blk->id), n});
                    b.CreateCondBr(b.CreateICmpNE(done, ci32(0)), merge,
                                   head);
                } else {
                    b.CreateBr(head);
                }
            } else {
                // par-for は head（逐次フォールバック）と merge へ既に分岐済み
                b.SetInsertPoint(head);
            }
            b.SetInsertPoint(head);
            if (repeatFirst) {
                BasicBlock* recheck = newBB("repeat.count.input");
                BasicBlock* evaluated = newBB("repeat.count.evaluated");
                llvm::Value* first = b.CreateLoad(i8t, repeatFirst);
                b.CreateCondBr(b.CreateICmpNE(first, ci8(0)), evaluated, recheck);
                b.SetInsertPoint(recheck);
                // Actual second-and-later TIMES reporter evaluation.
                (void)emitInputOf(blk, "TIMES");
                if (!bbDone(b.GetInsertBlock())) b.CreateBr(evaluated);
                b.SetInsertPoint(evaluated);
                b.CreateStore(ci8(0), repeatFirst);
            }
            emitBlockCount();  // one repeat primitive per control check
            llvm::Value* go;
            if (intIv) {
                llvm::Value* count = b.CreateLoad(i32t, intIv);
                go = b.CreateICmpNE(count, ci32(0));
            } else {
                llvm::Value* i = b.CreateLoad(f64t, iv);
                go = b.CreateFCmpOGE(i, cf64(0.5));
            }
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            if (conditionalRepeat && intIv)
                emitConditionalRepeatChunk(*conditionalRepeat,
                    b.CreateLoad(i32t, intIv), intIv, head);
            if (repeatCopies && intIv)
                emitRepeatCopyChunk(*repeatCopies,
                    b.CreateLoad(i32t, intIv), intIv, head);
            if (repeatAffine && intIv)
                emitRepeatAffineChunk(*repeatAffine,
                    b.CreateLoad(i32t, intIv), intIv, head);
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block), true);
            }
            if (!bbDone(b.GetInsertBlock())) {
                // Only the PROVEN bounded inner-loop body's yield is
                // suppressed; the enclosing outer latch stays observable.
                if (!boundedNativeInner) emitLoopYield();
                if (intIv) {
                    llvm::Value* remaining = b.CreateLoad(i32t, intIv);
                    b.CreateStore(b.CreateSub(remaining, ci32(1)), intIv);
                } else {
                    llvm::Value* i2 = b.CreateLoad(f64t, iv);
                    b.CreateStore(b.CreateFSub(i2, cf64(1.0)), iv);
                }
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            endLoopYieldScope(true);
            endLoopPromotion(promotedLoop);
            if (emptyDone) {
                b.CreateBr(emptyDone);
                b.SetInsertPoint(emptyDone);
            }
            return;
        }

        if (op == "control_forever") {
            // forever の本体は必ず 1 回は走るのでガード不要
            emitHoistedSetters(hoistableSetters(blk), nullptr);
            beginLoopYieldScope(blk);
            const bool promotedLoop = beginLoopPromotion(blk, false);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            b.CreateBr(bodyBB);
            b.SetInsertPoint(bodyBB);
            emitBlockCount();  // control_forever repeats every body entry
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block), true);
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                b.CreateBr(bodyBB);
            }
            b.SetInsertPoint(merge);
            // forever は通常脱出しない（stop 系は昇格対象外）— 状態だけ戻す
            endLoopYieldScope(false);
            clearLoopPromotion(promotedLoop);
            return;
        }

        if (op == "control_while" || op == "control_repeat_until") {
            BasicBlock* inductionDone = nullptr;
            if (!runtime.blockCountEnabled)
            if (auto plan = warpInductionPlan(blk)) {
                BasicBlock* fallback = newBB("induction.fallback");
                inductionDone = newBB("induction.done");
                emitWarpInduction(*plan, blk, fallback, inductionDone);
                if (verbose)
                    std::fprintf(stderr,
                        "xyo-jit: guarded warp induction %s\n", blk->id.c_str());
            }
            if (!runtime.blockCountEnabled)
            if (auto plan = transientRepeatPattern(blk)) {
                if (safeTransientReporter(
                        transientChild(blk, "CONDITION"),
                        plan->variable->id)) {
                    emitTransientRepeat(blk, std::move(*plan));
                    return;
                }
            }
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            // 不変性が証明された counted while/until は GPU 並列へ —
            // 境界は limitParent/limitInput が指す不変オペランドを
            // ループ前に一度だけ評価する。元の条件は CPU パスで
            // 変更なく再評価される。
            beginLoopYieldScope(blk);
            const bool promotedLoop = beginLoopPromotion(blk, false);
            const gpu::ParallelLoop* pl =
                promotedLoop ? nullptr : tryGpuLoopEx(blk);
            if (pl && pl->limitParent && !pl->limitInput.empty()) {
                // 境界は生の数値のみ受理 — emitToNumF64 は NaN を 0 に
                // 畳むため emitCmpNumF64 で非数値は NaN として helper の
                // isfinite ガードへ渡す（sqrt(-1) 等で回数を捏造しない）。
                llvm::Value* limBits =
                    emitInputOf(const_cast<Block*>(pl->limitParent),
                                pl->limitInput.c_str());
                // Prime numeric-string caches without replacing a raw NaN.
                (void)emitToNumF64(limBits);
                llvm::Value* limOk = nullptr;
                llvm::Value* limNum = emitCmpNumF64(limBits, &limOk);
                llvm::Value* lim =
                    b.CreateSelect(limOk, limNum,
                                   cf64(std::numeric_limits<double>::quiet_NaN()));
                llvm::Value* done =
                    callN("xyo_gpu_repeat", i32t,
                          {ctxArg, gpuProgPtr(&pl->program, blk->id), lim});
                b.CreateCondBr(b.CreateICmpNE(done, ci32(0)), merge,
                               head);
            } else {
                b.CreateBr(head);
            }
            b.SetInsertPoint(head);
            emitBlockCount();  // while / repeat-until predicate checks
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* t = emitTruthyV(cond);
            llvm::Value* go = op == "control_while"
                ? t : b.CreateNot(t);
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            // MATH-004: guarded side-effect-free until loops; the slow
            // successor retains the entire original Scratch body.
            if (op == "control_repeat_until") {
                if (auto affine = affineUntilPlan(blk))
                    emitAffineUntilChunk(*affine, head);
                if (auto numeric = numericUntilPlan(blk))
                    emitNumericUntilChunk(*numeric, head);
            }
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block), true);
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            endLoopYieldScope(true);
            endLoopPromotion(promotedLoop);
            if (inductionDone) {
                b.CreateBr(inductionDone);
                b.SetInsertPoint(inductionDone);
            }
            return;
        }

        if (op == "control_for_each") {
            if (!runtime.blockCountEnabled)
            if (auto plan = transientRepeatPattern(blk)) {
                const BlockField* indexVar = findField(blk, "VARIABLE");
                if (indexVar && !indexVar->id.empty() &&
                    indexVar->id != plan->variable->id &&
                    safeTransientReporter(
                        transientChild(blk, "VALUE"), plan->variable->id)) {
                    emitTransientRepeat(blk, std::move(*plan));
                    return;
                }
            }
            const auto ssaSum = runtime.blockCountEnabled
                ? std::nullopt : forEachSumPlan(blk);
            if (verbose && ssaSum)
                std::fprintf(stderr, "xyo-jit: foreach SSA sum %s\n",
                             blk->id.c_str());
            // Scratch の for_each は index=0 から index < toNum(VALUE) の間
            // 回る（scratch3_control.forEach）。値が整数でない場合に
            // ceil 回になるので i=1..<=n では合わない（2.5 → 3 回）。
            // scratch3_control.forEach re-evaluates VALUE at every iteration.
            // Hoisting the reporter breaks loops that change a list's length.
            beginLoopYieldScope(blk);
            const bool promotedLoop = beginLoopPromotion(blk, false);
            const gpu::Program* gp =
                promotedLoop ? nullptr : tryGpuLoop(blk);
            // A literal for-each upper bound is immutable. Unlike a dynamic
            // reporter it cannot change across iterations or yield. The
            // private zero-based induction counter then fits u32, and
            // ceil(bound) is the exact number of iterations even for a
            // fractional bound such as 2.5. Other bounds retain the original
            // double counter and re-evaluation at each header.
            std::optional<uint32_t> intTripCount;
            if (foreachIntegerCounter) {
                const BlockInput* limit = blk->inputs.find("VALUE");
                if (limit) {
                    const std::string& source =
                        limit->block.empty() ? limit->shadow : limit->block;
                    Block* literal = getBlock(source);
                    if (literal && literal->shadow && literal->fields.size() == 1 &&
                        (literal->opcode == "math_number" ||
                         literal->opcode == "math_integer" ||
                         literal->opcode == "math_whole_number" ||
                         literal->opcode == "math_positive_number")) {
                        const double n = cast::toNumber(
                            literal->fields.begin()->second.value);
                        if (std::isfinite(n) && n >= 0.0 &&
                            n <= 2147483647.0)
                            intTripCount = static_cast<uint32_t>(std::ceil(n));
                    }
                }
            }
            AllocaInst* iv = intTripCount ? nullptr
                : entryAlloca(f64t, 1, "fe_i");
            AllocaInst* iv32 = intTripCount
                ? entryAlloca(i32t, 1, "fe_i32") : nullptr;
            if (iv32)
                b.CreateStore(ci32(0), iv32);
            else
                b.CreateStore(cf64(0.0), iv);
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            if (gp) {
                llvm::Value* gpuBound = emitToNumF64(emitInputOf(blk, "VALUE"));
                llvm::Value* done =
                    callN("xyo_gpu_repeat", i32t,
                          {ctxArg, gpuProgPtr(gp, blk->id), gpuBound});
                b.CreateCondBr(b.CreateICmpNE(done, ci32(0)), merge,
                               head);
            } else {
                b.CreateBr(head);
            }
            b.SetInsertPoint(head);
            emitBlockCount();  // for-each loop check
            llvm::Value* integralIndex = iv32
                ? b.CreateLoad(i32t, iv32) : nullptr;
            llvm::Value* i = iv32
                ? b.CreateUIToFP(integralIndex, f64t)
                : b.CreateLoad(f64t, iv);
            // Still emit VALUE in its original location. Only pure literal
            // bounds can use the faster i32 comparison.
            llvm::Value* bound = emitToNumF64(emitInputOf(blk, "VALUE"));
            llvm::Value* go = iv32
                ? b.CreateICmpULT(integralIndex, ci32(*intTripCount))
                : b.CreateFCmpOLT(i, bound);
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            if (ssaSum && intTripCount && iv32)
                emitForEachSumChunk(*ssaSum, integralIndex, *intTripCount,
                                    iv32, head);
            {
                const BlockField* f = findField(blk, "VARIABLE");
                if (f) {
                    llvm::Value* next = iv32
                        ? b.CreateUIToFP(
                            b.CreateAdd(integralIndex, ci32(1)), f64t)
                        : b.CreateFAdd(i, cf64(1.0));
                    llvm::Value* vv = iv32
                        ? emitCanonKnownNonNaN(next) : emitCanon(next);
                    if (PromotedVar* p = findPromoted(f); p && p->numeric)
                        storeNumericPair(*p, vv, next, b.getInt1(true));
                    else
                        emitVarSet(f, vv);
                }
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                const bool finiteIndex = promotedLoop && f &&
                    finiteIndexBody(in, f->id);
                bool inserted = false;
                if (finiteIndex)
                    inserted = finiteInductionVars.insert(f->id).second;
                bool boundedIndex = false;
                if (finiteIndex && inductionRangesEnabled &&
                    strictInductionBody(in, f)) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantFP>(bound)) {
                        if (c->getValueAPF().isFinite()) {
                            const double limit = c->getValueAPF().convertToDouble();
                            // The index in the body is i+1. Because i < bound,
                            // the last visible value is ceil(bound), even for
                            // fractional bounds. A signed-i32 cap also proves
                            // that every increment is exactly representable.
                            if (limit > 0.0 && limit <= 2147483647.0) {
                                activeInductionRanges.emplace(
                                    f->id, IntInterval{
                                        1, static_cast<int64_t>(std::ceil(limit))});
                                boundedIndex = true;
                            }
                        }
                    }
                }
                if (in) emitChainBlocks(chainOf(in->block), true);
                if (boundedIndex) activeInductionRanges.erase(f->id);
                if (inserted) finiteInductionVars.erase(f->id);
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                if (iv32) {
                    llvm::Value* i2 = b.CreateLoad(i32t, iv32);
                    b.CreateStore(b.CreateAdd(i2, ci32(1)), iv32);
                } else {
                    llvm::Value* i2 = b.CreateLoad(f64t, iv);
                    b.CreateStore(b.CreateFAdd(i2, cf64(1.0)), iv);
                }
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            endLoopYieldScope(true);
            endLoopPromotion(promotedLoop);
            return;
        }

        if (op == "control_all_at_once") {
            // TW irgen: "if 1 = 1" 相当 — サブスタックをそのまま実行
            const BlockInput* in = blk->inputs.find("SUBSTACK");
            if (in) emitChainBlocks(chainOf(in->block), true);
            return;
        }

        if (op == "control_wait") {
            llvm::Value* s = emitToNumF64(emitInputOf(blk, "DURATION"));
            call2("xyo_rt_wait", voidt, ctxArg, s);
            return;
        }

        if (op == "control_wait_until") {
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* waitBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* done = BasicBlock::Create(lc, "", fn);
            b.CreateBr(head);
            b.SetInsertPoint(head);
            emitBlockCount();  // wait-until retry
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* t = emitTruthyV(cond);
            b.CreateCondBr(t, done, waitBB);
            b.SetInsertPoint(waitBB);
            // 待機系の yield — danger モードでも他スレッドが進まないと
            // 条件が変わらないため、通常のループ yield ではなく
            // 「待機なら譲る」専用ヘルパを使う。
            const char* eventEnv = std::getenv("XYO_EVENT_SCHED");
            // Native JIT source tokens are real in-memory Block addresses.
            // AOT object files can hold obsolete addresses and must keep
            // ordinary polling. Instrumented block counting also retains
            // every retry rather than eliding logically executed blocks.
            const bool eventPureWait = !aot && !runtime.blockCountEnabled &&
                ((eventEnv && eventEnv[0] == '1') ||
                 isolatedEventWaitDependencyEnabled()) &&
                blocks->isPureWaitCondition(blk);
            if (eventPureWait)
                call2("xyo_wait_event_yield", voidt, ctxArg,
                      ci64(reinterpret_cast<uintptr_t>(blk)));
            else
                call1("xyo_wait_yield", voidt, ctxArg);
            b.CreateBr(head);
            b.SetInsertPoint(done);
            if (eventPureWait)
                call1("xyo_wait_event_done", voidt, ctxArg);
            return;
        }

        if (op == "control_stop") {
            const BlockField* f = findField(blk, "STOP_OPTION");
            std::u16string opt;
            if (f && f->value.isString() && f->value.asStrPtr())
                opt = f->value.asStrPtr()->view();
            if (opt == u"all") {
                call1("xyo_stop_all", voidt, ctxArg);
                emitStopped();  // 復帰しない — 後続は到達不能
            } else if (opt == u"other scripts in sprite" ||
                       opt == u"other scripts in stage") {
                call1("xyo_stop_others", voidt, ctxArg);
                // 自スレッドは継続 — ret しない
            } else {
                // 手続き内の "stop this script" は呼出し側へ復帰（JS
                // stopThisScript が procedures_call フレームまで巻き戻す
                // セマンティクス — 早期 return イディオム）。
                // exitBB 経由で isWarp 復帰も行う。
                if (inProc) b.CreateBr(getExitBB());
                else {
                    call1("xyo_stop_script", voidt, ctxArg);
                    emitStopped();
                }
            }
            return;
        }

        if (op == "data_setvariableto") {
            const BlockField* f = findField(blk, "VARIABLE");
            if (f) {
                emitVarSet(f, emitInputOf(blk, "VALUE"));
            }
            return;
        }
        if (op == "data_changevariableby") {
            const BlockField* f = findField(blk, "VARIABLE");
            if (f) {
                const bool selfDelta = sameScalarChangeInput(blk, f);
                llvm::Value* delta = emitInputOf(blk, "VALUE");
                if (selfDelta) ++duplicateScalarChanges;
                emitVarChange(f, delta, selfDelta);
            }
            return;
        }

        if (op == "event_broadcast") {
            llvm::Value* v = emitInputOf(blk, "BROADCAST_INPUT");
            call2("xyo_broadcast_v", voidt, ctxArg, v);
            return;
        }
        if (op == "event_broadcastandwait") {
            llvm::Value* v = emitInputOf(blk, "BROADCAST_INPUT");
            call2("xyo_broadcast_wait_v", voidt, ctxArg, v);
            return;
        }

        if (op == "procedures_call") {
            emitProcedureCall(blk);
            return;
        }

        if (op == "procedures_return") {
            // reporter ABI 経由でしか値は使われない — 副作用評価のみして終了
            if (blk->inputs.has("VALUE")) emitInputOf(blk, "VALUE");
            b.CreateBr(getExitBB());
            return;
        }

        // --- リスト操作 ---
        if (op == "data_replaceitemoflist") {
            const BlockField* f = findField(blk, "LIST");
            if (!f) return;   // フィールド欠落 → obj()==nullptr → no-op
            emitListReplace(blk, f);
            return;
        }
        if (op == "data_addtolist" || op == "data_deleteoflist" ||
            op == "data_deletealloflist" || op == "data_insertatlist") {
            const BlockField* f = findField(blk, "LIST");
            if (!f) return;   // フィールド欠落 → obj()==nullptr → no-op
            if (op == "data_addtolist") {
                emitListAdd(blk, f);
                return;
            }
            llvm::Value* v = varSlot(f, true);
            if (op == "data_deleteoflist")
                callN("xyo_list_delete", voidt,
                      {ctxArg, v, emitInputOf(blk, "INDEX")});
            else if (op == "data_deletealloflist")
                callN("xyo_list_deleteall", voidt, {ctxArg, v});
            else
                callN("xyo_list_insert", voidt,
                      {ctxArg, v, emitInputOf(blk, "INDEX"),
                       emitInputOf(blk, "ITEM")});
            return;
        }

        // --- motion XY 系（setXY fast path inline 化）---
        if (op == "motion_changexby") {
            emitMoveXY(blk, "DX", 0, "xyo_mot_changex");
            return;
        }
        if (op == "motion_changeyby") {
            emitMoveXY(blk, "DY", 1, "xyo_mot_changey");
            return;
        }
        if (op == "motion_setx") {
            emitMoveXY(blk, "X", 2, "xyo_mot_setx");
            return;
        }
        if (op == "motion_sety") {
            emitMoveXY(blk, "Y", 3, "xyo_mot_sety");
            return;
        }
        if (op == "motion_gotoxy") {
            llvm::Value* xv = emitInputOf(blk, "X");
            llvm::Value* yv = emitInputOf(blk, "Y");
            llvm::Value* nxv = emitToNumF64(xv);
            llvm::Value* nyv = emitToNumF64(yv);
            emitSetXY(nxv, nyv, [&] {
                if (boxedMotion) call3("xyo_mot_gotoxy", voidt, ctxArg, xv, yv);
                else call3("xyo_mot_setxy_num", voidt, ctxArg, nxv, nyv);
            });
            return;
        }

        // --- pen（compat を避けて直接ヘルパ/inline）---
        if (op == "pen_penUp")   { emitPenUp(); return; }
        if (op == "pen_penDown") { emitPenDown(); return; }
        if (op == "pen_clear")   { call1("xyo_pen_clear", voidt, ctxArg); return; }
        if (op == "pen_stamp")   { call1("xyo_pen_stamp", voidt, ctxArg); return; }
        if (op == "pen_setPenColorToColor") {
            emitPenSetColor(blk);
            return;
        }
        if (op == "pen_changePenColorParamBy" ||
            op == "pen_setPenColorParamTo") {
            bool change = op == "pen_changePenColorParamBy";
            int kind = penParamKind(blk, "COLOR_PARAM");
            if (kind >= 0) emitPenParam(blk, kind, change);
            else callOpCtx(
                change ? "xyo_pen_changeparam" : "xyo_pen_setparam",
                voidt, blk, "COLOR_PARAM", "VALUE");
            return;
        }
        if (op == "pen_setPenSizeTo") {
            callOpCtx("xyo_pen_setsize", voidt, blk, "SIZE");
            return;
        }
        if (op == "pen_changePenSizeBy") {
            callOpCtx("xyo_pen_changesize", voidt, blk, "SIZE");
            return;
        }

        // --- モニタ表示切替 ---
        if (op == "data_showvariable" || op == "data_hidevariable" ||
            op == "data_showlist" || op == "data_hidelist") {
            const bool isList = op == "data_showlist" || op == "data_hidelist";
            const BlockField* f = findField(blk, isList ? "LIST" : "VARIABLE");
            if (!f) return;
            const bool vis = op == "data_showvariable" || op == "data_showlist";
            callN("xyo_var_monitor", voidt,
                  {ctxArg, varIdC(f), ci32(vis ? 1 : 0)});
            return;
        }

        // --- 直接ヘルパ（スタック） ---
        {
            auto it = directStack().find(op);
            if (it != directStack().end()) {
                emitDirect(blk, it->second);
                return;
            }
        }

        // --- compat 層 ---
        if (hasSubstackInput(blk)) { fail = true; return; }
        emitCompat(blk);
    }

    BasicBlock* getExitBB() {
        if (!exitBB) exitBB = BasicBlock::Create(lc, "", fn);
        return exitBB;
    }

    void emitCompat(Block* blk) {
        // cached ops の先頭 BlockCached を取り、入力を書き込んでから呼ぶ
        BlockCached* bc = blocks->getCachedExecute(blk->id);
        if (!bc || !bc->blockFunction) { fail = true; return; }
        llvm::Value* bcP = slotPtr(AotSlotKind::BlockCached,
                                   bcKey(blk->id), bc);
        // 各入力を JIT で評価して argValues に書き込む
        for (auto& [name, in] : blk->inputs.items) {
            if (name == "SUBSTACK" || name == "SUBSTACK2") continue;
            llvm::Value* v = emitInputOf(blk, name);
            call3("xyo_rt_setarg", voidt, bcP,
                  slotPtr(AotSlotKind::Name, name, name.c_str()), v);
        }
        call2("xyo_rt_compat", i64t, ctxArg, bcP);
    }

    void emitProcedureCall(Block* blk) {
        // 未定義手続き呼び出しは no-op（TW irgen getProcedureInfo:
        // prototype 無し・definition 無しともに StackOpcode.NOP で
        // 引数入力も評価しない）。interp の procCall も同様に早期
        // return する。
        const std::string pc =
            blk->mutation ? blk->mutation->attr("proccode") : "";

        std::vector<std::u16string> names;
        std::vector<std::string> ids;
        std::vector<Value> defaults;
        if (!blocks->getProcedureParamNamesIdsAndDefaults(
                pc, names, ids, defaults)) return;

        std::string defId = blocks->getProcedureDefinition(pc);
        Block* def = defId.empty() ? nullptr : getBlock(defId);
        if (!def) return;
        const BlockInput* cb = def->inputs.find("custom_block");
        Block* proto = cb ? getBlock(cb->block) : nullptr;
        // innerDefinition 欠落は warp=false 扱い（TW 同様に許容）。
        // mutation（warp/引数情報）は procedures_prototype 側に載る
        const bool warp =
            (proto && proto->mutation) ? proto->mutation->warp() : false;
        const std::string vkey = pc + (warp ? "|w" : "|n");
        size_t nargs = ids.size();
        Function* pf = procFns[vkey];
        if (!pf) {
            // 引数はレジスタ渡し（ctx の後に i64 値を nargs 個）—
            // pargs 配列と ctx->args の差し替えを省く。callee 側の
            // argument reporter は仮引数 SSA を直接参照する。
            std::vector<Type*> params{ptrt};
            params.resize(1 + nargs, i64t);
            pf = Function::Create(
                FunctionType::get(voidt, params, false),
                Function::ExternalLinkage,
                prefix + "_p" + std::to_string(procCounter++), &mod);
            procFns[vkey] = pf;
            // ボディは後で emitPendingProcedures で埋める（再帰対応）
            pendingProcs.push_back({pf, def, warp, names});
        }

        // 引数を左→右に評価して実引数化（未接続入力は prototype の
        // デフォルト — JS procedures_call と同じく has()=false の
        // とき defaults[i]）
        std::vector<llvm::Value*> argv;
        argv.reserve(nargs + 1);
        argv.push_back(ctxArg);
        for (size_t i = 0; i < nargs; ++i) {
            const BlockInput* in = blk->inputs.find(ids[i]);
            if (in && (!in->block.empty() || !in->shadow.empty())) {
                argv.push_back(emitInputOf(blk, ids[i]));
            } else if (i < defaults.size()) {
                argv.push_back(constValue(defaults[i]));
            } else {
                argv.push_back(ci64(Value::num(0).bits));
            }
        }
        // 手続き内部の yield 可否は callEdges の到達集合で最終判定する
        callEdges[fn].push_back(pf);
        // 呼出し中は interpreter スタックにも procedures_call フレームを
        // 残す（JS 同形）— isRecursiveCall と warpMode 継承が参照する。
        // 手続き自身が yield/compat なヘルパを持たず呼出し循環にも
        // 属さない場合はフレームが観測不能 — elideLeafProcFrames で
        // push/pop を後から削る。
        if (portableCallFrames() && !aot) {
            // Capture the caller's active loop path before entering a child.
            // A recursive callee's yield otherwise sees stale parent state.
            // The same SSA counter snapshot is passed to the verified
            // enter helper: on the FIRST root call no session exists yet,
            // so the ordinary checkpoint helper above cannot publish it.
            const auto callerPath = emitPortableLoopCheckpoint(false);
            // Capture the exact register arguments in the common 64-bit
            // numeric ABI BEFORE xyo_proc_push can yield for recursive
            // nonwarp calls. Real heap pointers are filtered out by
            // the host mirror until a GC-safe Value ABI exists.
            llvm::Value* argumentBits =
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrt));
            if (nargs) {
                // Must allocate exactly once per LLVM function activation.
                // An alloca in a Scratch repeat loop would otherwise grow
                // the native Fiber stack on EVERY iteration until overflow.
                llvm::IRBuilder<> entryBuilder(
                    &fn->getEntryBlock(),
                    fn->getEntryBlock().getFirstInsertionPt());
                auto* cells = entryBuilder.CreateAlloca(
                    i64t, ci32(uint32_t(nargs)), "callframe.arguments");
                for (size_t i = 0; i < nargs; ++i) {
                    auto* cell = b.CreateInBoundsGEP(
                        i64t, cells, ci64(i));
                    b.CreateStore(argv[i + 1], cell);
                }
                argumentBits = cells;
            }
            auto sourceId = b.CreateGlobalString(blk->id);
            auto sourceCode = b.CreateGlobalString(pc);
            if (callerPath) {
                callN("xyo_proc_frame_enter_verified_rootpath", voidt,
                    {ctxArg, sourceId, sourceCode, argumentBits,
                     ci32(uint32_t(nargs)), ci64(compiledSourceRevision),
                     ci64(compiledSourceDigest.lo),
                     ci64(compiledSourceDigest.hi),
                     callerPath->ids, callerPath->counters,
                     ci32(callerPath->count)});
            } else {
                callN("xyo_proc_frame_enter_verified", voidt,
                    {ctxArg, sourceId, sourceCode, argumentBits,
                     ci32(uint32_t(nargs)), ci64(compiledSourceRevision),
                     ci64(compiledSourceDigest.lo),
                     ci64(compiledSourceDigest.hi)});
            }
        }
        auto* pushI = llvm::cast<CallInst>(callN(
            "xyo_proc_push", voidt,
            {ctxArg, b.CreateGlobalString(blk->id),
             slotPtr(AotSlotKind::BlockCached, bcKey(blk->id),
                     blocks->getCachedExecute(blk->id)),
             ci32(warp ? 1 : 0), b.CreateGlobalString(pc),
             ci32(nestedBranchDepth), ci32(0)}));
        if (portableCallFrames() && !aot)
            call1("xyo_proc_frame_body_started", voidt, ctxArg);
        auto* callI = llvm::cast<CallInst>(b.CreateCall(pf, argv));
        auto* popI = llvm::cast<CallInst>(
            call1("xyo_proc_pop", voidt, ctxArg));
        if (portableCallFrames() && !aot)
            call1("xyo_proc_frame_leave", voidt, ctxArg);
        procCallSites.push_back({pushI, callI, popI, pf, pc});
    }

    struct ProcCallSite {
        llvm::CallInst* push;
        llvm::CallInst* call;
        llvm::CallInst* pop;
        llvm::Function* callee;
        std::string proccode;
    };
    std::vector<ProcCallSite> procCallSites;

    /// A fully lowered warp-only procedure graph can run on the native fiber
    /// without materializing interpreter stack frames. Its implicit warp-loop
    /// timer may suspend the native fiber; the register arguments and native
    /// call stack survive that suspension. Explicit waits/compat/other
    /// stack-observing operations are excluded by the allowlist below.
    /// Non-warp recursion still needs frames for isRecursiveCall handling.
    bool canElideWarpRecursiveFrames(Function* start) {
        static const std::unordered_set<std::string_view> kStackInvisible = {
            "data_variable", "data_setvariableto", "data_changevariableby",
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_lt", "operator_gt", "operator_equals",
            "operator_and", "operator_or", "operator_not",
            "argument_reporter_string_number", "argument_reporter_boolean",
            "argument_reporter", "control_if", "control_if_else",
            // Only fully native, stack-transparent list/string operations.
            // These do not yield, use interpreter frames, or mutate warpMode.
            "data_addtolist", "data_deletealloflist", "data_itemoflist",
            "data_lengthoflist", "data_replaceitemoflist",
            "operator_letter_of", "operator_length", "operator_contains",
            // A warp loop may trigger the stuck-thread timer, but that
            // suspends the native fiber rather than entering the interpreter.
            "control_repeat_until", "control_repeat",
            "procedures_call",
        };
        const char* setting = std::getenv("XYO_JIT_RECURSIVE_FRAME_ELIDE");
        if (setting && setting[0] == '0') return false;
        std::unordered_set<Function*> seenProcs;
        std::function<bool(Function*)> check = [&](Function* f) -> bool {
            if (!seenProcs.insert(f).second) return true;
            // fnDirect also includes implicit warp-loop timer polls. Those
            // do not need interpreter frames. The strict opcode allowlist
            // below rejects all explicit yields and compatibility execution.

            // Only actual warp definitions are permitted. Looking up the
            // definition again avoids trusting the caller's warp state.
            std::string proccode;
            bool found = false;
            for (const auto& [key, candidate] : procFns) {
                if (candidate != f) continue;
                if (!key.ends_with("|w")) return false;
                proccode = key.substr(0, key.size() - 2);
                found = true;
                break;
            }
            if (!found) return false;
            const std::string defId = blocks->getProcedureDefinition(proccode);
            Block* def = defId.empty() ? nullptr : getBlock(defId);
            if (!def) return false;

            std::vector<std::string> todo{def->next};
            std::unordered_set<std::string> seenBlocks;
            while (!todo.empty()) {
                std::string id = std::move(todo.back());
                todo.pop_back();
                if (id.empty() || !seenBlocks.insert(id).second) continue;
                Block* node = getBlock(id);
                if (!node) return false;
                if (node->shadow) continue;
                if (!kStackInvisible.contains(node->opcode)) return false;
                if (!node->next.empty()) todo.push_back(node->next);
                for (const auto& [name, in] : node->inputs.items) {
                    (void)name;
                    if (!in.block.empty()) todo.push_back(in.block);
                    if (!in.shadow.empty()) todo.push_back(in.shadow);
                }
            }
            auto edges = callEdges.find(f);
            if (edges != callEdges.end())
                for (Function* callee : edges->second)
                    if (!check(callee)) return false;
            return true;
        };
        return check(start);
    }

    /// Remove interpreter stack frames from native procedure calls only when
    /// unobservable. Non-warp recursive procedures need the stack for the
    /// six-ancestor recursion check and may yield on recursive entry.
    void elideLeafProcFrames() {
        if (procCallSites.empty()) return;
        const bool preservePortableFrames = portableCallFrames() && !aot;
        std::unordered_set<Function*> cyclic;
        for (auto& [name, start] : procFns) {
            std::unordered_set<Function*> seen;
            std::function<bool(Function*)> visits = [&](Function* node) {
                auto edges = callEdges.find(node);
                if (edges == callEdges.end()) return false;
                for (Function* callee : edges->second) {
                    if (callee == start) return true;
                    if (seen.insert(callee).second && visits(callee)) return true;
                }
                return false;
            };
            if (visits(start)) cyclic.insert(start);
        }

        std::unordered_set<Function*> safeRecursive;
        for (Function* f : cyclic) {
            if (canElideWarpRecursiveFrames(f)) {
                safeRecursive.insert(f);
            } else {
                fnDirect[f] = true; // preserve conservative mayYield
            }
        }

        for (auto& s : procCallSites) {
            if (preservePortableFrames) {
                // Native recursion detection still depends on the old
                // interpreter stack. Preserve that behavior even when the
                // portable mirror is observing all procedure calls.
                if (cyclic.contains(s.callee) &&
                    !safeRecursive.contains(s.callee))
                    s.push->setArgOperand(6, ci32(1));
                continue;
            }
            if (cyclic.contains(s.callee)) {
                if (!safeRecursive.contains(s.callee)) {
                    s.push->setArgOperand(6, ci32(1));
                    continue;
                }
                if (verbose) std::fprintf(stderr,
                    "xyo-jit: frame elide (pure warp recursion) %s\n",
                    s.proccode.c_str());
            } else {
                auto dit = fnDirect.find(s.callee);
                if (dit != fnDirect.end() && dit->second) {
                    if (verbose) std::fprintf(stderr,
                        "xyo-jit: frame keep (yield) %s\n",
                        s.proccode.c_str());
                    continue;
                }
            }
            s.push->eraseFromParent();
            s.pop->eraseFromParent();
        }
    }

    /// 小さな手続きは callsite へ inline する — call/レジスタ引数準備・
    /// ret 分岐を消し、実引数 SSA が callee 本体へ直接流れ込む。
    /// 再帰・inline 不能は InlineFunction が拒否して呼出しのまま残る。
    /// 意味論に関わる判定（fnDirect/callEdges/elide）は emit 時点の
    /// グラフで済んでいるため、IR レベルの inline は安全。
    void inlineSmallProcCalls() {
        // Diagnostic knob for the code-size versus call-overhead tradeoff:
        // LLVM's normal inliner remains active regardless of this setting.
        // Zero skips the explicit Scratch-procedure inlining pass.
        unsigned maxProcInsts = 220;
        if (const char* limit = std::getenv("XYO_JIT_INLINE_LIMIT"))
            maxProcInsts = unsigned(std::strtoul(limit, nullptr, 10));
        if (maxProcInsts == 0) return;
        std::unordered_map<Function*, unsigned> cost;
        llvm::InlineFunctionInfo ifi;
        for (auto& s : procCallSites) {
            if (!s.call->getParent()) continue;   // 既に inline 済み
            auto it = cost.find(s.callee);
            if (it == cost.end()) {
                unsigned n = 0;
                for (auto& bb : *s.callee) n += unsigned(bb.size());
                it = cost.emplace(s.callee, n).first;
            }
            if (it->second > maxProcInsts) continue;
            llvm::InlineFunction(*s.call, ifi);
        }
    }

    /// Resolve stable Variable* handles once per native invocation, after all
    /// top-entry initializers (which can resize Thread::jitVarSlots). Slots are
    /// never rewritten during this invocation: compat runs the interpreter,
    /// native procedures reuse this script's slots, and yield resumes the same
    /// fiber. This is NOT value/list-pointer hoisting and makes no alias claim.
    void hoistVarSlotPointers() {
        // Indices belong to this script, NOT the shared AOT Module. Restrict
        // ownership explicitly, also excluding any earlier failed AOT emit.
        std::set<Function*> owned{entryFn};
        for (auto& [key, proc] : procFns) owned.insert(proc);
        for (Function* pf : parFns) owned.insert(pf);
        for (Function* f : owned) {
            if (!f || f->empty()) continue;
            std::vector<std::pair<LoadInst*, uint64_t>> loads;
            std::vector<std::pair<LoadInst*, uint64_t>> listLoads;
            for (BasicBlock& bb : *f) {
                for (Instruction& instruction : bb) {
                    auto* load = dyn_cast<LoadInst>(&instruction);
                    if (!load) continue;
                    if (MDNode* lm = load->getMetadata("xyo.varlist")) {
                        auto* wrapped = lm->getNumOperands() == 1
                            ? dyn_cast<ConstantAsMetadata>(
                                  lm->getOperand(0).get()) : nullptr;
                        auto* index = wrapped
                            ? dyn_cast<ConstantInt>(wrapped->getValue())
                            : nullptr;
                        load->setMetadata("xyo.varlist", nullptr);
                        if (!index || index->getZExtValue() >= numVarSlots ||
                            load->getType() != ptrt) {
                            fail = true;
                            return;
                        }
                        listLoads.emplace_back(load, index->getZExtValue());
                        continue;
                    }
                    MDNode* marker = load->getMetadata("xyo.varslot");
                    if (!marker) continue;
                    auto* wrapped = marker->getNumOperands() == 1
                        ? dyn_cast<ConstantAsMetadata>(marker->getOperand(0).get())
                        : nullptr;
                    auto* index = wrapped
                        ? dyn_cast<ConstantInt>(wrapped->getValue()) : nullptr;
                    // Strip even in A/B reload mode: subsequent shared-module
                    // emits must never reinterpret another script's indices.
                    load->setMetadata("xyo.varslot", nullptr);
                    if (!index || index->getZExtValue() >= numVarSlots ||
                        load->getType() != ptrt) {
                        fail = true;
                        return;
                    }
                    loads.emplace_back(load, index->getZExtValue());
                }
            }
            if ((loads.empty() && listLoads.empty()) || slotReload) continue;
            BasicBlock& entry = f->getEntryBlock();
            auto insert = entry.getFirstInsertionPt();
            bool hasInit = false;
            for (Instruction& instruction : entry) {
                auto* call = dyn_cast<CallInst>(&instruction);
                Function* callee = call ? call->getCalledFunction() : nullptr;
                if (callee && callee->getName() == "xyo_varslot_init") {
                    insert = instruction.getIterator();
                    ++insert;
                    hasInit = true;
                }
            }
            if ((f == entryFn && !hasInit) || insert == entry.end() ||
                f->arg_empty()) {
                fail = true;
                return;
            }
            IRBuilder<> ib(&entry, insert);
            llvm::Value* array = ib.CreateLoad(ptrt,
                ib.CreateConstGEP1_32(i8t, f->getArg(0), kVarSlotDataOff),
                "varslots");
            std::map<uint64_t, llvm::Value*> handles;
            for (auto [load, index] : loads) {
                auto [it, added] = handles.emplace(index, nullptr);
                if (added) {
                    llvm::Value* slot = ib.CreateGEP(ptrt, array, ci64(index));
                    it->second = ib.CreateLoad(ptrt, slot, "varslot");
                }
                load->replaceAllUsesWith(it->second);
            }
            // Variable::list loads: the Variable* itself comes from the same
            // slot array, so lift the field load to the entry block as well.
            std::map<uint64_t, llvm::Value*> listHandles;
            for (auto [load, index] : listLoads) {
                auto [it, added] = listHandles.emplace(index, nullptr);
                if (added) {
                    llvm::Value* var = handles.count(index)
                        ? handles[index] : nullptr;
                    if (!var) {
                        llvm::Value* slot = ib.CreateGEP(ptrt, array,
                                                         ci64(index));
                        var = ib.CreateLoad(ptrt, slot, "varslot");
                        handles[index] = var;
                    }
                    llvm::Value* lp = ib.CreateConstGEP1_32(i8t, var,
                                                           kVarListOff);
                    it->second = ib.CreateLoad(ptrt, lp, "varlist");
                }
                load->replaceAllUsesWith(it->second);
            }
            // Gather first, RAUW second, erase last: shared old GEP/load chains
            // may still feed other marked loads until all replacements exist.
            for (auto [load, index] : loads)
                RecursivelyDeleteTriviallyDeadInstructions(load);
            for (auto [load, index] : listLoads)
                RecursivelyDeleteTriviallyDeadInstructions(load);
        }
    }

    struct PendingProc {
        Function* f;
        Block* def;
        bool warp;
        std::vector<std::u16string> names;
    };
    std::vector<PendingProc> pendingProcs;

    /// par-for の本体関数を埋める。カウンタは反復ローカルへ差し替え、
    /// append は `base+iter` 番目のスロット書き込みになる。
    void emitPendingParFors() {
        while (!pendingParFors.empty()) {
            PendingParFor pp = std::move(pendingParFors.back());
            pendingParFors.pop_back();
            Function* savedFn = fn;
            llvm::Value* savedCtx = ctxArg;
            auto savedArgs = argIndex;
            auto savedProcArgs = curProcArgs;
            auto savedKnown = knownNum;
            const bool savedInProc = inProc;
            fn = pp.f;
            exitBB = nullptr;
            argIndex.clear();
            knownNum.clear();
            inProc = false;
            curProcArgs.clear();
            BasicBlock* entry = BasicBlock::Create(lc, "", pp.f);
            b.SetInsertPoint(entry);
            ctxArg = pp.f->getArg(0);
            // 反復カウンタ: init + iter*delta を Value bits で持つローカル
            AllocaInst* ctrA = b.CreateAlloca(i64t, nullptr, "pf_ctr");
            b.CreateStore(b.CreateBitCast(pp.f->getArg(2), i64t), ctrA);
            varOverride[pp.plan.counterField->id] = ctrA;
            ParForCtx pc;
            pc.lists = pp.f->getArg(3);
            pc.bases = pp.f->getArg(4);
            pc.iter = pp.f->getArg(1);
            for (size_t i = 0; i < pp.plan.lists.size(); ++i)
                pc.index[pp.plan.lists[i]->id] = int(i);
            parFor = &pc;
            emitChainBlocks(chainOf(pp.plan.bodyHead->id));
            if (!bbDone(b.GetInsertBlock())) b.CreateRetVoid();
            if (verifyFunction(*pp.f, &llvm::errs())) fail = true;
            parFor = nullptr;
            varOverride.clear();
            fn = savedFn;
            ctxArg = savedCtx;
            argIndex = std::move(savedArgs);
            curProcArgs = std::move(savedProcArgs);
            knownNum = std::move(savedKnown);
            inProc = savedInProc;
        }
    }

    void emitPendingProcedures() {
        while (!pendingProcs.empty()) {
            PendingProc pp = std::move(pendingProcs.back());
            pendingProcs.pop_back();
            Function* savedFn = fn;
            auto savedArgs = argIndex;
            auto savedProcArgs = curProcArgs;
            fn = pp.f;
            exitBB = nullptr;
            argIndex.clear();
            knownNum.clear();
            inProc = true;
            const bool savedFnWarp = curFnWarp;
            curFnWarp = pp.warp;
            curProcArgs.clear();
            for (size_t i = 0; i < pp.names.size(); ++i) {
                argIndex[pp.names[i]] = int64_t(i);
                if (i + 1 < pp.f->arg_size())
                    curProcArgs.push_back(pp.f->getArg(unsigned(i + 1)));
            }
            BasicBlock* entry = BasicBlock::Create(lc, "", pp.f);
            b.SetInsertPoint(entry);
            ctxArg = pp.f->getArg(0);
            // Interpreter enters the executable procedures_definition block
            // once per call. A native procedure entry must account for it,
            // independently of the body's blocks and arguments.
            if (runtime.blockCountEnabled) {
                const BlockCached* def = blocks->getCachedExecute(pp.def->id);
                if (def && def->blockFunction) emitBlockCount();
            }
            llvm::Value* saved = loadIsWarp();
            // 手続きの warpMode は呼出し元フレームから継承し、定義が
            // warp のときのみ 1 へ昇格する（非 warp 定義で 0 に降格
            // しない — JS stepToProcedure / TW irgen の
            // isWarp = script.isWarp || def.warp 相当）。
            if (pp.warp) storeIsWarp(ci32(1));
            emitChainBlocks(chainOf(pp.def->next));
            if (!bbDone(b.GetInsertBlock())) b.CreateBr(getExitBB());
            if (exitBB) {
                b.SetInsertPoint(exitBB);
                storeIsWarp(saved);
                b.CreateRetVoid();
            }
            fn = savedFn;
            argIndex = std::move(savedArgs);
            curProcArgs = std::move(savedProcArgs);
            inProc = false;
            curFnWarp = savedFnWarp;
            if (fail) return;
        }
    }
};

} // namespace

/// モジュールにスクリプトを emit。成功したら entry + procedures を埋める。
/// 失敗時は false（interpreter fallback）。
/// aot 非 null ならホストポインタを slot load に置き換える（AOT 用）。
bool jitEmitScript(llvm::Module& mod, llvm::LLVMContext& lc, Runtime& runtime,
                   Target* target, Blocks* blocks, const std::string& topBlockId,
                   const std::string& entryName,
                   CompiledScript& out, JitPinned& pinned,
                   std::vector<std::string>& pendingFnNames,
                   std::map<std::string, std::string>& procNameMap,
                   AotEmit* aot,
                   const Block* osrStart,
                   const ir::ScratchCompilationUnit* source,
                   bool standaloneOsrLoop) {
    Codegen cg{lc, mod, IRBuilder<>(lc), runtime, pinned};
    cg.target = target;
    cg.blocks = blocks;
    cg.gpuProgs = &out.gpuProgs;
    cg.aot = aot;
    cg.osrStartLoop = osrStart;
    if (const char* direct = std::getenv("XYO_JIT_OSR_NESTED_DIRECT_ARGS");
        direct && direct[0] == '1' && standaloneOsrLoop)
        cg.directNestedOsrArgs = true;
    if (const char* hoist = std::getenv("XYO_JIT_OSR_NESTED_HOIST_ARGS");
        cg.directNestedOsrArgs && hoist && hoist[0] == '1')
        cg.hoistNestedOsrArgs = true;
    if (const char* guard =
            std::getenv("XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT");
        standaloneOsrLoop && !aot && guard && guard[0] == '1') {
        const char* proof =
            std::getenv("XYO_JIT_OSR_NESTED_SOURCE_PROOF");
        cg.nestedOsrInFlightGuard = proof && proof[0] == '1';
        const char* innerYield = std::getenv(
            "XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT");
        cg.nestedOsrInFlightInnerYield = cg.nestedOsrInFlightGuard &&
            innerYield && innerYield[0] == '1';
        if (const char* bounded =
                std::getenv("XYO_JIT_OSR_NESTED_INNER_BOUNDED");
            cg.nestedOsrInFlightGuard &&
            !cg.nestedOsrInFlightInnerYield &&
            bounded && bounded[0] == '1')
            cg.nestedOsrBoundedInner = true;
    }
    // Validate a lossless snapshot of the selected source script. Capturing
    // only this script avoids copying the entire Scratch project on every
    // JIT invocation. Procedure definitions remain owned by the live Blocks.
    // LLVM's scheduler, OSR and BlockCached still depend on live Block
    // pointer identity; migrating those references requires further work.
    Block* top = blocks->getBlock(topBlockId);
    if (!top) return false;
    // Normal synchronous JIT receives the same captured source as WASM.
    // AOT/async entrypoints capture locally until their callers are migrated.
    const auto captured = source ? ir::ScratchCompilationUnit{}
                                 : ir::ScratchCompilationUnit::capture(*blocks, top);
    const auto& sharedUnit = source ? *source : captured;
    if (!sharedUnit.valid() || sharedUnit.script.topId != topBlockId)
        return false;
    cg.compiledSourceRevision = sharedUnit.sourceRevision;
    if (cg.portableCallFrames() && !aot) {
        cg.compiledSourceDigest = ir::scratchSourceDigest(sharedUnit);
        if (!cg.compiledSourceDigest) return false;
    }
    // Live Blocks are still consulted by the existing LLVM emitter.
    // Reject an async compilation whose snapshot became stale before
    // emission, rather than silently authorizing a newer revision.
    if (blocks->revision() != sharedUnit.sourceRevision)
        return false;
    if (!top) return false;
    // ハット本体は emit しない（startHats が評価する）。
    // edge-activated hat はコンパイル対象外。
    if (runtime.getIsEdgeActivatedHat(top->opcode)) return false;

    // For a nested procedure OSR, emit only the currently active loop.
    // The interpreter still owns the enclosing call stack and every suffix.
    std::vector<Block*> body = standaloneOsrLoop && osrStart
        ? std::vector<Block*>{blocks->getBlock(osrStart->id)}
        : cg.chainOf(osrStart ? osrStart->id : top->next);
    if (std::getenv("XYO_JIT_DUMP")) {
        llvm::errs() << "emit: top=" << top->opcode << " next=" << top->next
                     << " body.size=" << body.size() << "\n";
        for (Block* bb2 : body) llvm::errs() << "  blk " << bb2->opcode << "\n";
    }
    if (!cg.scanChain(body)) return false;

    cg.prefix = entryName;
    Function* f = Function::Create(
        FunctionType::get(cg.voidt, {cg.ptrt}, false),
        Function::ExternalLinkage, entryName, &mod);
    cg.fn = f;
    cg.entryFn = f;
    cg.ctxArg = f->getArg(0);
    BasicBlock* entry = BasicBlock::Create(lc, "", f);
    cg.b.SetInsertPoint(entry);
    if (std::getenv("XYO_JIT_DUMP"))
        llvm::errs() << "entry=" << (void*)entry << " f=" << (void*)f
                     << " fsize=" << f->size() << "\n";
    // RT-001 experiment: before the FIRST Scratch statement, attach a
    // source-verified root CallFrame even when the script makes no
    // procedure calls. The default and existing lazy procedure path
    // remain unchanged. This must not initialize a standalone nested
    // interpreter->LLVM OSR entry as a fresh top-level invocation.
    const bool rootOnlyDeoptProducer = !aot && !osrStart &&
        cg.portableCallFrames() && [] {
            const char* v = std::getenv("XYO_RT_ROOT_ONLY_DEOPT");
            return v && v[0] == '1';
        }();
    if (rootOnlyDeoptProducer)
        cg.callN("xyo_proc_frame_init_root_verified", cg.voidt,
                 {cg.ctxArg, cg.ci64(cg.compiledSourceRevision),
                  cg.ci64(cg.compiledSourceDigest.lo),
                  cg.ci64(cg.compiledSourceDigest.hi)});
    cg.emitChainBlocks(body);
    if (cg.hoistNestedOsrArgs &&
        std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
        std::fprintf(stderr, "[jit-osr-nested] hoisted-args=%zu\n",
                     cg.hoistedNestedOsrArgs.size());
    if (std::getenv("XYO_JIT_DUMP")) {
        llvm::errs() << "after chain: ipbb=" << (void*)cg.b.GetInsertBlock()
                     << " term=" << (void*)(cg.b.GetInsertBlock()
                         ? (void*)cg.b.GetInsertBlock()->getTerminator()
                         : nullptr)
                     << " fail=" << cg.fail << " fsize=" << f->size()
                     << " cgfn=" << (void*)cg.fn
                     << " entryinsts=" << entry->size()
                     << " entryparent=" << (void*)entry->getParent() << "\n";
        entry->print(llvm::errs());
    }
    if (!bbDone(cg.b.GetInsertBlock())) cg.b.CreateBr(cg.getExitBB());
    if (cg.exitBB) {
        cg.b.SetInsertPoint(cg.exitBB);
        cg.b.CreateRetVoid();
    }
    cg.emitPendingProcedures();
    cg.emitPendingParFors();
    if (cg.fail) return false;
    // No editor update may occur during the live Blocks lowering
    // interval. Source edits after publication are checked at every
    // native CallFrame handoff, where the interpreter deoptimizer runs.
    if (blocks->revision() != cg.compiledSourceRevision)
        return false;
    if (cg.compiledSourceDigest) {
        auto current = ir::ScratchCompilationUnit::capture(*blocks, top);
        if (!current.valid() ||
            ir::scratchSourceDigest(current) != cg.compiledSourceDigest)
            return false;
    }
    cg.elideLeafProcFrames();

    // 到達不能になった合流点など、終端のない BB を unreachable で閉じる
    IRBuilder<> ub(lc);
    for (Function& fn2 : mod) {
        for (BasicBlock& bb : fn2) {
            if (!bbDone(&bb)) {
                ub.SetInsertPoint(&bb);
                ub.CreateUnreachable();
            }
        }
    }
    cg.inlineSmallProcCalls();
    cg.hoistVarSlotPointers();
    if (cg.fail) return false;

    if (const char* stats = std::getenv("XYO_JIT_DUPLICATE_SCALAR_STATS");
        stats && stats[0] == '1') {
        std::fprintf(stderr, "[jit-duplicate-scalar] %s pairs=%llu self_changes=%llu\n",
                     topBlockId.c_str(),
                     (unsigned long long)cg.duplicateScalarNumericReads,
                     (unsigned long long)cg.duplicateScalarChanges);
    }
    if (const char* stats = std::getenv("XYO_JIT_LOCAL_READ_STATS");
        stats && stats[0] == '1') {
        std::fprintf(stderr, "[jit-local-read] %s reuses=%llu\n",
                     topBlockId.c_str(),
                     (unsigned long long)cg.localReadReuses);
    }
    if (const char* stats = std::getenv("XYO_JIT_PHI_STATS");
        stats && stats[0] == '1') {
        std::fprintf(stderr,
            "[jit-phi] %s merges=%llu demand_scans=%llu "
            "candidates=%llu boxed=%llu boxed_skipped=%llu "
            "numeric=%llu numeric_skipped=%llu\n",
            topBlockId.c_str(),
            (unsigned long long)cg.phiMergeBlocks,
            (unsigned long long)cg.phiDemandScans,
            (unsigned long long)cg.phiCandidates,
            (unsigned long long)cg.phiBoxedEmitted,
            (unsigned long long)cg.phiBoxedSkipped,
            (unsigned long long)cg.phiNumericEmitted,
            (unsigned long long)cg.phiNumericSkipped);
    }

    if (std::getenv("XYO_JIT_DUMP")) {
        llvm::errs() << "--- module for " << f->getName() << " (bbs="
                     << f->size() << ", fail=" << cg.fail << ") ---\n";
        mod.print(llvm::errs(), nullptr);
    }
    if (verifyFunction(*f, &llvm::errs())) return false;

    pendingFnNames.push_back(f->getName().str());
    out.entry = nullptr;  // JIT アドレス解決後に埋める
    out.mainThread = cg.mainThread;
    // mayYield = entry から到達可能な関数のいずれかが yield 点を持つか
    {
        std::unordered_map<llvm::Function*, int> memo;
        std::function<bool(llvm::Function*)> yields =
            [&](llvm::Function* g) -> bool {
                auto it = memo.find(g);
                if (it != memo.end()) return it->second != 0;
                memo[g] = 1;   // 再帰 → 保守的に true
                bool v = false;
                auto dit = cg.fnDirect.find(g);
                if (dit != cg.fnDirect.end()) v = dit->second;
                if (!v) {
                    auto cit = cg.callEdges.find(g);
                    if (cit != cg.callEdges.end())
                        for (llvm::Function* c : cit->second)
                            if (yields(c)) { v = true; break; }
                }
                memo[g] = v;
                return v;
            };
        out.mayYield = yields(f);
    }
    procNameMap.clear();
    for (auto& [k, pf] : cg.procFns) procNameMap[k] = pf->getName().str();
    return true;
}

} // namespace xyo
