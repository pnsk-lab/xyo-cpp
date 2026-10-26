// xyo-cpp: ブロックグラフ → LLVM IR コード生成。
// jsgen.js + irgen.js の C++/LLVM 版。生成関数は void fn(JitCtx*)。
// 値は i64 の NaN ボックス bit パターンとして受け渡しする。
// 入力・スタック双方、未 lowering のものは compat 層へ落とす。
#include "jit.hpp"
#include "aot_emit.hpp"
#include "parfor.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>

#include <cstddef>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../gpu/gpu_compile.hpp"
#include "../engine/runtime.hpp"
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
    LLVMContext& lc;
    Module& mod;
    IRBuilder<> b;
    Runtime& runtime;
    JitPinned& pinned;

    Target* target;
    Blocks* blocks;
    /// AOT モード: ホストポインタを slot load に置き換える（null なら JIT）。
    AotEmit* aot = nullptr;
    bool fail = false;
    const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;
    // Compile-time A/B switch; no branch is added to generated hot loops.
    const bool boxedMotion = std::getenv("XYO_JIT_MOTION_BOXED") != nullptr;
    const bool genericEq = std::getenv("XYO_JIT_EQ_GENERIC") != nullptr;
    const bool slotReload = std::getenv("XYO_JIT_SLOT_RELOAD") != nullptr;
    const bool genericLiteralNum = std::getenv("XYO_JIT_LITERAL_NUM_GENERIC") != nullptr;
    const bool genericMathBox = std::getenv("XYO_JIT_MATH_BOX_GENERIC") != nullptr;

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
        if (!gpuProgs || gpuDisabled) return nullptr;
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
        if (disabled || !curFnWarp || !blocks) return out;
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
        if (hs.empty()) return;
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
        if (parForDisabled || aot || !blocks) return false;
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
        return b.CreateLoad(ptrt, b.CreateConstGEP1_32(i64t, ctxArg, idx));
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
            "xyo_loop_yield", "xyo_rt_wait", "xyo_stop_all",
            "xyo_stop_script", "xyo_broadcast_wait_v", "xyo_rt_compat",
            "xyo_rt_reporter", "xyo_mot_glidexy", "xyo_mot_glideto",
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
    static constexpr uint32_t kYieldCheckNOff =
        uint32_t(offsetof(JitCtx, yieldCheckN));

    /// ループ末尾の yield 点。non-warp は常に xyo_loop_yield。
    /// warp は 500ms stuck 窓の時計読みを 64 回に 1 回へ間引く
    /// （判定遅延 << 500ms）。xyo_loop_yield 内で isWarp/isStuck を
    /// 再確認するので意味論は不変。
    void emitLoopYield() {
        llvm::Value* iw = loadIsWarp();
        BasicBlock* wBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* nwBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* chkBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* done = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(b.CreateICmpNE(iw, ci32(0)), wBB, nwBB);
        b.SetInsertPoint(nwBB);
        call1("xyo_loop_yield", voidt, ctxArg);
        b.CreateBr(done);
        b.SetInsertPoint(wBB);
        llvm::Value* np =
            b.CreateConstGEP1_32(i8t, ctxArg, kYieldCheckNOff);
        llvm::Value* n =
            b.CreateAdd(b.CreateLoad(i64t, np), ci64(1));
        b.CreateStore(n, np);
        llvm::Value* due =
            b.CreateICmpEQ(b.CreateAnd(n, ci64(63)), ci64(0));
        b.CreateCondBr(due, chkBB, done);
        b.SetInsertPoint(chkBB);
        call1("xyo_loop_yield", voidt, ctxArg);
        b.CreateBr(done);
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
        llvm::Value* tag = b.CreateAnd(b.CreateLShr(bits, ci64(48)),
                                       ci64(0xFFF8));
        return b.CreateOr(b.CreateICmpNE(tag, ci64(0x7FF8)),
                          b.CreateICmpEQ(bits, ci64(val::kNan)));
    }
    llvm::Value* emitAsF64(llvm::Value* bits) {
        return b.CreateBitCast(bits, f64t);
    }
    /// emitCanon 等で num タグが保証される SSA 値（再チェック省略用）
    std::unordered_set<llvm::Value*> knownNum;
    // Only constValue registers immutable string literals, keyed by their exact
    // emitted boxed Value, never by interpreting arbitrary ConstantInt pointers.
    // Keep across native functions: LLVM constants are safe module-wide, while
    // instruction keys (including AOT slot-derived bits) are unique SSA values.
    // Payloads are doubles only: no GC references or AOT host pointers.
    std::map<llvm::Value*, double> literalNums;

    /// double → NaN 正規化 bits（Value::numCanonical と同型）
    llvm::Value* emitCanon(llvm::Value* d) {
        llvm::Value* isn = b.CreateFCmpUNO(d, d);
        llvm::Value* r = b.CreateSelect(isn, ci64(val::kNan),
                                      b.CreateBitCast(d, i64t));
        knownNum.insert(r);
        return r;
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
        if (auto d = constNumV(bits))
            return cf64(std::isnan(*d) ? 0.0 : *d);
        if (!genericLiteralNum) {
            auto it = literalNums.find(bits);
            if (it != literalNums.end()) return cf64(it->second);
        }
        if (knownNum.count(bits)) return emitNotNaN(emitAsF64(bits));
        llvm::Value* ok;
        llvm::Value* d = emitNumBoolF64(bits, &ok);
        llvm::Value* nd = emitNotNaN(d);
        BasicBlock* fastBB = b.GetInsertBlock();
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* strBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* hitBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* missBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* zeroBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(ok, merge, slowBB);
        b.SetInsertPoint(slowBB);
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(isStr, strBB, zeroBB);
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
        llvm::Value* hv = call1("xyo_to_num", f64t, bits);
        b.CreateBr(merge);
        b.SetInsertPoint(zeroBB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(f64t, 4);
        p->addIncoming(nd, fastBB);
        p->addIncoming(cd, hitBB);
        p->addIncoming(hv, missBB);
        p->addIncoming(cf64(0.0), zeroBB);
        return p;
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
    static constexpr uint32_t kSStrRgbStateOff =
        uint32_t(offsetof(SStr, rgbState));
    static constexpr uint32_t kSStrRgbCacheOff =
        uint32_t(offsetof(SStr, rgbCache));

    /// ctx->varSlotData[idx] から Variable* を直接 load して返す。
    /// 初出の (id,isList) にはエントリ関数先頭へ xyo_varslot_init を
    /// 挿入（全使用点を支配。proc 呼出しでは init は再実行されない）。
    llvm::Value* varSlot(const BlockField* f, bool isList) {
        std::string key = f->id;
        key += isList ? '\x01' : '\x02';
        auto [it, ins] = varSlotIdx.emplace(key, numVarSlots);
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
    llvm::Value* varValuePtr(const BlockField* f) {
        // par-for の反復カウンタは反復ローカル（i64 の Value bits）へ差し替える
        if (!varOverride.empty()) {
            auto ov = varOverride.find(f->id);
            if (ov != varOverride.end()) return ov->second;
        }
        llvm::Value* v = varSlot(f, false);
        return b.CreateConstGEP1_32(i8t, v, kVarValueOff);
    }
    llvm::Value* varListPtr(const BlockField* f) {
        llvm::Value* v = varSlot(f, true);
        llvm::Value* lp = b.CreateConstGEP1_32(i8t, v, kVarListOff);
        return b.CreateLoad(ptrt, lp);
    }

    llvm::Value* emitVarGet(const BlockField* f) {
        // jitVar（lookupOrCreate）は常に非 null — helper の v? ガードと等価
        return b.CreateLoad(i64t, varValuePtr(f));
    }
    void emitVarSet(const BlockField* f, llvm::Value* val) {
        b.CreateStore(val, varValuePtr(f));
    }
    void emitVarChange(const BlockField* f, llvm::Value* delta) {
        llvm::Value* vp = varValuePtr(f);
        llvm::Value* cur = b.CreateLoad(i64t, vp);
        // xyo_var_change と同型: numCanonical(toNum(cur)+toNum(delta))
        llvm::Value* s = emitCanon(
            b.CreateFAdd(emitToNumF64(cur), emitToNumF64(delta)));
        b.CreateStore(s, vp);
    }

    // --- 算術・比較の fast path -------------------------------------------

    enum class AOp { Add, Sub, Mul, Div, Mod };
    /// xyo_add/sub/mul/div/mod と同型 — numCanonical(toNum(a) op
    /// toNum(b)) を直接 emit（helper 呼出しなし）。
    llvm::Value* emitArith(Block* blk, const char* in1, const char* in2,
                           AOp aop) {
        llvm::Value* a = emitInputOf(blk, in1);
        llvm::Value* bv = emitInputOf(blk, in2);
        llvm::Value* fa = emitToNumF64(a);
        llvm::Value* fb = emitToNumF64(bv);
        llvm::Value* r;
        switch (aop) {
        case AOp::Add: r = b.CreateFAdd(fa, fb); break;
        case AOp::Sub: r = b.CreateFSub(fa, fb); break;
        case AOp::Mul: r = b.CreateFMul(fa, fb); break;
        case AOp::Div: r = b.CreateFDiv(fa, fb); break;
        case AOp::Mod: {
            llvm::Value* r0 = b.CreateFRem(fa, fb);
            r = b.CreateFRem(b.CreateFAdd(r0, fb), fb);
            r = b.CreateSelect(b.CreateFCmpOEQ(r, cf64(0.0)),
                               cf64(0.0), r);
            break;
        }
        }
        return emitCanon(r);
    }

    /// cast::compare 意味での数値変換。*okOut=true なら f64 が有効
    /// （num(kNan除く)|true|false|numState==1 の数値キャッシュ済み str）。
    /// それ以外は NaN→文字列比較の領域なので helper に任せる。
    llvm::Value* emitCmpNumF64(llvm::Value* bits, llvm::Value** okOut) {
        if (knownNum.count(bits)) {
            *okOut = b.CreateICmpNE(bits, ci64(val::kNan));
            return emitAsF64(bits);
        }
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
        b.CreateCondBr(ok0, merge, slowBB);
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
        *okOut = ok;
        return d;
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
            // round(f(pi*x/180)*1e10)/1e10 — math-util 準拠
            llvm::Value* rad = b.CreateFMul(x, cf64(kPi / 180.0));
            llvm::Value* v = b.CreateCall(
                decl(fn == 4 ? "sin" : "cos", f64t, {f64t}), {rad});
            v = b.CreateUnaryIntrinsic(Intrinsic::round,
                                       b.CreateFMul(v, cf64(1e10)));
            return b.CreateFDiv(v, cf64(1e10));
        }
        case 7: case 8: case 9: {
            llvm::Value* v = libm(fn == 7 ? "asin" : fn == 8 ? "acos" : "atan");
            return b.CreateFMul(v, cf64(180.0 / kPi));
        }
        case 10: return uni(Intrinsic::log);
        case 11:
            return b.CreateFDiv(uni(Intrinsic::log),
                                cf64(2.302585092994046));
        case 12: return uni(Intrinsic::exp);
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
        b.CreateCondBr(emitIsNum(l), numBB, slowBB);
        b.SetInsertPoint(numBB);
        b.CreateCondBr(b.CreateICmpEQ(
                           b.CreateLShr(s, ci64(48)),
                           ci64(val::kTagStr >> 48)),
                       strBB, slowBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(s, ci64(val::kPtrMask)), ptrt);
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
            if (c < 'a' || c > 'z') return std::nullopt;
        }
        // Test the ORIGINAL spelling: "Infinity" is numeric but "infinity"
        // is not. Numeric literals must retain Scratch's numeric comparison
        // (e.g. Infinity == "1e999"), not just case-insensitive text matching.
        if (!std::isnan(cast::toNumberOrNaN(field->value))) return std::nullopt;
        return word;
    }

    /// Equality with a nonnumeric ASCII word of 1..8 letters (3D2 uses "n"
    /// and "none"). A different first ASCII unit cannot lowercase to the word:
    /// lowercasing never removes that ASCII prefix. Otherwise inline comparison
    /// requires equal length and ALL ASCII units. Unicode anywhere, or unequal
    /// length after a matching prefix, falls back without assuming anything
    /// about Unicode length or mappings (Kelvin sign, dotted I, surrogates).
    /// Only SINGLE letters can reject non-heap Values: numeric cast of the
    /// literal is NaN, forcing string comparison; finite numbers contain digits,
    /// NaN/Infinity/true/false/undefined/r,g,b have multiple characters, and the
    /// inline empty string has none. Words retain the helper because e.g. bool
    /// true == "true" and numeric NaN == "NaN" must still succeed.
    llvm::Value* emitEqAsciiWord(llvm::Value* a, llvm::Value* bv,
                               llvm::Value* dynamic, std::u16string_view word) {
        BasicBlock* strBB = newBB("eq.lit.str");
        BasicBlock* firstBB = newBB("eq.lit.first");
        BasicBlock* asciiBB = newBB("eq.lit.ascii");
        BasicBlock* lenBB = newBB("eq.lit.len");
        BasicBlock* wordBB = newBB("eq.lit.word");
        BasicBlock* compareBB = newBB("eq.lit.compare");
        BasicBlock* ret0BB = newBB("eq.lit.no");
        BasicBlock* ret1BB = newBB("eq.lit.yes");
        BasicBlock* helperBB = newBB("eq.lit.slow");
        BasicBlock* merge = newBB("eq.lit.merge");
        llvm::Value* isStr = b.CreateICmpEQ(
            b.CreateLShr(dynamic, ci64(48)), ci64(val::kTagStr >> 48));
        b.CreateCondBr(isStr, strBB, word.size() == 1 ? ret0BB : helperBB);
        b.SetInsertPoint(strBB);
        llvm::Value* sp = b.CreateIntToPtr(
            b.CreateAnd(dynamic, ci64(val::kPtrMask)), ptrt);
        llvm::Value* len = b.CreateLoad(i32t, fld(sp, kSStrLenOff));
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

    /// operator_equals（i32）。xyo_eq と同型:
    /// 1) bits 一致 → 1  2) 両辺 compare 数値 → fcmp
    /// 3) 両 str かつ numState 既知かつ不一致 → 0  4) 他 → helper
    llvm::Value* emitEq(Block* blk) {
        llvm::Value* a = emitInputOf(blk, "OPERAND1");
        llvm::Value* bv = emitInputOf(blk, "OPERAND2");
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
        llvm::Value* fa = emitCmpNumF64(a, &okA);
        llvm::Value* fb = emitCmpNumF64(bv, &okB);
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
        llvm::Value* h = call2("xyo_eq", i32t, a, bv);
        b.CreateBr(merge);
        b.SetInsertPoint(ret1BB);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i32t, 5);
        p->addIncoming(ci32(1), sameBB);
        p->addIncoming(c, cmpBB);
        p->addIncoming(ci32(1), ret1BB);
        p->addIncoming(ci32(0), ret0BB);
        p->addIncoming(h, helperBB);
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
            // wrapColor: fmod(v,100); <0 → +100
            nv = b.CreateFRem(nv, cf64(100.0));
            nv = b.CreateSelect(
                b.CreateFCmpOLT(nv, cf64(0.0)),
                b.CreateFAdd(nv, cf64(100.0)), nv);
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
        llvm::Value *okA, *okB;
        llvm::Value* fa = emitCmpNumF64(a, &okA);
        llvm::Value* fb = emitCmpNumF64(bv, &okB);
        llvm::Value* both = b.CreateAnd(okA, okB);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(both, fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* c1 = b.CreateFCmp(pred, fa, fb);
        llvm::Value* z = b.CreateZExt(c1, i32t);
        b.CreateBr(merge);
        b.SetInsertPoint(slowBB);
        llvm::Value* s = call2(helper, i32t, a, bv);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        PHINode* p = b.CreatePHI(i32t, 2);
        p->addIncoming(z, fastBB);
        p->addIncoming(s, slowBB);
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
        llvm::Value* isn = emitIsNum(bits);
        llvm::Value* isTrue = b.CreateICmpEQ(bits, ci64(val::kTrue));
        BasicBlock* numBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        // isNum → fcmp one; それ以外は kTrue||helper（kFalse は
        // helper でも false だが short-circuit で節約）
        llvm::Value* goNum = b.CreateOr(isn, isTrue);
        b.CreateCondBr(goNum, numBB, slowBB);
        b.SetInsertPoint(numBB);
        llvm::Value* tn = b.CreateOr(
            isTrue,
            b.CreateFCmpONE(emitAsF64(bits), cf64(0.0)));
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
    static constexpr uint32_t kListLenOff = uint32_t(offsetof(SList, len));
    static constexpr uint32_t kListCapOff = uint32_t(offsetof(SList, cap));
    static constexpr uint32_t kListDirtyOff =
        uint32_t(offsetof(SList, monitorDirty));
    static constexpr uint32_t kListItemsOff =
        uint32_t(offsetof(SList, items));

    /// idx(Value) → idx 由来の double を dBB へ feed する分岐を emit。
    /// num → asF64、numState∈{1,3,4} の heap str → numCache を使う。
    /// それ以外（未parse str/NaN str/'last'/'random'/bool/color/""）は
    /// slowBB へ。戻り値は dBB の f64 phi（呼出側で addIncoming）。
    std::pair<BasicBlock*, PHINode*> emitListIdxAsF64(
            llvm::Value* idx, BasicBlock* slowBB) {
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

    llvm::Value* emitListItem(Block* blk, const BlockField* f) {
        llvm::Value* idx = emitInputOf(blk, "INDEX");
        llvm::Value* l = varListPtr(f);   // null 可（list 非保有）
        llvm::Value* nonNull = b.CreateICmpNE(
            b.CreatePtrToInt(l, i64t), ci64(0));
        BasicBlock* haveBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* fastBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* emptyBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* slowBB = BasicBlock::Create(lc, "", fn);
        BasicBlock* merge = BasicBlock::Create(lc, "", fn);
        b.CreateCondBr(nonNull, haveBB, slowBB);
        b.SetInsertPoint(haveBB);
        auto [dBB, d] = emitListIdxAsF64(idx, slowBB);
        b.SetInsertPoint(dBB);
        llvm::Value* len = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        // Ordered bounds BEFORE fptosi: NaN/Inf/huge values must not create
        // poison used by the branch. For positive d, trunc(d)-1 < len iff
        // 1 <= d < len+1. A u32 len and len+1 are exactly representable in f64.
        llvm::Value* upper = b.CreateFAdd(b.CreateUIToFP(len, f64t), cf64(1.0));
        llvm::Value* inb = b.CreateAnd(b.CreateFCmpOGE(d, cf64(1.0)),
                                       b.CreateFCmpOLT(d, upper));
        b.CreateCondBr(inb, fastBB, emptyBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* i0 = b.CreateSub(b.CreateFPToSI(d, i64t), ci64(1));
        llvm::Value* items = b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
        llvm::Value* v = b.CreateLoad(i64t, b.CreateGEP(i64t, items, i0));
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
        return phi;
    }

    void emitListReplace(Block* blk, const BlockField* f) {
        llvm::Value* idx = emitInputOf(blk, "INDEX");
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
        auto [dBB, d] = emitListIdxAsF64(idx, slowBB);
        b.SetInsertPoint(dBB);
        llvm::Value* len = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        // Same ordered guard as list reads; invalid numeric indices are no-op,
        // with INDEX and ITEM already evaluated in their original order above.
        llvm::Value* upper = b.CreateFAdd(b.CreateUIToFP(len, f64t), cf64(1.0));
        llvm::Value* inb = b.CreateAnd(b.CreateFCmpOGE(d, cf64(1.0)),
                                       b.CreateFCmpOLT(d, upper));
        b.CreateCondBr(inb, fastBB, merge);
        b.SetInsertPoint(fastBB);
        llvm::Value* i0 = b.CreateSub(b.CreateFPToSI(d, i64t), ci64(1));
        llvm::Value* items = b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
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

    /// data_addtolist — l!=null && len<cap なら store/len++/dirty のみ。
    /// null・満杯（要 ensure）は helper（SList 生成/realloc 込み）。
    /// par-for の本体では「base+iter 番目のスロット書き込み」に置き換える
    /// （len は並列領域の前に確保済み）。
    void emitListAdd(Block* blk, const BlockField* f) {
        if (parFor) {
            auto k = parFor->index.find(f->id);
            if (k != parFor->index.end()) {
                llvm::Value* item = emitInputOf(blk, "ITEM");
                llvm::Value* l = b.CreateLoad(
                    ptrt, b.CreateGEP(ptrt, parFor->lists, ci64(k->second)));
                llvm::Value* base = b.CreateLoad(
                    i64t, b.CreateGEP(i64t, parFor->bases, ci64(k->second)));
                llvm::Value* idx = b.CreateAdd(base, parFor->iter);
                callN("xyo_par_slot", voidt, {l, idx, item});
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
        llvm::Value* len = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListLenOff));
        llvm::Value* cap = b.CreateLoad(
            i32t, b.CreateConstGEP1_32(i8t, l, kListCapOff));
        b.CreateCondBr(b.CreateICmpULT(len, cap), fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        llvm::Value* items = b.CreateLoad(
            ptrt, b.CreateConstGEP1_32(i8t, l, kListItemsOff));
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
            if (!genericLiteralNum) literalNums[bits] = cast::toNumber(v);
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

    llvm::Value* emitInputBlock(const std::string& id) {
        Block* blk = getBlock(id);
        if (!blk) return ci64(Value::undefined().bits);
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
                // 手続き外/未知引数 — インタプリタ評価へフォールバック
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
            return emitCanon(b.CreateFAdd(
                b.CreateUnaryIntrinsic(Intrinsic::round, x),
                cf64(0.0)));
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
            int32_t fn = 0;
            auto it = ops.find(fname);
            if (it != ops.end()) fn = it->second;
            llvm::Value* x = emitToNumF64(emitInputOf(blk, "NUM"));
            llvm::Value* r = emitMathop(fn, x);
            // xyo_num is exactly Value::numCanonical. Keep its NaN/Inf/±0
            // semantics inline, and expose the numeric result to consumers so
            // immediate arithmetic need not re-test bool/string tags.
            return genericMathBox ? call1("xyo_num", i64t, r) : emitCanon(r);
        }

        // --- sensing 軽量 ---
        if (op == "sensing_timer") {
            llvm::Value* d = call1("xyo_timer", f64t, ctxArg);
            return call1("xyo_num", i64t, d);
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

        // --- 直接ヘルパ（レポータ）---
        if (op == "operator_letter_of") return emitLetterOf(blk);
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

    // --- スタック emit ------------------------------------------------------

    void emitStopped() {
        // stopScript で suspend しなかった場合の安全弁
        b.CreateRetVoid();
    }

    void emitChainBlocks(const std::vector<Block*>& chain) {
        for (size_t i = 0; i < chain.size(); ++i) {
            Block* blk = chain[i];
            if (fail) return;
            // 現在位置が既に終端済み（stop 等）なら残りは到達不能 — 打ち切る
            if (bbDone(b.GetInsertBlock())) return;
            if (!hoistedSetters.empty() && hoistedSetters.count(blk))
                continue;   // ループ外へホイスト済み
            emitStackBlock(blk);
        }
    }

    void emitStackBlock(Block* blk) {
        const std::string& op = blk->opcode;

        if (op == "control_if" || op == "control_if_else") {
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* isT = emitTruthyV(cond);
            BasicBlock* thenBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* elseBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            b.CreateCondBr(isT, thenBB, elseBB);
            b.SetInsertPoint(thenBB);
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) b.CreateBr(merge);
            b.SetInsertPoint(elseBB);
            if (op == "control_if_else") {
                const BlockInput* in = blk->inputs.find("SUBSTACK2");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) b.CreateBr(merge);
            b.SetInsertPoint(merge);
            return;
        }

        if (op == "control_repeat") {
            // for (i = toNum(TIMES); i >= 0.5; i--) { body; loopYield }
            llvm::Value* n = emitToNumF64(emitInputOf(blk, "TIMES"));
            // 本体が GPU 並列実行可能なら xyo_gpu_repeat へ委譲し、
            // 失敗時のみ従来の CPU ループへ進む分岐を emit する。
            const gpu::Program* gp = tryGpuLoop(blk);
            AllocaInst* iv = entryAlloca(f64t, 1, "rep_i");
            b.CreateStore(n, iv);
            // 本体先頭の不変 pen setter をループ外へ（本体が 0 回なら実行しない）
            emitHoistedSetters(hoistableSetters(blk),
                               b.CreateFCmpOGE(n, cf64(0.5)));
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            // 「カウンタ + append」ループなら par-for を試す（warp 実行中のみ）。
            // 失敗（0 返却）時は従来の CPU ループへ落ちる。
            const bool triedParFor = tryEmitParFor(blk, n, head, merge);
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
            llvm::Value* i = b.CreateLoad(f64t, iv);
            llvm::Value* go = b.CreateFCmpOGE(i, cf64(0.5));
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                llvm::Value* i2 = b.CreateLoad(f64t, iv);
                b.CreateStore(b.CreateFSub(i2, cf64(1.0)), iv);
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            return;
        }

        if (op == "control_forever") {
            // forever の本体は必ず 1 回は走るのでガード不要
            emitHoistedSetters(hoistableSetters(blk), nullptr);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            b.CreateBr(bodyBB);
            b.SetInsertPoint(bodyBB);
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                b.CreateBr(bodyBB);
            }
            b.SetInsertPoint(merge);
            return;
        }

        if (op == "control_while" || op == "control_repeat_until") {
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            // 不変性が証明された counted while/until は GPU 並列へ —
            // 境界は limitParent/limitInput が指す不変オペランドを
            // ループ前に一度だけ評価する。元の条件は CPU パスで
            // 変更なく再評価される。
            const gpu::ParallelLoop* pl = tryGpuLoopEx(blk);
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
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* t = emitTruthyV(cond);
            llvm::Value* go = op == "control_while"
                ? t : b.CreateNot(t);
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            {
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            return;
        }

        if (op == "control_for_each") {
            // for (i=1; i<=toNum(VALUE); i++) { var=i; body; loopYield }
            llvm::Value* n = emitToNumF64(emitInputOf(blk, "VALUE"));
            const gpu::Program* gp = tryGpuLoop(blk);
            AllocaInst* iv = entryAlloca(f64t, 1, "fe_i");
            b.CreateStore(cf64(1.0), iv);
            BasicBlock* head = BasicBlock::Create(lc, "", fn);
            BasicBlock* bodyBB = BasicBlock::Create(lc, "", fn);
            BasicBlock* merge = BasicBlock::Create(lc, "", fn);
            if (gp) {
                llvm::Value* done =
                    callN("xyo_gpu_repeat", i32t,
                          {ctxArg, gpuProgPtr(gp, blk->id), n});
                b.CreateCondBr(b.CreateICmpNE(done, ci32(0)), merge,
                               head);
            } else {
                b.CreateBr(head);
            }
            b.SetInsertPoint(head);
            llvm::Value* i = b.CreateLoad(f64t, iv);
            llvm::Value* go = b.CreateFCmpOLE(i, n);
            b.CreateCondBr(go, bodyBB, merge);
            b.SetInsertPoint(bodyBB);
            {
                const BlockField* f = findField(blk, "VARIABLE");
                if (f) {
                    llvm::Value* vv = call1("xyo_num", i64t, i);
                    emitVarSet(f, vv);
                }
                const BlockInput* in = blk->inputs.find("SUBSTACK");
                if (in) emitChainBlocks(chainOf(in->block));
            }
            if (!bbDone(b.GetInsertBlock())) {
                emitLoopYield();
                llvm::Value* i2 = b.CreateLoad(f64t, iv);
                b.CreateStore(b.CreateFAdd(i2, cf64(1.0)), iv);
                b.CreateBr(head);
            }
            b.SetInsertPoint(merge);
            return;
        }

        if (op == "control_all_at_once") {
            // TW irgen: "if 1 = 1" 相当 — サブスタックをそのまま実行
            const BlockInput* in = blk->inputs.find("SUBSTACK");
            if (in) emitChainBlocks(chainOf(in->block));
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
            llvm::Value* cond = emitInputOf(blk, "CONDITION");
            llvm::Value* t = emitTruthyV(cond);
            b.CreateCondBr(t, done, waitBB);
            b.SetInsertPoint(waitBB);
            emitLoopYield();
            b.CreateBr(head);
            b.SetInsertPoint(done);
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
            if (f) emitVarSet(f, emitInputOf(blk, "VALUE"));
            return;
        }
        if (op == "data_changevariableby") {
            const BlockField* f = findField(blk, "VARIABLE");
            if (f) emitVarChange(f, emitInputOf(blk, "VALUE"));
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
        auto* pushI = llvm::cast<CallInst>(callN(
            "xyo_proc_push", voidt,
            {ctxArg, b.CreateGlobalString(blk->id),
             slotPtr(AotSlotKind::BlockCached, bcKey(blk->id),
                     blocks->getCachedExecute(blk->id)),
             ci32(warp ? 1 : 0)}));
        auto* callI = llvm::cast<CallInst>(b.CreateCall(pf, argv));
        auto* popI = llvm::cast<CallInst>(
            call1("xyo_proc_pop", voidt, ctxArg));
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

    /// leaf 手続きの呼出しフレーム push/pop を消す。
    /// 条件: 手続きが自身のコードで yield 可能・compat なヘルパを
    /// 呼ばない（fnDirect が空）。フレームの観測者 — yield 中の
    /// warpMode・compat op の読む stack・interp stepToProcedure の
    /// isRecursiveCall — はいずれも yield/compat コード経由でしか
    /// 到達しない。再帰手続きも対象: JIT の再帰検出は存在せず
    /// ネイティブ再帰の深さもフレーム有無で変わらない（interp 側
    /// 手続き呼出しは def ブロックを直接実行し pf を経由しない）。
    void elideLeafProcFrames() {
        if (procCallSites.empty()) return;
        for (auto& s : procCallSites) {
            auto dit = fnDirect.find(s.callee);
            if (dit != fnDirect.end() && dit->second) {
                if (verbose) std::fprintf(stderr,
                    "xyo-jit: frame keep (yield) %s\n",
                    s.proccode.c_str());
                continue;
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
        constexpr unsigned kMaxProcInsts = 220;
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
            if (it->second > kMaxProcInsts) continue;
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
            for (BasicBlock& bb : *f) {
                for (Instruction& instruction : bb) {
                    auto* load = dyn_cast<LoadInst>(&instruction);
                    if (!load) continue;
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
            if (loads.empty() || slotReload) continue;
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
            // Gather first, RAUW second, erase last: shared old GEP/load chains
            // may still feed other marked loads until all replacements exist.
            for (auto [load, index] : loads)
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
                   AotEmit* aot) {
    Codegen cg{lc, mod, IRBuilder<>(lc), runtime, pinned};
    cg.target = target;
    cg.blocks = blocks;
    cg.gpuProgs = &out.gpuProgs;
    cg.aot = aot;

    Block* top = blocks->getBlock(topBlockId);
    if (!top) return false;
    // ハット本体は emit しない（startHats が評価する）。
    // edge-activated hat はコンパイル対象外。
    if (runtime.getIsEdgeActivatedHat(top->opcode)) return false;

    std::vector<Block*> body = cg.chainOf(top->next);
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
    cg.emitChainBlocks(body);
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
