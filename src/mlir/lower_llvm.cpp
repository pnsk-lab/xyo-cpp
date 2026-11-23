// xyo-cpp: MLIR 風 IR の LLVM lowering。
//
// 生成する関数は既存 JIT と同じ ABI `void fn(JitCtx*)`。ランタイム
// ヘルパは jit/rt.cpp の extern "C" をそのまま呼ぶので、意味論は
// 既存コード生成と同一の関数に委譲される。
#include "lower_llvm.hpp"

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include "../jit/jit.hpp"
#include "../model/blocks.hpp"  // utf8ToU16
#include "../model/variable.hpp"

namespace xyo::mlir {
namespace {

// 実行時ヘルパ（src/jit/rt.cpp）をコンパイル時に呼ぶ。定数畳み込み用。
// passes.cpp の Folder と同じ関数・同じ意味論。
extern "C" double xyo_to_num(uint64_t);

using llvm::BasicBlock;
using llvm::IRBuilder;
using LValue = llvm::Value;

struct Key {
    const void* owner = nullptr;
    uint32_t index = 0;
    bool blockArg = false;
    bool operator<(const Key& o) const {
        if (owner != o.owner) return owner < o.owner;
        if (index != o.index) return index < o.index;
        return blockArg < o.blockArg;
    }
};

Key keyOf(const Value& v) {
    if (v.isBlockArg()) return Key{v.block, v.argIndex, true};
    return Key{v.op, v.index, false};
}

struct Slot {
    std::string id;
    std::string name;
    bool isList = false;
};

/// 関数をまたいで共有する lowering 状態（変数スロット・グローバル文字列）。
/// スロット番号はスクリプト全体で一意で、初期化は entry 関数の先頭で 1 回。
struct Shared {
    /// 常に数値と証明できた変数 id（frontend が設定）。
    const std::unordered_set<std::string>* numericVars = nullptr;
    std::map<std::string, uint32_t> slotIndex;
    std::vector<Slot> slotDefs;
    std::map<std::string, llvm::Constant*> globals;
    /// compat 層 / clone 系ヘルパを emit した関数を含むモジュールは
    /// 呼出し側スレッドの affinity が要る（既存 codegen の mainThread
    /// マークと同じ規則）。entry 以外の手続き関数でも伝播する。
    bool mainThread = false;
};

/// xyo.const.f64 の定数値。
bool constF64Of(const Value& v, double* out) {
    if (!v.op || v.op->name != "xyo.const.f64") return false;
    const Attr* a = v.op->attr("d");
    if (!a || !a->isDouble()) return false;
    *out = a->d;
    return true;
}
struct Lowerer {
    Lowerer(llvm::Module& module, llvm::LLVMContext& context, Shared& shared_)
        : mod(module), lc(context), b(context), shared(shared_) {}

    llvm::Module& mod;
    llvm::LLVMContext& lc;
    IRBuilder<> b;
    llvm::Function* fn = nullptr;
    /// reporter 手続きは第 1 引数が戻り値スロットなので、xyo.arg の基点が 1 ずれる。
    unsigned argBase = 1;
    /// 変数スロット（Variable*）は 1 回の実行中ずっと同じなので、関数の
    /// entry で 1 回だけロードして使い回す（毎アクセス 2 回の余分な
    /// 依存ロードを消す）。
    std::map<std::string, LValue*> varPtrCache_;
    /// 値ごとの「toNumber(v) を f64 SSA にしたもの」。算術/比較を
    /// ランタイム呼出しではなく素の f64 演算へ落とすために使う。
    /// 値ごとの f64 影。**同じ基本ブロック内でのみ**再利用する
    /// （別ブロックで作った SSA 値は支配関係を満たさないことがある）。
    struct NumShadowEntry {
        LValue* value = nullptr;
        llvm::BasicBlock* block = nullptr;
    };
    std::map<Key, NumShadowEntry> numShadow_;
    /// スレッドの fiber は 1 回の実行中ずっと同じ。
    LValue* fiberCache_ = nullptr;
    /// xyo_list_limit（i32 の外部グローバル）。関数をまたいで 1 個。
    llvm::GlobalVariable* limitGlobal_ = nullptr;
    /// リスト格納モードが平坦系（extra を一切使わない）なら true。
    /// コンパイル時に確定するので毎アクセスの extra 検査を省ける。
    bool flatListStore_ = xyo_list_storage_mode <= 3;
    LValue* ctx = nullptr;
    std::map<Key, LValue*> vals;
    // 変数スロット: (id,name,isList) → idx。実体は Thread::jitVarSlots で、
    // エントリで xyo_varslot_init を 1 回だけ呼び、以後は
    // ctx->varSlotData[idx] を直接 load する（既存 codegen と同じ契約）。
    // スロット表は手続き関数と共有する（番号の意味を揃える）。
    Shared& shared;
    // warp ループ用の yield カウンタ（関数単位の alloca、mem2reg で
    // レジスタ化される）。ctx->yieldCheckN は関数脱出時に書き戻す。
    llvm::AllocaInst* yieldCnt_ = nullptr;
    // 関数の共通出口（warp 復帰・yield カウンタ書き戻し・ret）。stop 系の
    // 早期 return はここへ分岐する。必要なときだけ作る。
    llvm::BasicBlock* retBB_ = nullptr;
    std::string error;
    bool mayYield = false;
    bool voluntaryYield = false;
    /// スタックレス・コルーチン化（stackless::lower）が受理できる形で
    /// emit する。ループ yield は予算判定なしの直接 yield、待機系は
    /// begin/poll 形、内部で yield する helper は使わない。
    bool coroMode_ = false;
    /// この lowering 対象の関数が warp 手続きか（warp 内 yield を
    /// 協調点として !xyo.coop 印付けするために使う）。
    bool mfWarp_ = false;
    /// yielder: コルーチン化前提なので毎反復の直接 yield で良い。
    void emitCoroYield() {
        mayYield = true;
        voluntaryYield = true;
        if (!fiberCache_) {
            llvm::BasicBlock& fentry = fn->getEntryBlock();
            IRBuilder<> ib(&fentry, fentry.begin());
            fiberCache_ = ib.CreateLoad(
                ptr(), ib.CreateConstGEP1_32(
                           i8(), ctx, uint32_t(offsetof(JitCtx, fiber))));
        }
        b.CreateCall(callee("xyo_yield_fast", {ptr()}, b.getVoidTy()),
                     {fiberCache_});
    }
    /// コルーチン化前提の blocking helper 展開。扱えたら true。
    bool emitCoroBlockingDirect(std::string_view sym, Op& op);
    /// begin 済みの非同期処理が終わるまで poll するループ
    /// （legacy codegen の emitStepLoop と同じ形）。
    void emitCoroStepLoop(const char* stepSym) {
        mayYield = true;
        voluntaryYield = true;
        BasicBlock* head = BasicBlock::Create(lc, "step.head", fn);
        BasicBlock* checkOne = BasicBlock::Create(lc, "step.chk", fn);
        BasicBlock* yieldBB = BasicBlock::Create(lc, "step.yield", fn);
        BasicBlock* busyBB = BasicBlock::Create(lc, "step.busy", fn);
        BasicBlock* done = BasicBlock::Create(lc, "step.done", fn);
        b.CreateBr(head);
        b.SetInsertPoint(head);
        LValue* r = b.CreateCall(callee(stepSym, {ptr()}, i32()), {ctx});
        b.CreateCondBr(
            b.CreateICmpEQ(r, llvm::ConstantInt::get(i32(), 0)), done, checkOne);
        b.SetInsertPoint(checkOne);
        b.CreateCondBr(
            b.CreateICmpEQ(r, llvm::ConstantInt::get(i32(), 1)), yieldBB, busyBB);
        b.SetInsertPoint(yieldBB);
        emitCoroYield();
        b.CreateBr(head);
        b.SetInsertPoint(busyBB);
        b.CreateBr(head);
        b.SetInsertPoint(done);
    }

    llvm::BasicBlock* retBlock() {
        if (!retBB_) retBB_ = llvm::BasicBlock::Create(lc, "fn.ret", fn);
        return retBB_;
    }

    llvm::Type* i1() { return b.getInt1Ty(); }
    llvm::Type* i8() { return b.getInt8Ty(); }
    llvm::Type* i32() { return b.getInt32Ty(); }
    llvm::Type* i64() { return b.getInt64Ty(); }
    llvm::Type* f64() { return b.getDoubleTy(); }
    llvm::PointerType* ptr() { return llvm::PointerType::getUnqual(lc); }

    llvm::Type* ty(Type t) {
        switch (t.kind) {
        case TypeKind::F64: return f64();
        case TypeKind::Bool: return i1();
        case TypeKind::I64: return i64();
        case TypeKind::I32: return i32();
        case TypeKind::Ptr: return ptr();
        case TypeKind::Ctx: return ptr();
        case TypeKind::Value:
        default: return i64();
        }
    }

    llvm::FunctionCallee callee(const char* name,
                                std::vector<llvm::Type*> args,
                                llvm::Type* ret) {
        // 既存 codegen と同じ caller-affinity 規則: compat ヘルパは
        // 実行キャッシュや拡張プリミティブへ触れるので worker で
        // 走らせず、clone 生成/削除はスケジューラ/Target 配列を
        // 書き換えるので mainThread 固定にする。
        if (std::string_view(name).starts_with("xyo_rt_"))
            shared.mainThread = true;
        static const bool cloneSerialHint = [] {
            const char* v = std::getenv("XYO_CLONE_SERIAL_CODEGEN");
            return !v || v[0] != '0';
        }();
        if (cloneSerialHint &&
            (std::string_view(name) == "xyo_ctl_createclone" ||
             std::string_view(name) == "xyo_ctl_deleteclone" ||
             std::string_view(name) == "xyo_ctl_deleteclone_begin"))
            shared.mainThread = true;
        auto fc = mod.getOrInsertFunction(
            name, llvm::FunctionType::get(ret, args, false));
        // 純粋な値変換ヘルパは readonly/willreturn として印す。
        // ループ条件の xyo_to_num(arg) のような不変呼出しを LICM が
        // ホイストできるようになる（属性が無いと毎反復コールのまま）。
        std::string_view sv(name);
        if (sv == "xyo_to_num" || sv == "xyo_truthy") {
            if (llvm::Function* f =
                    llvm::cast<llvm::Function>(fc.getCallee())) {
                f->setDoesNotThrow();
                f->setWillReturn();
                f->setOnlyReadsMemory();
            }
        }
        return fc;
    }

    llvm::Value* ctxField(uint64_t off) {
        return b.CreateConstGEP1_32(i8(), ctx, static_cast<uint32_t>(off));
    }

    // --- グローバル文字列 -------------------------------------------
    llvm::Constant* idPtr(const std::string& id) {
        std::string key = "s:" + id;
        auto it = shared.globals.find(key);
        if (it != shared.globals.end()) return it->second;
        auto* gv = new llvm::GlobalVariable(
            mod, llvm::ArrayType::get(i8(), id.size() + 1), true,
            llvm::GlobalValue::PrivateLinkage, nullptr, ".xyo.varid");
        gv->setInitializer(llvm::ConstantDataArray::getString(lc, id, true));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        gv->setAlignment(llvm::Align(1));
        shared.globals[key] = gv;
        return gv;
    }

    llvm::Constant* namePtr(const std::string& utf8) {
        std::string key = "u:" + utf8;
        auto it = shared.globals.find(key);
        if (it != shared.globals.end()) return it->second;
        std::u16string u = utf8ToU16(utf8);
        u.push_back(u'\0');
        auto* gv = new llvm::GlobalVariable(
            mod, llvm::ArrayType::get(b.getInt16Ty(), u.size()), true,
            llvm::GlobalValue::PrivateLinkage, nullptr, ".xyo.varname");
        gv->setInitializer(llvm::ConstantDataArray::get(
            lc, llvm::ArrayRef<uint16_t>(
                    reinterpret_cast<const uint16_t*>(u.data()), u.size())));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        gv->setAlignment(llvm::Align(2));
        shared.globals[key] = gv;
        return gv;
    }

    LValue* mapValue(const Value& v, LValue* lv) {
        vals[keyOf(v)] = lv;
        return lv;
    }

    LValue* value(const Value& v) {
        if (v.type == Type::ctx() && ctx) return ctx;
        auto it = vals.find(keyOf(v));
        if (it == vals.end()) {
            error = "value not lowered: " +
                    (v.op ? v.op->name : std::string("(blockarg)"));
            return llvm::ConstantInt::get(i64(), 0);
        }
        LValue* lv = it->second;
        // 昇格済みの f64 値（xyo.list.item.num 等）が !xyo.value 期待の
        // オペランドに流れてきた場合は箱に戻す。生の store double は
        // NaN 正規化（canonical kNan）をスキップし、i64 ビット列として
        // 扱う箇所では `icmp slt double, i64` の不正 IR になる
        // （IPSCCP クラッシュの実績あり）。xyo.box.f と同じ形で
        // NaN は canonical 化する。
        if (lv && lv->getType() == f64() && v.type == Type::value()) {
            lv = b.CreateSelect(
                b.CreateFCmpUNO(lv, lv),
                llvm::ConstantInt::get(
                    i64(), static_cast<int64_t>(val::kNan)),
                b.CreateBitCast(lv, i64()));
        }
        // !xyo.bool 期待のオペランドに i64（定数畳み込みが作った
        // Value ボックスの kTrue/kFalse、または boolean 定数の raw 0/1）
        // が流れてきた場合は i1 に畳む。false と見なすのは 0 と
        // ボックス済み kFalse だけ — ボックス kTrue(0x7FF8...2) と
        // raw 1 の両 convention を正しく扱う。
        if (lv && lv->getType() == i64() && v.type == Type::boolean()) {
            LValue* nz =
                b.CreateICmpNE(lv, llvm::ConstantInt::get(i64(), 0));
            LValue* nf = b.CreateICmpNE(
                lv, llvm::ConstantInt::get(
                        i64(), static_cast<int64_t>(val::kFalse)));
            lv = b.CreateAnd(nz, nf);
        }
        return lv;
    }

    // --- 変数スロット -----------------------------------------------
    uint32_t slotFor(const Op& op, bool isList) {
        return slotFor(std::string(op.attrStr("id")),
                       std::string(op.attrStr("name")), isList);
    }

    uint32_t slotFor(const std::string& id, const std::string& name,
                     bool isList) {
        std::string key = id;
        key += '\x03';
        key += name;
        key += isList ? '\x01' : '\x02';
        auto it = shared.slotIndex.find(key);
        if (it != shared.slotIndex.end()) return it->second;
        const uint32_t idx = static_cast<uint32_t>(shared.slotDefs.size());
        shared.slotIndex.emplace(std::move(key), idx);
        shared.slotDefs.push_back({id, name, isList});
        return idx;
    }

    LValue* slotVariable(const Op& op, bool isList) {
        return slotVariable(std::string(op.attrStr("id")),
                            std::string(op.attrStr("name")), isList);
    }

    LValue* slotVariable(const std::string& id, const std::string& name,
                         bool isList) {
        std::string key = id;
        key += '\x03';
        key += name;
        key += isList ? '\x01' : '\x02';
        auto cached = varPtrCache_.find(key);
        if (cached != varPtrCache_.end()) return cached->second;
        const uint32_t idx = slotFor(id, name, isList);
        // entry の先頭でロードする（スロット初期化は lowering の最後に
        // さらに手前へ挿入されるので、必ず init の後になる）。
        // 注: !invariant.load は不採用 — sha256 で効かず bubblesort が
        // LLVM の remat 化で 39→46ms へ悪化した（実測で撤回）。
        llvm::BasicBlock& fentry = fn->getEntryBlock();
        IRBuilder<> ib(&fentry, fentry.begin());
        LValue* arr = ib.CreateLoad(
            ptr(), ib.CreateConstGEP1_32(
                       i8(), ctx, uint32_t(offsetof(JitCtx, varSlotData))));
        LValue* p = ib.CreateGEP(ptr(), arr,
                                 llvm::ConstantInt::get(i64(), idx));
        LValue* v = ib.CreateLoad(ptr(), p);
        varPtrCache_.emplace(std::move(key), v);
        return v;
    }

    /// エントリ先頭に xyo_varslot_init を挿入する（全使用点を支配する）。
    void emitSlotInits() {
        if (shared.slotDefs.empty()) return;
        llvm::BasicBlock& entry = fn->getEntryBlock();
        IRBuilder<> ib(&entry, entry.begin());
        auto f = callee("xyo_varslot_init", {ptr(), i64(), ptr(), ptr(), i32()},
                        b.getVoidTy());
        for (size_t i = 0; i < shared.slotDefs.size(); ++i) {
            const Slot& slot = shared.slotDefs[i];
            ib.CreateCall(f, {ctx, llvm::ConstantInt::get(i64(), i),
                              idPtr(slot.id), namePtr(slot.name),
                              llvm::ConstantInt::get(i32(),
                                                     slot.isList ? 1 : 0)});
        }
    }

    // --- region / 制御構文 -------------------------------------------
    void lowerBlock(Block& blk, BasicBlock* bb);

    void lowerRegionBody(Region* r, BasicBlock* bb,
                         std::vector<LValue*>* yielded) {
        if (!r || r->blocks.size() != 1) {
            error = "region must have exactly one block";
            return;
        }
        Block* body = r->entry();
        lowerBlock(*body, bb);
        if (yielded) {
            Op* term = body->lastOp();
            yielded->clear();
            if (term && term->name == "xyo.yield")
                for (const Value& v : term->operands)
                    yielded->push_back(value(v));
        }
    }

    void emitIf(Op& op);
    void emitWhile(Op& op);
    void emitRepeat(Op& op);
    void emitForever(Op& op);
    void emitCompat(Op& op, bool asValue);
    void emitAllAtOnce(Op& op);
    void emitForEach(Op& op);
    void emitStop(Op& op);
    void emitWaitUntil(Op& op);
    void emitLoopYield();
    /// warp ループのチャンク展開で、途中コピーの yield 検査を抑止する。
    /// true のとき xyo.warp_yield はオペランドをそのまま通す。
    bool suppressWarpYield_ = false;
    /// バッチループ: この warp_yield だけを検査せず通す（入れ子ループの
    /// 検査はそのまま残す）。チャンク展開の suppressWarpYield_ とは別。
    Op* suppressWarpYieldOp_ = nullptr;
    /// yield カウンタを 1 回の検査で何反復ぶん進めるか（チャンク幅）。
    int64_t warpChunkStep_ = 1;
    /// 実行時の反復数でカウンタを進める場合の上書き（バッチループ用）。
    LValue* warpStepOverride_ = nullptr;
    /// ループ不変なリスト読みの巻き上げ。
    /// 反復間で値が変わらない読み（contains / item # of）をループ前置へ
    /// 出し、yield（＝他スクリプトが走る可能性がある点）でのみ再評価する。
    /// これで「1 反復ごとの全走査」が「1 yield ごとの全走査」になる。
    struct HoistEntry {
        Op* op = nullptr;
        llvm::AllocaInst* slot = nullptr;
        LValue* var = nullptr;
        LValue* item = nullptr;
        const char* sym = nullptr;
    };
    /// ループ不変なリストヘッダ（平坦ストレージのみ）。
    /// items/len を前置で読み、yield 後に読み直す。alloca 経由なので
    /// mem2reg でレジスタ（phi）に昇格する。
    struct ListHeader {
        std::string id;
        llvm::Value* var = nullptr;
        llvm::AllocaInst* lpSlot = nullptr;
        llvm::AllocaInst* itemsSlot = nullptr;
        llvm::AllocaInst* lenSlot = nullptr;
        // rep.batch の外側ループで運ぶ SSA 値（バッチ境界でのみ更新）。
        // 設定されているときはスロットよりこちらを優先する。
        LValue* lpPhi = nullptr;
        LValue* itemsPhi = nullptr;
        LValue* lenPhi = nullptr;
    };
    /// to_num(getvar) のループ不変キャッシュ（ループ内で書かれない
    /// 変数）。preheader と yield 後の読み直しでだけ to_num する。
    struct NumCacheEntry {
        Op* gv = nullptr;              // xyo.getvar op
        LValue* var = nullptr;         // Variable*
        llvm::AllocaInst* slot = nullptr;  // f64 キャッシュ
    };
    struct LoopScope {
        llvm::BasicBlock* preheader = nullptr;
        std::unordered_set<Op*> bodyOps;
        std::unordered_set<std::string> mutatedLists;
    /// 構造変化（伸縮）のあるリスト。replace のみは含まない
    /// （ヘッダ共有可）。
    std::unordered_set<std::string> structMutatedLists;
        bool observable = false;
        std::vector<ListHeader> headers;
        std::vector<NumCacheEntry> numCache;
    };
    std::vector<LoopScope> loopScopes_;
    /// getvar op → 数値キャッシュ alloca（アクティブなスコープの和）。
    std::unordered_map<const Op*, llvm::AllocaInst*> loopNumCache_;
    /// xyo.arg op → 数値キャッシュ alloca（proc 引数は呼出し中不変 —
    /// 関数エントリで 1 回だけ to_num して使い回す）。
    std::unordered_map<const Op*, llvm::AllocaInst*> argNumCache_;
    void popLoopScope() {
        for (const NumCacheEntry& e : loopScopes_.back().numCache)
            loopNumCache_.erase(e.gv);
        loopScopes_.pop_back();
    }
    ListHeader* findListHeader(const std::string& id);
    /// リストポインタをロードする（ループ不変ヘッダがあればキャッシュから）。
    LValue* listPtrFor(Op& op, LValue* var, ListHeader** out);
    void readListHeader(const ListHeader& h);
    std::vector<HoistEntry> hoistStack_;
    llvm::AllocaInst* entryAlloca(llvm::Type* t, const char* name);
    void pushLoopScope(Block& body, bool warp, Block* extra = nullptr);
    void recomputeHoisted();
    bool tryHoistListRead(Op& op);
    /// チャンク分割された warp repeat の本体を発行する（64 反復 + tail）。
    bool emitChunkedRepeat(Op& op, LValue* remaining, Block* body,
                           const std::vector<LValue*>& initVals);
    /// warp ループを 64 反復のバッチに区切り、yield 検査をバッチ境界だけに
    /// 出す（本体は 1 回だけ lower する）。本体に入れ子ループが無いこと、
    /// xyo.warp_yield が 1 個で yield の先頭に整列していることが条件。
    bool emitBatchedRepeat(Op& op, LValue* remaining, Block* body,
                           const std::vector<LValue*>& initVals);
    /// warp_yield の検査だけを、現在値 cur を運搬値として発行する。
    std::vector<LValue*> emitWarpCheck(Op* wy,
                                       const std::vector<LValue*>& cur);
    LValue* numberShadow(const Value& v);
    /// i64 bits → f64 の inline toNum fast/slow（numberShadow 末尾と
    /// numCache 初期化で共用。挿入位置に fast/slow/join を生成する）。
    LValue* emitToNumInline(LValue* bits);
    /// 値 bits が「数値化キャッシュを持つ SStr」なら inline で数値を
    /// 読む（legacy と同形: タグ==0x7FF9 → numState(acquire)!=0 →
    /// state==2 ? 0 : numCache）。strBits は SStr* を指す生ビット。
    /// 非文字列・state==0 は missBB へ流す。fastEndOut に値確定ブロックを
    /// 返す（join 側 PHI の incoming 元）。
    LValue* numericStringFast(LValue* bits, BasicBlock* missBB,
                              BasicBlock* joinBB, BasicBlock** fastEndOut);
    /// !xyo.f64 オペランドの読み取り。生 NaN は toNumber 一致で 0 に
    /// 変換する（.f 演算子チェインでも数値境界の意味論を守る）。
    /// boxed な !xyo.value を f64 にする（既に f64 なら NaN 正規化だけ）。
    ///
    /// lowering が既に NaN 正規化済み（または非 NaN と判明）の f64
    /// だけ再検査を省く。.f 演算子・ループ運搬 arg は生 NaN を持ち得る
    /// （inf-inf 等 → interp は to_num で 0 に直すので store するまで
    /// shadow は NaN まま）。
    LValue* f64Operand(const Value& v) {
        LValue* d = value(v);
        if (!d || d->getType() == f64()) {
            if (!d) return d;
            if (v.op) {
                const std::string& pn = v.op->name;
                if (pn == "xyo.to_num" || pn == "xyo.const.f64")
                    return d;  // lowering が uno 正規化（または定数）済み
            }
            // numFacts で非 NaN が確実なら heal を省略する。
            // 特にループ運搬の 'd' 影は入口で正規化済みのとき本体では
            // 再評価する必要がなく、ループキャリー連鎖から ord+sel が消える。
            if (numFacts(v) & 1) return d;
            return b.CreateSelect(b.CreateFCmpUNO(d, d),
                                  llvm::ConstantFP::get(f64(), 0.0), d);
        }
        // ボックス値を数値へ。定数はコンパイル時に畳む。
        if (v.op && v.op->name == "xyo.const") {
            const Attr* a = v.op->attr("bits");
            if (a && a->isInt()) {
                const uint64_t cb = static_cast<uint64_t>(a->i);
                if (cb < static_cast<uint64_t>(val::kNan))
                    return llvm::ConstantFP::get(
                        f64(), xyo::Value{cb}.asNumberUnchecked());
                return llvm::ConstantFP::get(f64(), xyo_to_num(cb));
            }
        }
        LValue* num =
            b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {d});
        return b.CreateSelect(b.CreateFCmpUNO(num, num),
                              llvm::ConstantFP::get(f64(), 0.0), num);
    }
    /// リスト索引の生 f64。`xyo.box.f` producer は昇格変数の数値影なので
    /// bits 化（box + NaN 正規化）→ bitcast の往復を省いてオペランドを
    /// 直接使う。cvttsd2si は NaN に INT64_MIN を返すので範囲判定は同値。
    /// bits が必要な呼び出し側は自分で value() する（slow 経路のみに
    /// 置けば fast 側の box も消える）。
    LValue* indexF(const Value& v) {
        if (v.op && v.op->name == "xyo.box.f" && !v.op->operands.empty())
            return value(v.op->operands[0]);
        // 定数は bitcast ではなく to_num で畳む。文字列定数の bits は
        // タグ付きポインタで、bitcast→cvtt が巨大値に畳まれると
        // icmp ult が常に false に fold され fast path が消えて
        // slow 専用ループになる（実測で item 定数索引が 2.4×遅かった）。
        if (v.op && v.op->name == "xyo.const") {
            const Attr* a = v.op->attr("bits");
            if (a && a->isInt()) {
                const uint64_t cb = static_cast<uint64_t>(a->i);
                if (cb < static_cast<uint64_t>(val::kNan))
                    return llvm::ConstantFP::get(
                        f64(), xyo::Value{cb}.asNumberUnchecked());
                return llvm::ConstantFP::get(f64(), xyo_to_num(cb));
            }
        }
        return b.CreateBitCast(value(v), f64());
    }
    /// slow 経路（helper 呼出し）用の bits。`xyo.box.f` producer は
    /// op 本体を参照せず f64 から等価な箱化をここで emit する — これで
    /// hot 経路の `value()` 参照が無くなり、無条件に emit された box.f
    /// の命令列が LLVM DCE で消える。
    LValue* slowBits(const Value& v) {
        if (v.op && v.op->name == "xyo.box.f" && !v.op->operands.empty()) {
            LValue* d = value(v.op->operands[0]);
            return b.CreateSelect(
                b.CreateFCmpUNO(d, d),
                llvm::ConstantInt::get(
                    i64(), static_cast<int64_t>(val::kNan)),
                b.CreateBitCast(d, i64()));
        }
        return value(v);
    }

    // ---- f64 の値属性（non-NaN / finite）推論 ---------------------------
    // sel(uno(d),0,d) / box.f の canonical 化を静的に省く軽量解析。
    // ループ運搬の block argument は自分の更新値を経由して循環するので
    // 最大不動点として解く:
    //   (1) そのブロックの全 arg を楽観の 3 で初期化
    //   (2) arg の incoming（entry 値 + yield バックエッジ値）を
    //       evalFacts で評価して min を取る（arg は仮定値を返すだけで
    //       循環は即停止）。値は下がるのみなので 6 回で必ず安定する。
    // evalFacts は arg 以外を（非 arg の）再帰で普通に評価する。
    std::map<Key, int> numFactsArg_;        // block arg: 仮定 or 確定
    std::map<Key, char> numFactsSettled_;   // arg が確定済みか
    std::map<Key, const std::vector<const Value*>*> loopArgIns_;
    std::deque<std::vector<const Value*>> numFactsInsStore_;

    int evalFacts(const Value& v) {
        if (v.type != Type::f64()) return 0;
        if (!v.op) {
            // block arg: 仮定値（未解決なら楽観 3）。
            auto it = numFactsArg_.find(keyOf(v));
            if (it != numFactsArg_.end()) return it->second;
            return 3;
        }
        const Op& o = *v.op;
        const std::string& n = o.name;
        if (n == "xyo.const.f64") {
            const Attr* a = o.attr("d");
            const double dv = a ? a->asDouble(0.0) : 0.0;
            return dv != dv ? 0 : (std::isfinite(dv) ? 3 : 1);
        }
        if (n == "xyo.to_num" || n == "xyo.to_num.d") return 1;
        if (n == "xyo.list.item.num" || n == "xyo.list.item.num.i")
            return 1;  // タグ検査済み bitcast または to_num — 常に非 NaN
        if (n == "xyo.list.len" || n == "xyo.list.len.i") return 3;
        if (n == "xyo.add.f" || n == "xyo.sub.f") {
            const int a0 = evalFacts(o.operands[0]);
            const int c0 = evalFacts(o.operands[1]);
            int r = 0;
            if (((a0 & 1) && (c0 & 2)) || ((a0 & 2) && (c0 & 1)))
                r = ((a0 & 2) && (c0 & 2)) ? 3 : 1;
            if (std::getenv("XYO_MLIR_NUMDBG"))
                std::fprintf(stderr,
                             "[numfacts-eval] %s l=%s:%d r=%s:%d -> %d\n",
                             n.c_str(),
                             o.operands[0].op ? o.operands[0].op->name.c_str()
                                              : "arg",
                             a0,
                             o.operands[1].op ? o.operands[1].op->name.c_str()
                                              : "arg",
                             c0, r);
            return r;
        }
        if (n == "xyo.mul.f") {
            return (evalFacts(o.operands[0]) & 2) &&
                   (evalFacts(o.operands[1]) & 2) ? 3 : 0;
        }
        if (n == "xyo.div.f" || n == "xyo.mod.f") {
            const int a0 = evalFacts(o.operands[0]);
            const int c0 = evalFacts(o.operands[1]);
            if ((a0 & 2) && (c0 & 2)) {
                double cc = 0.0;
                if (constF64Of(o.operands[1], &cc) && cc != 0.0)
                    return n == "xyo.div.f" ? 3 : 1;
            }
            return 0;
        }
        if (n == "xyo.round.f" || n == "xyo.mathop.f")
            return evalFacts(o.operands[0]);
        if (n == "xyo.select" && o.operands.size() >= 3)
            return evalFacts(o.operands[1]) & evalFacts(o.operands[2]);
        if (n == "xyo.if") {
            int r = 3;
            for (auto& reg : o.regions) {
                if (!reg) continue;
                for (auto& blk : reg->blocks) {
                    Op* t = blk->lastOp();
                    if (!t || t->name != "xyo.yield") continue;
                    if (v.index < t->operands.size())
                        r &= evalFacts(t->operands[v.index]);
                    else
                        r = 0;
                }
            }
            return r;
        }
        if ((n == "xyo.warp_yield" || n == "xyo.loop_yield") &&
            v.index < o.operands.size())
            return evalFacts(o.operands[v.index]);
        if (n == "xyo.repeat" || n == "xyo.while" ||
            n == "xyo.repeat_until") {
            // ネストしたループの結果はその body arg の確定値（未解決
            // ならこの solve 経路では 0 — 保守的）。
            for (auto& rr : o.regions)
                if (rr && !rr->blocks.empty() &&
                    !rr->blocks[0]->args.empty()) {
                    Block* bd = rr->entry();
                    if (bd && v.index < bd->args.size()) {
                        Value a;
                        a.block = bd;
                        a.argIndex = v.index;
                        a.type = bd->args[v.index].type;
                        return numFacts(a);
                    }
                    break;
                }
            return 0;
        }
        return 0;
    }

    /// arg の確定属性。未登録なら対象ループの arg 群を不動点で解く。
    int numFactsBlockArg(const Value& v) {
        const Key k = keyOf(v);
        auto it = numFactsArg_.find(k);
        if (it != numFactsArg_.end() && numFactsSettled_.count(k))
            return it->second;
        auto ins = loopArgIns_.find(k);
        if (ins == loopArgIns_.end()) {
            numFactsArg_[k] = 0;
            numFactsSettled_[k] = 1;
            return 0;
        }
        // 同一ブロックの f64 arg をまとめて不動点で解く。
        std::vector<const Value*> sibArgs;
        if (v.block) {
            for (const Value& a : v.block->args)
                if (a.type == Type::f64() && loopArgIns_.count(keyOf(a)))
                    sibArgs.push_back(&a);
        }
        if (sibArgs.empty()) {
            numFactsArg_[k] = 0;
            numFactsSettled_[k] = 1;
            return 0;
        }
        for (const Value* a : sibArgs) {
            const Key ak = keyOf(*a);
            if (!numFactsSettled_.count(ak))
                numFactsArg_[ak] = 3;  // 楽観シード
        }
        // arg の incoming は arg ごと（entry 値 + その arg の yield 値）。
        for (int iter = 0; iter < 6; ++iter) {
            bool changed = false;
            for (const Value* a : sibArgs) {
                const Key ak = keyOf(*a);
                // 確定済み（markArgNonNaN で非 NaN を保証した値など）は
                // 不動点の対象外。ここで上書きすると書き込み側で正規化
                // した意味が消え、読み側の heal が復活する。
                if (numFactsSettled_.count(ak)) continue;
                int next = 3;
                for (const Value* in : *loopArgIns_[ak])
                    next &= evalFacts(*in);
                if (next < numFactsArg_[ak]) {
                    numFactsArg_[ak] = next;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        for (const Value* a : sibArgs)
            if (!numFactsSettled_.count(keyOf(*a)))
                numFactsSettled_[keyOf(*a)] = 1;
        if (std::getenv("XYO_MLIR_NUMDBG")) {
            std::fprintf(stderr, "[numfacts] %s arg #%u -> %d",
                         fn ? fn->getName().str().c_str() : "?",
                         v.argIndex, numFactsArg_[k]);
            for (const Value* a : sibArgs) {
                std::fprintf(stderr, " | arg%u ins:", a->argIndex);
                for (const Value* in : *loopArgIns_[keyOf(*a)])
                    std::fprintf(stderr, " %s:%d",
                                 in->op ? in->op->name.c_str() : "arg",
                                 evalFacts(*in));
                std::fprintf(stderr, " =>%d", numFactsArg_[keyOf(*a)]);
            }
            std::fprintf(stderr, "\n");
        }
        return numFactsArg_[k];
    }

    int numFacts(const Value& v) {
        if (!v.op) return numFactsBlockArg(v);
        return evalFacts(v);
    }

    /// ループ運搬の f64 引数を「非 NaN と確定」として登録する
    /// （書き込み側で正規化した場合だけ呼ぶ）。
    void markArgNonNaN(const Value& a) {
        numFactsArg_[keyOf(a)] = 1;
        numFactsSettled_[keyOf(a)] = 1;
    }

    /// ループ運搬値の NaN 正規化を**書き込み側**で行う。
    ///
    /// 読み側で `select(uno(d), 0, d)` すると vcmpordsd(4cy) + blend(1) +
    /// vaddsd(4) がループキャリーの依存連鎖になり、リスト読みのループが
    /// legacy の 2.7 倍遅かった（実測 5.2ns/反復 vs 1.9ns）。稀な NaN だけ
    /// 別ブロックへ分岐させれば、連鎖は fadd(4cy) だけで済む（legacy と
    /// 同じ形 — `vucomisd; jp` は予測でほぼ無料）。
    ///
    /// 正規化値は 0.0 でよい: 影が NaN なら対応する bits は box.f が
    /// 作った正準 NaN で、to_num(正準 NaN) = 0 だから。
    LValue* healCarryOnWrite(LValue* raw) {
        if (!raw || raw->getType() != f64()) return raw;
        BasicBlock* cur = b.GetInsertBlock();
        BasicBlock* fix = BasicBlock::Create(lc, "carry.fix", fn);
        BasicBlock* cont = BasicBlock::Create(lc, "carry.next", fn);
        b.CreateCondBr(
            b.CreateFCmpUNO(raw, raw), fix, cont,
            llvm::MDNode::get(lc, {
                llvm::MDString::get(lc, "branch_weights"),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64(), 4095))}));
        b.SetInsertPoint(fix);
        b.CreateBr(cont);
        b.SetInsertPoint(cont);
        auto* p = b.CreatePHI(f64(), 2);
        p->addIncoming(raw, cur);
        p->addIncoming(llvm::ConstantFP::get(f64(), 0.0), fix);
        return p;
    }

    /// ループ外・バッチ境界用の安価な正規化（select 1 個）。
    LValue* healOnce(LValue* v) {
        if (!v || v->getType() != f64()) return v;
        return b.CreateSelect(b.CreateFCmpUNO(v, v),
                              llvm::ConstantFP::get(f64(), 0.0), v);
    }

    /// body 引数 i が「(bits, f64) ペア運搬の f64 側」か（kinds='b'）。
    /// 'b' は value → f64 の順に 2 引数、'd' は f64 単独、'i' は整数単独。
    /// 'b' なら bits は box.f 由来で常に正準なので、影だけ書き込み側で
    /// 正規化してよい。
    static bool isPairCarry(const Block& body, size_t i) {
        return i > 0 && i < body.args.size() &&
               body.args[i].type == Type::f64() &&
               body.args[i - 1].type == Type::value();
    }
    /// 整数ループの reload 用: 素の double ビットなら直接 fptosi、
    /// 正準形の整数文字列などは to_num 経由（呼び出し側が整数値を保証）。
    /// to_num 呼び出しは低速側にだけ残る。
    LValue* toIntChecked(LValue* bits, llvm::Type* destTy) {
        BasicBlock* fast = BasicBlock::Create(lc, "toint.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "toint.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "toint.join", fn);
        LValue* limit = llvm::ConstantInt::get(
            i64(), static_cast<int64_t>(val::kNan));
        b.CreateCondBr(b.CreateICmpSLT(bits, limit), fast, slow,
            llvm::MDNode::get(lc, {llvm::MDString::get(lc, "branch_weights"),
                                   llvm::ConstantAsMetadata::get(
                                       llvm::ConstantInt::get(i64(), 63)),
                                   llvm::ConstantAsMetadata::get(
                                       llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* f = b.CreateFPToSI(b.CreateBitCast(bits, f64()), destTy);
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* s = b.CreateFPToSI(
            b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {bits}), destTy);
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(destTy, 2);
        phi->addIncoming(f, fastEnd);
        phi->addIncoming(s, slowEnd);
        return phi;
    }
    /// オペランドがコンパイル時に確定した有限 double なら返す。
    bool finiteConst(const Value& v, double* out) const {
        if (!v.op || v.op->name != "xyo.const") return false;
        const uint64_t bits = static_cast<uint64_t>(v.op->attrInt("bits"));
        if (bits >= static_cast<uint64_t>(val::kNan)) return false;
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        if (!std::isfinite(d)) return false;
        *out = d;
        return true;
    }
    /// f64 入力の Scratch mod: 両辺が正の i32 整数なら UREM32、
    /// それ以外は fmod+符号補正。|c| が 2 の冪定数なら c*trunc(x/c)
    /// 置き換え（除数定数は呼び出し側が hasDiv/dc で渡す）。
    LValue* emitModF64(LValue* a, LValue* c, bool hasDiv, double dc);
    /// 動的除数の mod の共有 slow 側。モジュールに 1 個だけ作る
    /// noinline/cold の f64(f64,f64) — ±2^53 内の非零整数なら i64
    /// SREM+符号補正、それ以外は fmod+符号補正（legacy
    /// modWideVariableFn と同じ）。
    llvm::Function* modWideVariableFn();
    void emitWarpLoopYield();
    /// warp ループの「本当に yield が要るか」検査を emit し、
    /// 要るときは handBB へ、要らなければ done へ分岐する。
    /// legacy codegen の emitWarpDeadlineCheck と同じ形:
    ///  - 実行時 isWarp==0 なら無条件で handoff（非 warp 実行は毎反復
    ///    ではなくこの検査間隔ごとに譲る契約 = xyo_loop_yield と同値）
    ///  - ctx->sampledWarpClockUs が生きていればアトミック 2 ロード +
    ///    比較だけ（呼出しゼロ）。watchdog が既に時計を標本化している
    ///    ため同期ヘルパを呼ぶ必要がない
    ///  - なければ xyo_warp_yield_due（GC 予算 + isStuck の同期検査）
    /// warpKnown は「呼び出し側が既に isWarp!=0 を確認済み」のとき
    /// 先頭の検査を省く。
    /// 戻り値は「譲らない」側で done へ分岐する直前の基本ブロック
    ///（done の PHI incoming は due 元のブロックではなくこちらを使う）。
    BasicBlock* emitWarpDueCheck(BasicBlock* handBB, BasicBlock* done,
                                 bool warpKnown);
    /// warp 本体の「値をすべて定数ステップで更新するだけ」の判定結果。
    struct ChunkInfo {
        std::map<uint32_t, double> steps;
        std::vector<std::string> ids;
        std::vector<std::string> names;
        bool ok = false;
    };
    ChunkInfo analyzeChunk(Block& body);
    std::vector<LValue*> spillAndYield(const std::vector<std::string>& ids,
                                       const std::vector<std::string>& names,
                                       const std::vector<LValue*>& values);
    /// Value ビットのまま spill し、reload したビットを返す（正確）。
    std::vector<LValue*> spillAndYieldBits(
        const std::vector<std::string>& ids,
        const std::vector<std::string>& names,
        const std::vector<LValue*>& bits);
    void emitChunkedRepeat(Op& op, LValue* count, const ChunkInfo& info);
    llvm::AllocaInst* yieldCounter();
    void emitOp(Op& op);
};

namespace {
/// 「素の double」判定に使うビット境界（Value のタグ域は kNan 以上）。
constexpr int64_t kNumBitsLimit = static_cast<int64_t>(val::kNan);
}  // namespace

namespace {
bool isPow2(double d);
}

/// f64 版の Scratch mod（xyo.mod と xyo.mod.f の共有実装）。
/// |c| が 2 の冪の定数なら x - c*trunc(x/c)、そうでなければ両辺が
/// 正の i32 整数のとき UREM32（fmod libcall を避ける）、どちらも
/// 外れるときだけ frem+fdiv+select の符号補正を使う。
LValue* Lowerer::emitModF64(LValue* a, LValue* c, bool hasDiv,
                            double dc) {
    LValue* r = nullptr;
    if (hasDiv && isPow2(dc) && std::fabs(dc) >= 1.0) {
        LValue* q = b.CreateFMul(
            a, llvm::ConstantFP::get(f64(), 1.0 / dc));
        LValue* t = b.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, q);
        LValue* r0 = b.CreateFSub(a, b.CreateFMul(c, t));
        // fmod(n,c) は r==0 のとき符号を n に合わせる（cutform では
        // 必ず +0 になるため。-0.0 入力と c の整数倍の両方で必要）。
        LValue* r0IsZero =
            b.CreateFCmpOEQ(r0, llvm::ConstantFP::get(f64(), 0.0));
        LValue* signedZero = b.CreateSelect(
            b.CreateFCmpOLT(a, llvm::ConstantFP::get(f64(), 0.0)),
            llvm::ConstantFP::get(f64(), -0.0),
            llvm::ConstantFP::get(f64(), 0.0));
        LValue* isNegZeroIn = b.CreateICmpEQ(
            b.CreateBitCast(a, i64()),
            llvm::ConstantInt::get(i64(), 0x8000000000000000ULL));
        signedZero = b.CreateSelect(isNegZeroIn, a, signedZero);
        r0 = b.CreateSelect(r0IsZero, signedZero, r0);
        // 補正の符号判定 r0/c も乗算で足りる（1/dc が表現できる場合。
        // 最小 denormal 等は除算に残す）。
        LValue* qc = (std::isfinite(1.0 / dc))
                         ? b.CreateFMul(
                               r0, llvm::ConstantFP::get(f64(), 1.0 / dc))
                         : b.CreateFDiv(r0, c);
        LValue* neg =
            b.CreateFCmpOLT(qc, llvm::ConstantFP::get(f64(), 0.0));
        return b.CreateSelect(neg, b.CreateFAdd(r0, c), r0);
    }
    // 非 2 冪除数: 呼出し側には「両辺が正の i32 整数 → UREM32」の
    // fast path だけ残し、それ以外は __xyo_mod_var_wide（noinline/cold）
    // へ譲る（legacy の emitArithNumeric AOp::Mod と同じ形。sha256 の
    // mod 2^32 連打で inline 幅広分岐が呼出し側を圧迫していた）。
    BasicBlock* fastBB = BasicBlock::Create(lc, "mod.intfast", fn);
    BasicBlock* wideBB = BasicBlock::Create(lc, "mod.wide", fn);
    BasicBlock* joinBB = BasicBlock::Create(lc, "mod.join", fn);
    auto isIntVal = [&](LValue* x) {
        LValue* t = b.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
        return b.CreateAnd(b.CreateFCmpORD(x, x), b.CreateFCmpOEQ(t, x));
    };
    LValue* aOk = b.CreateAnd(
        b.CreateAnd(
            b.CreateFCmpOGT(a, llvm::ConstantFP::get(f64(), 0.0)),
            b.CreateFCmpOLE(a,
                            llvm::ConstantFP::get(f64(), 2147483647.0))),
        isIntVal(a));
    LValue* bOk = b.CreateAnd(
        b.CreateAnd(
            b.CreateFCmpOGE(c, llvm::ConstantFP::get(f64(), 1.0)),
            b.CreateFCmpOLE(c,
                            llvm::ConstantFP::get(f64(), 2147483647.0))),
        isIntVal(c));
    b.CreateCondBr(b.CreateAnd(aOk, bOk), fastBB, wideBB,
                   llvm::MDNode::get(
                       lc, {llvm::MDString::get(lc, "branch_weights"),
                            llvm::ConstantAsMetadata::get(
                                llvm::ConstantInt::get(i64(), 48)),
                            llvm::ConstantAsMetadata::get(
                                llvm::ConstantInt::get(i64(), 16))}));
    b.SetInsertPoint(fastBB);
    LValue* ai = b.CreateFPToUI(a, i32());
    LValue* bi = b.CreateFPToUI(c, i32());
    LValue* fastR = b.CreateUIToFP(b.CreateURem(ai, bi), f64());
    BasicBlock* fastEnd = b.GetInsertBlock();
    b.CreateBr(joinBB);
    b.SetInsertPoint(wideBB);
    LValue* wideR =
        b.CreateCall(modWideVariableFn(), {a, c});
    BasicBlock* wideEnd = b.GetInsertBlock();
    b.CreateBr(joinBB);
    b.SetInsertPoint(joinBB);
    auto* phi = b.CreatePHI(f64(), 2);
    phi->addIncoming(fastR, fastEnd);
    phi->addIncoming(wideR, wideEnd);
    r = phi;
    return r;
}

/// 動的除数 mod の共有 slow 関数（legacy modWideVariableFn 相当）。
llvm::Function* Lowerer::modWideVariableFn() {
    constexpr const char* name = "__xyo_mod_var_wide";
    if (auto* existing = mod.getFunction(name)) return existing;
    auto* f = llvm::Function::Create(
        llvm::FunctionType::get(f64(), {f64(), f64()}, false),
        llvm::GlobalValue::InternalLinkage, name, mod);
    f->addFnAttr(llvm::Attribute::NoInline);
    f->addFnAttr(llvm::Attribute::Cold);
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
            ir.CreateFCmpOGT(
                v, llvm::ConstantFP::get(f64(), -4503599627370496.0)),
            ir.CreateFCmpOLT(
                v, llvm::ConstantFP::get(f64(), 4503599627370496.0)));
    };
    llvm::Value* range = ir.CreateAnd(
        ir.CreateAnd(bounded(fa), bounded(fb)),
        ir.CreateAnd(
            ir.CreateFCmpONE(fa, llvm::ConstantFP::get(f64(), 0.0)),
            ir.CreateFCmpONE(fb, llvm::ConstantFP::get(f64(), 0.0))));
    ir.CreateCondBr(range, check, slow);
    ir.SetInsertPoint(check);
    llvm::Value* ni = ir.CreateFPToSI(fa, i64());
    llvm::Value* di = ir.CreateFPToSI(fb, i64());
    llvm::Value* ints = ir.CreateAnd(
        ir.CreateFCmpOEQ(ir.CreateSIToFP(ni, f64()), fa),
        ir.CreateFCmpOEQ(ir.CreateSIToFP(di, f64()), fb));
    ir.CreateCondBr(ints, integer, slow);
    ir.SetInsertPoint(integer);
    llvm::Value* rem = ir.CreateSRem(ni, di);
    llvm::Value* adjust = ir.CreateAnd(
        ir.CreateXor(
            ir.CreateICmpSLT(rem, llvm::ConstantInt::get(i64(), 0)),
            ir.CreateICmpSLT(di, llvm::ConstantInt::get(i64(), 0))),
        ir.CreateICmpNE(rem, llvm::ConstantInt::get(i64(), 0)));
    llvm::Value* wrapped =
        ir.CreateSelect(adjust, ir.CreateAdd(rem, di), rem);
    llvm::Value* numeric = ir.CreateSIToFP(wrapped, f64());
    llvm::Value* negativeZero = ir.CreateAnd(
        ir.CreateICmpEQ(rem, llvm::ConstantInt::get(i64(), 0)),
        ir.CreateICmpSLT(ni, llvm::ConstantInt::get(i64(), 0)));
    ir.CreateRet(ir.CreateSelect(
        negativeZero, llvm::ConstantFP::get(f64(), -0.0), numeric));
    ir.SetInsertPoint(slow);
    llvm::Value* r0 = ir.CreateFRem(fa, fb);
    llvm::Value* signDiff = ir.CreateFCmpOLT(
        ir.CreateFDiv(r0, fb), llvm::ConstantFP::get(f64(), 0.0));
    ir.CreateRet(ir.CreateSelect(signDiff, ir.CreateFAdd(r0, fb), r0));
    return f;
}

llvm::AllocaInst* Lowerer::entryAlloca(llvm::Type* t, const char* name) {
    llvm::BasicBlock& entry = fn->getEntryBlock();
    IRBuilder<> ib(&entry, entry.begin());
    return ib.CreateAlloca(t, nullptr, name);
}

namespace {
/// ループ内にあって「他のスクリプトが走りうる」op か。true なら巻き上げ
/// したリスト読みは反復中に古くなる可能性があるので巻き上げない。
bool hoistBarrier(const std::string& n) {
    if (dialect::isPure(n) || dialect::isReadOnly(n)) return false;
    static const std::unordered_set<std::string> kSafe = {
        "xyo.setvar",   "xyo.getvar",    "xyo.if",      "xyo.while",
        "xyo.repeat",   "xyo.forever",   "xyo.yield",   "xyo.condition",
        "xyo.loop_yield", "xyo.warp_yield",
        // リストの読み書きは変数スロットや他スクリプトを触らない。
        "xyo.list.ptr", "xyo.list.item", "xyo.list.itemnum",
        "xyo.list.length", "xyo.list.contains", "xyo.list.contents",
        "xyo.list.add", "xyo.list.replace", "xyo.list.insert",
        "xyo.list.delete", "xyo.list.deleteall",
        // 昇格/融合で作られる変種（.num は f64 読み、.i は i32 索引）。
        "xyo.list.item.i", "xyo.list.item.num", "xyo.list.item.num.i",
        "xyo.list.replace.i",
    };
    return kSafe.count(n) == 0;
}

bool listMutator(const std::string& n) {
    return n == "xyo.list.add" || n == "xyo.list.replace" ||
           n == "xyo.list.insert" || n == "xyo.list.delete" ||
           n == "xyo.list.deleteall" || n == "xyo.list.replace.i";
}
/// 構造を変えるミューテータ（伸縮・再配置し得る）。replace は値だけを
/// 書き換え、SList*・len・items は不変なのでヘッダ共有できる。
bool structuralMutator(const std::string& n) {
    return n == "xyo.list.add" || n == "xyo.list.insert" ||
           n == "xyo.list.delete" || n == "xyo.list.deleteall";
}
}

namespace {
/// 書き込む Value bits が「必ず数値（素の double または正準 NaN 未満）」
/// と静的に証明できるか。真なら SList::numericOnly の更新を省いてよい
/// （数値の書き込みは numericOnly を 1→0 に変えない）。
bool guaranteedNumericBits(const Value& v) {
    if (!v.op) return false;
    const std::string& n = v.op->name;
    if (n == "xyo.from_int") return true;
    if (n == "xyo.const") {
        const Attr* a = v.op->attr("bits");
        if (a && a->isInt())
            return static_cast<uint64_t>(a->i) <
                   static_cast<uint64_t>(kNumBitsLimit);
        return false;
    }
    // 数値証明済みリストの読みは必ず素の double ビット。
    if ((n == "xyo.list.item" || n == "xyo.list.item.i") &&
        !v.op->operands.empty() && v.op->operands[0].op &&
        v.op->operands[0].op->name == "xyo.list.ptr" &&
        v.op->operands[0].op->attrInt("numeric") != 0)
        return true;
    return false;
}
}  // namespace

namespace {
/// リストヘッダのキャッシュは既定 on（XYO_MLIR_LISTHDR=0 で off）。
/// 読み専用ループの Variable.list/len/items を前置で読む。alloca 経由
/// のため mem2reg でレジスタに乗り、反復ごとの 3 連鎖ロードが消える。
/// yield 後の読み直しは実ハンドオフ時のみ。
bool listHeaderOptIn() {
    // 既定 on（XYO_MLIR_LISTHDR=0 で off）。書き込み側の item.i/
    // replace.i が Variable.list/len/items を反復ごとにロードする
    // ため、書き込みループ（bubblesort の swap 等）では hoist が効く。
    // 読み専用ループではほぼ中立（i32-1 運搬でロード自体が軽い）。
    static const bool on = [] {
        const char* e = std::getenv("XYO_MLIR_LISTHDR");
        return !e || e[0] != '0';
    }();
    return on;
}
}  // namespace

void Lowerer::pushLoopScope(Block& body, bool warp, Block* extra) {
    LoopScope sc;
    sc.preheader = b.GetInsertBlock();
    auto walkInto = [&](Block& blk) {
        std::function<void(Block&)> walk = [&](Block& cb) {
            for (Op* o : cb.ops) {
                sc.bodyOps.insert(o);
                if (listMutator(o->name) && !o->operands.empty() &&
                    o->operands[0].op &&
                    o->operands[0].op->name == "xyo.list.ptr")
                    sc.mutatedLists.insert(
                        std::string(o->operands[0].op->attrStr("id")));
                if (structuralMutator(o->name) && !o->operands.empty() &&
                    o->operands[0].op &&
                    o->operands[0].op->name == "xyo.list.ptr")
                    sc.structMutatedLists.insert(
                        std::string(o->operands[0].op->attrStr("id")));
                if (hoistBarrier(o->name)) sc.observable = true;
                for (auto& r : o->regions)
                    if (r)
                        for (auto& rcb : r->blocks) walk(*rcb);
            }
        };
        walk(blk);
    };
    walkInto(body);
    // while の cond 領域もループ内で毎反復評価されるため同じスコープに
    // 含める（cond でリストを読む/書くケースをヘッダ判定に反映する）。
    if (extra) walkInto(*extra);
    // 平坦ストレージでは extra が存在せず、リストを伸縮しないループなら
    // items/len は反復中ずっと同じ。前置で読んでおく（yield 後に読み直す）。
    // warp ループに限る: 非 warp は毎反復 yield し、他スクリプトが書き換
    // える度に読み直しが必要になってヘッダ化の意味が無いばかりか、読み直
    // し漏れがあれば古い配列を読む（list read memo の writer 混在ケース）。
    if (flatListStore_ && warp && !sc.observable && listHeaderOptIn()) {
        std::map<std::string, std::string> used;
        for (Op* o : sc.bodyOps) {
            const std::string& n = o->name;
            if (n != "xyo.list.item" && n != "xyo.list.item.i" &&
                n != "xyo.list.item.num" && n != "xyo.list.item.num.i" &&
                n != "xyo.list.replace" && n != "xyo.list.replace.i" &&
                n != "xyo.list.length")
                continue;
            if (o->operands.empty() || !o->operands[0].op) continue;
            Op* p = o->operands[0].op;
            if (p->name != "xyo.list.ptr") continue;
            std::string id(p->attrStr("id"));
            // replace のみのループは len/items が不変なので共有する。
            if (sc.structMutatedLists.count(id)) continue;
            used[id] = std::string(p->attrStr("name"));
        }
        for (auto& [id, name] : used) {
            LValue* var = slotVariable(id, name, true);
            if (!error.empty()) break;
            ListHeader h;
            h.id = id;
            h.var = var;
            h.lpSlot = entryAlloca(ptr(), "lst.lp");
            h.itemsSlot = entryAlloca(ptr(), "lst.items");
            h.lenSlot = entryAlloca(i32(), "lst.len");
            LValue* lp = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), var, uint32_t(offsetof(Variable, list))));
            b.CreateStore(lp, h.lpSlot);
            b.CreateStore(llvm::ConstantPointerNull::get(ptr()), h.itemsSlot);
            b.CreateStore(llvm::ConstantInt::get(i32(), 0), h.lenSlot);
            BasicBlock* have = BasicBlock::Create(lc, "lst.have", fn);
            BasicBlock* doneBB = BasicBlock::Create(lc, "lst.ok", fn);
            b.CreateCondBr(b.CreateIsNull(lp), doneBB, have);
            b.SetInsertPoint(have);
            b.CreateStore(
                b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                       i8(), lp, uint32_t(offsetof(SList, items)))),
                h.itemsSlot);
            b.CreateStore(
                b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                        i8(), lp, uint32_t(offsetof(SList, len)))),
                h.lenSlot);
            b.CreateBr(doneBB);
            b.SetInsertPoint(doneBB);
            sc.headers.push_back(h);
        }
    }
    // to_num(getvar) のループ不変キャッシュ。warp ループの 64 反復
    // バッチ中は同じ変数値を再変換しない（legacy の数値影に相当）。
    // ループ内で書かれる変数は対象外。xyo.call・wait 系を含むスコープは
    // observable で落ちる（hoistBarrier で変数スロットへの任意書き込み
    // を防ぐ）。yield 後は recomputeHoisted が読み直すので、他
    // スクリプトからの書き換えも legacy と同じ周期で反映される。
    static const bool noNumCache = [] {
        const char* e = std::getenv("XYO_MLIR_NONUMCACHE");
        return e && e[0] == '1';
    }();
    if (warp && !sc.observable && !noNumCache) {
        std::unordered_set<std::string> written;
        std::unordered_set<Op*> toNumIns;
        for (Op* o : sc.bodyOps) {
            if (o->name == "xyo.setvar" && !o->attrStr("id").empty())
                written.insert(std::string(o->attrStr("id")));
            // 昇格変数は yield 点でメモリを往復する（間の getvar は
            // 古い値を読み得る）のでキャッシュ対象から外す。
            if (o->name == "xyo.warp_yield" ||
                o->name == "xyo.loop_yield") {
                const std::string vlist(o->attrStr("vars"));
                size_t pos = 0;
                while (pos < vlist.size()) {
                    const size_t e = vlist.find('\x1e', pos);
                    const std::string ent =
                        vlist.substr(pos, e == std::string::npos
                                              ? std::string::npos
                                              : e - pos);
                    const size_t sep = ent.find('\x1f');
                    if (sep != std::string::npos && sep > 0)
                        written.insert(ent.substr(0, sep));
                    if (e == std::string::npos) break;
                    pos = e + 1;
                }
            }
            if ((o->name == "xyo.to_num" || o->name == "xyo.to_num.d") &&
                !o->operands.empty())
                toNumIns.insert(o->operands[0].op);
        }
        for (Op* o : sc.bodyOps) {
            if (!toNumIns.count(o) || loopNumCache_.count(o)) continue;
            LValue* bits = nullptr;
            LValue* var = nullptr;
            if (o->name == "xyo.getvar") {
                const std::string id(o->attrStr("id"));
                if (written.count(id)) continue;
                var = slotVariable(id, std::string(o->attrStr("name")),
                                   false);
                if (!error.empty()) break;
                bits = b.CreateLoad(
                    i64(),
                    b.CreateConstGEP1_32(
                        i8(), var, uint32_t(offsetof(Variable, value))));
            } else if (o->name == "xyo.arg") {
                // proc 引数は呼出し間で不変 — キャッシュ後は読み直し不要。
                bits = fn->getArg(argBase +
                                  static_cast<uint32_t>(o->attrInt("index")));
            } else {
                continue;
            }
            NumCacheEntry e;
            e.gv = o;
            e.var = var;
            e.slot = entryAlloca(f64(), "lnc.d");
            b.CreateStore(emitToNumInline(bits), e.slot);
            sc.numCache.push_back(e);
            loopNumCache_.emplace(o, e.slot);
        }
    }
    loopScopes_.push_back(std::move(sc));
}

LValue* Lowerer::listPtrFor(Op& op, LValue* var, ListHeader** out) {
    *out = nullptr;
    if (!op.operands.empty() && op.operands[0].op &&
        op.operands[0].op->name == "xyo.list.ptr") {
        const std::string id(op.operands[0].op->attrStr("id"));
        *out = findListHeader(id);
    }
    if (*out)
        return (*out)->lpPhi ? (*out)->lpPhi
                             : b.CreateLoad(ptr(), (*out)->lpSlot);
    return b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                  i8(), var, uint32_t(offsetof(Variable, list))));
}

Lowerer::ListHeader* Lowerer::findListHeader(const std::string& id) {
    for (auto it = loopScopes_.rbegin(); it != loopScopes_.rend(); ++it)
        for (auto& h : it->headers)
            if (h.id == id) return &h;
    return nullptr;
}

/// リストヘッダを現在のメモリから読み直す（yield 後）。
void Lowerer::readListHeader(const ListHeader& h) {
    LValue* lp = b.CreateLoad(
        ptr(), b.CreateConstGEP1_32(
                   i8(), h.var, uint32_t(offsetof(Variable, list))));
    b.CreateStore(lp, h.lpSlot);
    b.CreateStore(llvm::ConstantPointerNull::get(ptr()), h.itemsSlot);
    b.CreateStore(llvm::ConstantInt::get(i32(), 0), h.lenSlot);
    BasicBlock* have = BasicBlock::Create(lc, "lst.reload", fn);
    BasicBlock* doneBB = BasicBlock::Create(lc, "lst.reloaded", fn);
    b.CreateCondBr(b.CreateIsNull(lp), doneBB, have);
    b.SetInsertPoint(have);
    b.CreateStore(
        b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                               i8(), lp, uint32_t(offsetof(SList, items)))),
        h.itemsSlot);
    b.CreateStore(
        b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                i8(), lp, uint32_t(offsetof(SList, len)))),
        h.lenSlot);
    b.CreateBr(doneBB);
    b.SetInsertPoint(doneBB);
}


void Lowerer::recomputeHoisted() {
    // リストヘッダは他スレッドが伸縮し得るので yield のたびに読み直す。
    for (LoopScope& sc : loopScopes_) {
        for (const ListHeader& h : sc.headers) readListHeader(h);
        for (const NumCacheEntry& e : sc.numCache) {
            if (!e.var) continue;  // xyo.arg: 不変なので読み直し不要
            LValue* bits = b.CreateLoad(
                i64(), b.CreateConstGEP1_32(
                           i8(), e.var,
                           uint32_t(offsetof(Variable, value))));
            b.CreateStore(emitToNumInline(bits), e.slot);
        }
    }
    for (HoistEntry& e : hoistStack_) {
        if (!e.op || !e.var) continue;
        std::vector<LValue*> args{ctx, e.var};
        std::vector<llvm::Type*> sig{ptr(), ptr()};
        if (e.item) {
            args.push_back(e.item);
            sig.push_back(i64());
        }
        LValue* v = b.CreateCall(callee(e.sym, sig, i64()), args);
        b.CreateStore(v, e.slot);
    }
}

/// ループ不変なリスト検索を前置へ巻き上げる。成功したら true。
bool Lowerer::tryHoistListRead(Op& op) {
    static const bool dbg = std::getenv("XYO_MLIR_HOISTDBG") != nullptr;
    static const bool off = [] {
        const char* e = std::getenv("XYO_MLIR_NOHOIST");
        return e && e[0] == '1';
    }();
    if (off) return false;
    if (loopScopes_.empty() || op.operands.size() < 2) {
        if (dbg) std::fprintf(stderr, "[hoist] %s: no scope\n", op.name.c_str());
        return false;
    }
    const LoopScope& sc = loopScopes_.back();
    if (dbg)
        std::fprintf(stderr, "[hoist] %s: bodyOps=%zu mutated=%zu obs=%d\n",
                     op.name.c_str(), sc.bodyOps.size(),
                     sc.mutatedLists.size(), (int)sc.observable);
    if (!sc.preheader || sc.observable) {
        if (dbg) std::fprintf(stderr, "[hoist] %s: pre=%p obs=%d\n",
                              op.name.c_str(), (void*)sc.preheader,
                              (int)sc.observable);
        return false;
    }
    Op* ptrOp = op.operands[0].op;
    if (dbg) std::fprintf(stderr, "[hoist] cand %s ptr=%s\n", op.name.c_str(),
                          ptrOp ? ptrOp->name.c_str() : "(null)");
    if (!ptrOp || ptrOp->name != "xyo.list.ptr") return false;
    const std::string id(ptrOp->attrStr("id"));
    const std::string name(ptrOp->attrStr("name"));
    // 本体がこのリストを書き換えるなら反復中に結果が変わる。
    for (const LoopScope& s2 : loopScopes_)
        if (s2.mutatedLists.count(id)) return false;
    // 検索対象がループ内で定義された値なら反復ごとに変わる。
    const Value& needle = op.operands[1];
    if (!needle.op) return false;
    // 定数はループ内で生成されていても値は不変（前置で作り直す）。
    bool needleConst = false;
    if (sc.bodyOps.count(needle.op))
        needleConst = needle.op->name == "xyo.const" ||
                      needle.op->name == "xyo.const.f64";
    if (sc.bodyOps.count(needle.op) && !needleConst) return false;
    for (const HoistEntry& e : hoistStack_)
        if (e.op == &op) return true;  // 既に巻き上げ済み（同一 op の再訪）
    const char* sym = op.name == "xyo.list.itemnum" ? "xyo_list_itemnum"
                                                     : "xyo_list_contains";
    // 前置で必要な値を作る: 変数スロット（ループ不変）と検索対象。
    BasicBlock* bodyBB = b.GetInsertBlock();
    auto ip = b.saveIP();
    b.SetInsertPoint(sc.preheader->getTerminator());
    LValue* var = slotVariable(id, name, true);
    if (!error.empty()) {
        b.restoreIP(ip);
        return false;
    }
    LValue* item = nullptr;
    if (needleConst) {
        uint64_t bits = 0;
        if (needle.op->name == "xyo.const") {
            bits = static_cast<uint64_t>(needle.op->attrInt("bits"));
        } else {
            const Attr* a = needle.op->attr("d");
            if (!a || !a->isDouble()) {
                b.restoreIP(ip);
                return false;
            }
            bits = xyo::Value::numCanonical(a->d).bits;
        }
        item = llvm::ConstantInt::get(i64(), bits);
    } else {
        item = value(needle);
    }
    if (!item) {
        b.restoreIP(ip);
        return false;
    }
    HoistEntry e;
    e.op = &op;
    e.slot = entryAlloca(i64(), "xyo.hoist");
    e.var = var;
    e.item = item;
    e.sym = sym;
    b.CreateStore(b.CreateCall(callee(sym, {ptr(), ptr(), i64()}, i64()),
                               {ctx, var, item}),
                  e.slot);
    b.restoreIP(ip);
    b.SetInsertPoint(bodyBB);
    hoistStack_.push_back(e);
    vals[Key{&op, 0, false}] = b.CreateLoad(i64(), e.slot);
    return true;
}

LValue* Lowerer::numberShadow(const Value& v) {
    const Key key = keyOf(v);
    auto it = numShadow_.find(key);
    if (it != numShadow_.end() && it->second.block == b.GetInsertBlock())
        return it->second.value;
    // to_num(getvar/arg) で、その変数がループ内で書かれないときは
    // preheader で 1 回だけ求めたキャッシュ値を使う（legacy の昇格
    // 数値影に相当。yield 後は recomputeHoisted が読み直す）。
    if (v.op && (v.op->name == "xyo.getvar" || v.op->name == "xyo.arg")) {
        llvm::AllocaInst* cs = nullptr;
        auto cit = loopNumCache_.find(v.op);
        if (cit != loopNumCache_.end()) cs = cit->second;
        else {
            auto ait = argNumCache_.find(v.op);
            if (ait != argNumCache_.end()) cs = ait->second;
        }
        if (cs) {
            LValue* d = b.CreateLoad(f64(), cs);
            numShadow_[key] = NumShadowEntry{d, b.GetInsertBlock()};
            return d;
        }
    }
    // from_int（i32 範囲の正確な整数）は sitofp だけで数値になる。
    // タグ検査の分岐を出さない。
    if (v.op && v.op->name == "xyo.from_int" && !v.op->operands.empty()) {
        LValue* iv = value(v.op->operands[0]);
        if (iv) {
            LValue* d = b.CreateSIToFP(iv, f64());
            numShadow_[key] = NumShadowEntry{d, b.GetInsertBlock()};
            return d;
        }
    }
    // 数値リスト（numeric=1 — 全書き込みが数値と証明済み）の
    // list.item/.i 読みは要素が必ず素の double。タグ検査の分岐を出さず
    // bitcast + NaN 治療だけにする（bubble/quicksort 系で比較ごとの
    // fast/slow 分岐 1 個を消す）。item.num 相当の意味論 — 要素が
    // 文字列などの場合は numeric 証明が成立していないため起きない。
    // （書き込み証明をすり抜けた文字列は bitcast でゴミ double を読む
    // が、item.num 自体が同じ挙動なので差分は無い。）
    if (v.op && (v.op->name == "xyo.list.item" ||
                 v.op->name == "xyo.list.item.i") &&
        !v.op->operands.empty() && v.op->operands[0].op &&
        v.op->operands[0].op->name == "xyo.list.ptr" &&
        v.op->operands[0].op->attrInt("numeric") != 0) {
        LValue* bits = value(v);
        if (bits && bits->getType() == i64()) {
            LValue* f = b.CreateBitCast(bits, f64());
            LValue* nonNan = b.CreateSelect(
                b.CreateFCmpUNO(f, f),
                llvm::ConstantFP::get(f64(), 0.0), f);
            numShadow_[key] = NumShadowEntry{nonNan, b.GetInsertBlock()};
            return nonNan;
        }
    }
    // 常に数値と証明できた変数でもタグ検査は省けない。numericVars の
    // 証明は「プロジェクト内の全書き込みが数値を生成する」だけで、
    // 実行の合間にホスト（テスト/モニタ/未コンパイル経路）から直接
    // Variable.value を文字列・真偽値に書き換えられる — そうすると
    // bitcast がポインタビットを浮動小数と誤読し 0 になる（found:
    // nan codegen input>=19 の全失敗）。分岐 1 個を残して正しくする。
    if (false && shared.numericVars && v.op && v.op->name == "xyo.getvar" &&
        shared.numericVars->count(std::string(v.op->attrStr("id")))) {
        LValue* bits = value(v);
        if (!bits) {
            error = "numberShadow: numeric var not lowered";
            return nullptr;
        }
        LValue* f = b.CreateBitCast(bits, f64());
        LValue* nonNan = b.CreateSelect(b.CreateFCmpUNO(f, f),
                                        llvm::ConstantFP::get(f64(), 0.0), f);
        numShadow_[key] = NumShadowEntry{nonNan, b.GetInsertBlock()};
        return nonNan;
    }
    // f64 値（昇格後の影・畳み込んだ定数）はそのまま使う。
    // ただし NaN は toNumber に揃えて 0 に変換する。Value 経路では
    // box→canonical kNan→to_num=0 となるため、生 NaN を伝播させると
    // `sqrt(-1)+7` が 7 ではなく NaN になる（数値 NaN を 0 に正規化
    // するのはタグ域の Value だけではなく非正準 NaN も同様 — 既存の
    // fast path と同じ `uno→0` 変換を使う）。
    if (v.type == Type::f64()) {
        LValue* d = value(v);
        if (!d) {
            error = "numberShadow: operand has no lowered value";
            return nullptr;
        }
        // f64Operand と同じく、lowering が正規化済みの値だけ再検査を
        // 省く（.f 演算子・ループ arg は NaN を持ち得る）。
        bool clean = false;
        if (v.op) {
            const std::string& pn = v.op->name;
            clean = pn == "xyo.to_num" || pn == "xyo.const.f64";
        }
        // 非 NaN が証明できた値（書き込み側で正規化済みのループ運搬値など）
        // は再検査しない。ループキャリーに vcmpord+blend(5cy) が乗ると
        // リスト読みループが実測 2 倍遅くなる。
        if (!clean && (numFacts(v) & 1)) clean = true;
        if (!clean) {
            d = b.CreateSelect(b.CreateFCmpUNO(d, d),
                               llvm::ConstantFP::get(f64(), 0.0), d);
        }
        numShadow_[key] = NumShadowEntry{d, b.GetInsertBlock()};
        return d;
    }
    // 数値定数はコンパイル時に畳む（数値リテラルは文字列 Value で来る
    // ことがある。毎反復 xyo_to_num を呼ばない）。
    if (v.op && v.op->name == "xyo.const") {
        const Attr* a = v.op->attr("bits");
        if (a && a->isInt()) {
            const uint64_t cb = static_cast<uint64_t>(a->i);
            LValue* d;
            if (cb < static_cast<uint64_t>(kNumBitsLimit)) {
                d = llvm::ConstantFP::get(
                    f64(), xyo::Value{cb}.asNumberUnchecked());
            } else {
                d = llvm::ConstantFP::get(f64(), xyo_to_num(cb));
            }
            numShadow_[key] = NumShadowEntry{d, b.GetInsertBlock()};
            return d;
        }
    }
    LValue* bits = value(v);
    if (!bits) {
        error = "numberShadow: operand has no lowered value";
        return nullptr;
    }
    // オペランド型が !xyo.value でも、実際の lowering 結果が f64（
    // xyo.list.item.num 等の昇格済み数値）の場合がある。i64 ビット列
    // として扱うと `icmp slt double, i64` のような不正 IR を生成して
    // 後段の LLVM パス（IPSCCP）を落とすため、f64 として扱い、上と
    // 同じ `uno→0` の NaN 正規化だけを適用する。
    if (bits->getType() == f64()) {
        LValue* d = (numFacts(v) & 1)
                        ? static_cast<LValue*>(bits)
                        : static_cast<LValue*>(b.CreateSelect(
                              b.CreateFCmpUNO(bits, bits),
                              llvm::ConstantFP::get(f64(), 0.0), bits));
        numShadow_[key] = NumShadowEntry{d, b.GetInsertBlock()};
        return d;
    }
    static const bool noNumFast = [] {
        const char* e = std::getenv("XYO_MLIR_NONUMFAST");
        return e && e[0] == '1';
    }();
    if (noNumFast) {
        LValue* slow = b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {bits});
        numShadow_[key] = NumShadowEntry{slow, b.GetInsertBlock()};
        return slow;
    }
    LValue* phi = emitToNumInline(bits);
    numShadow_[key] = NumShadowEntry{phi, b.GetInsertBlock()};
    return phi;
}

LValue* Lowerer::emitToNumInline(LValue* bits) {
    LValue* limit = llvm::ConstantInt::get(i64(), kNumBitsLimit);
    // toNumberImmediate と同じ: 素の double はそのまま（NaN は 0）、
    // それ以外は ヒープ文字列だけ parse が必要 → runtime helper。
    BasicBlock* fast = BasicBlock::Create(lc, "num.fast", fn);
    BasicBlock* slow = BasicBlock::Create(lc, "num.slow", fn);
    BasicBlock* join = BasicBlock::Create(lc, "num.join", fn);
    b.CreateCondBr(b.CreateICmpSLT(bits, limit), fast, slow,
                   llvm::MDNode::get(lc, {llvm::MDString::get(lc, "branch_weights"),
                                          llvm::ConstantAsMetadata::get(
                                              llvm::ConstantInt::get(i64(), 63)),
                                          llvm::ConstantAsMetadata::get(
                                              llvm::ConstantInt::get(i64(), 1))}));
    b.SetInsertPoint(fast);
    LValue* f = b.CreateBitCast(bits, f64());
    LValue* nonNan = b.CreateSelect(b.CreateFCmpUNO(f, f),
                                    llvm::ConstantFP::get(f64(), 0.0), f);
    BasicBlock* fastEnd = b.GetInsertBlock();
    b.CreateBr(join);
    b.SetInsertPoint(slow);
    LValue* s = b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {bits});
    BasicBlock* slowEnd = b.GetInsertBlock();
    b.CreateBr(join);
    b.SetInsertPoint(join);
    auto* phi = b.CreatePHI(f64(), 2);
    phi->addIncoming(nonNan, fastEnd);
    phi->addIncoming(s, slowEnd);
    return phi;
}


llvm::AllocaInst* Lowerer::yieldCounter() {
    if (yieldCnt_) return yieldCnt_;
    llvm::BasicBlock& entry = fn->getEntryBlock();
    IRBuilder<> ib(&entry, entry.begin());
    yieldCnt_ = ib.CreateAlloca(i64(), nullptr, "xyo.yieldcnt");
    // 注意: ctxField() は本体ビルダ \(b\) を使うので、エントリ先頭に置く
    // 初期化は必ず ib で組み立てる（そうしないと定義前使用になる）。
    llvm::Value* counter = ib.CreateConstGEP1_32(
        i8(), ctx, uint32_t(offsetof(JitCtx, yieldCheckN)));
    ib.CreateStore(ib.CreateLoad(i64(), counter), yieldCnt_);
    return yieldCnt_;
}

llvm::BasicBlock* Lowerer::emitWarpDueCheck(BasicBlock* handBB,
                                            BasicBlock* done,
                                            bool warpKnown) {
    static const bool sharedWarpClock = [] {
        const char* env = std::getenv("XYO_JIT_WARP_CLOCK");
        return !env || env[0] != '0';
    }();
    BasicBlock* chkBB = BasicBlock::Create(lc, "wyield.chk", fn);
    if (!warpKnown) {
        LValue* warp = b.CreateLoad(
            i32(), ctxField(uint32_t(offsetof(JitCtx, isWarp))));
        b.CreateCondBr(b.CreateICmpEQ(warp, llvm::ConstantInt::get(i32(), 0)),
                       handBB, chkBB);
    } else {
        b.CreateBr(chkBB);
    }
    b.SetInsertPoint(chkBB);
    llvm::Value* should = nullptr;
    BasicBlock* mergeBB = BasicBlock::Create(lc, "wyield.merge", fn);
    if (sharedWarpClock) {
        // legacy の sampled warp clock: 呼出しを挟まずアトミックを 2 つ
        // 読んで期限と比較する。ポインタが無ければ同期ヘルパへ退く。
        llvm::Value* clockField = b.CreateConstGEP1_32(
            i8(), ctx, uint32_t(offsetof(JitCtx, sampledWarpClockUs)));
        llvm::Value* clockPtr = b.CreateLoad(ptr(), clockField);
        BasicBlock* sampled = BasicBlock::Create(lc, "wyield.sampled", fn);
        BasicBlock* fallback = BasicBlock::Create(lc, "wyield.fallback", fn);
        b.CreateCondBr(
            b.CreateICmpNE(clockPtr,
                           llvm::ConstantPointerNull::get(ptr())),
            sampled, fallback, llvm::MDNode::get(lc, {
                llvm::MDString::get(lc, "branch_weights"),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64(), 1000)),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64(), 1))}));

        b.SetInsertPoint(sampled);
        auto* timeUs = b.CreateLoad(i64(), clockPtr);
        timeUs->setAtomic(llvm::AtomicOrdering::Monotonic);
        llvm::Value* deadlineField = b.CreateConstGEP1_32(
            i8(), ctx, uint32_t(offsetof(JitCtx, sampledWarpDeadlineUs)));
        llvm::Value* deadlinePtr = b.CreateLoad(ptr(), deadlineField);
        auto* deadlineUs = b.CreateLoad(i64(), deadlinePtr);
        deadlineUs->setAtomic(llvm::AtomicOrdering::Monotonic);
        llvm::Value* expired = b.CreateICmpSGT(timeUs, deadlineUs);
        b.CreateBr(mergeBB);

        b.SetInsertPoint(fallback);
        LValue* due = b.CreateCall(
            callee("xyo_warp_yield_due", {ptr()}, i32()), {ctx});
        llvm::Value* oldExpired =
            b.CreateICmpNE(due, llvm::ConstantInt::get(i32(), 0));
        b.CreateBr(mergeBB);

        b.SetInsertPoint(mergeBB);
        auto* joined = b.CreatePHI(b.getInt1Ty(), 2);
        joined->addIncoming(expired, sampled);
        joined->addIncoming(oldExpired, fallback);
        should = joined;
    } else {
        LValue* due = b.CreateCall(
            callee("xyo_warp_yield_due", {ptr()}, i32()), {ctx});
        should = b.CreateICmpNE(due, llvm::ConstantInt::get(i32(), 0));
        b.CreateBr(mergeBB);
        b.SetInsertPoint(mergeBB);
    }
    b.CreateCondBr(should, handBB, done, llvm::MDNode::get(lc, {
        llvm::MDString::get(lc, "branch_weights"),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64(), 100000))}));
    return mergeBB;
}

void Lowerer::emitWarpLoopYield() {
    mayYield = true;
    llvm::AllocaInst* cnt = yieldCounter();
    LValue* n = b.CreateAdd(b.CreateLoad(i64(), cnt),
                            llvm::ConstantInt::get(i64(), 1));
    b.CreateStore(n, cnt);
    BasicBlock* dueBB = BasicBlock::Create(lc, "wyield.due", fn);
    BasicBlock* handBB = BasicBlock::Create(lc, "wyield.hand", fn);
    BasicBlock* done = BasicBlock::Create(lc, "wyield.done", fn);
    LValue* due = b.CreateICmpEQ(
        b.CreateAnd(n, llvm::ConstantInt::get(i64(), 63)),
        llvm::ConstantInt::get(i64(), 0));
    b.CreateCondBr(due, dueBB, done);
    b.SetInsertPoint(dueBB);
    // 64 反復ごとの検査は legacy と同じく「期限超過か」を見るだけにし、
    // 実際に譲るときだけ yield + hoist 再評価を行う。以前は検査のたびに
    // xyo_loop_yield（内部で時計を読む不透明呼出し）と recomputeHoisted
    // を無条件で走らせていた。
    emitWarpDueCheck(handBB, done, /*warpKnown=*/false);
    b.SetInsertPoint(handBB);
    b.CreateCall(callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
    recomputeHoisted();
    b.CreateBr(done);
    b.SetInsertPoint(done);
}

namespace {
/// 0 でも非有限でもない 2 の冪か。
bool isPow2(double d) {
    if (!(d == d) || !std::isfinite(d) || d == 0.0) return false;
    int e = 0;
    return std::fabs(std::frexp(d, &e)) == 0.5;
}
}  // namespace

bool Lowerer::emitCoroBlockingDirect(std::string_view sym, Op& op) {
    auto arg = [&](size_t i) {
        return i < op.operands.size() ? value(op.operands[i])
                                      : static_cast<LValue*>(nullptr);
    };
    auto callVoid = [&](const char* name, std::vector<llvm::Type*> sig,
                        std::vector<llvm::Value*> args) {
        b.CreateCall(callee(name, std::move(sig), b.getVoidTy()), args);
    };
    // glide: begin → step poll。
    if (sym == "xyo_mot_glidexy") {
        callVoid("xyo_mot_glidebegin_xy", {ptr(), i64(), i64(), i64()},
                 {ctx, arg(0), arg(1), arg(2)});
        emitCoroStepLoop("xyo_mot_glidestep");
        return true;
    }
    if (sym == "xyo_mot_glideto") {
        callVoid("xyo_mot_glidebegin_to", {ptr(), i64(), i64()},
                 {ctx, arg(0), arg(1)});
        emitCoroStepLoop("xyo_mot_glidestep");
        return true;
    }
    if (sym == "xyo_looks_switchbackdropwait") {
        callVoid("xyo_looks_backdropbegin", {ptr(), i64()}, {ctx, arg(0)});
        emitCoroStepLoop("xyo_looks_backdropstep");
        return true;
    }
    // 「begin して 1 回 yield」で足りるもの。
    if (sym == "xyo_looks_saysecs" || sym == "xyo_looks_thinksecs") {
        callVoid(sym == "xyo_looks_saysecs" ? "xyo_looks_saybegin"
                                            : "xyo_looks_thinkbegin",
                 {ptr(), i64(), i64()}, {ctx, arg(0), arg(1)});
        emitCoroYield();
        return true;
    }
    if (sym == "xyo_sns_askwait") {
        callVoid("xyo_sns_askbegin", {ptr(), i64()}, {ctx, arg(0)});
        emitCoroYield();
        return true;
    }
    if (sym == "xyo_snd_playwait") {
        callVoid("xyo_snd_playbegin", {ptr(), i64()}, {ctx, arg(0)});
        emitCoroYield();
        return true;
    }
    // sound の effect/volume: legacy は *_begin の戻り値が非 0 のときだけ
    // yield する（warp 中の連続設定で無駄に譲らないため）。
    if (sym == "xyo_snd_seteffect" || sym == "xyo_snd_changeeffect" ||
        sym == "xyo_snd_setvolume" || sym == "xyo_snd_changevolume") {
        const char* begin = sym == "xyo_snd_seteffect"
                                ? "xyo_snd_seteffect_begin"
                            : sym == "xyo_snd_changeeffect"
                                ? "xyo_snd_changeeffect_begin"
                            : sym == "xyo_snd_setvolume"
                                ? "xyo_snd_setvolume_begin"
                                : "xyo_snd_changevolume_begin";
        LValue* r = b.CreateCall(callee(begin, {ptr(), i64()}, i32()),
                                 {ctx, arg(0)});
        BasicBlock* yieldBB = BasicBlock::Create(lc, "snd.yield", fn);
        BasicBlock* cont = BasicBlock::Create(lc, "snd.cont", fn);
        b.CreateCondBr(
            b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0)),
            yieldBB, cont);
        b.SetInsertPoint(yieldBB);
        emitCoroYield();
        b.CreateBr(cont);
        b.SetInsertPoint(cont);
        return true;
    }
    return false;
}

void Lowerer::emitLoopYield() {
    // 1 回スケジューラへ譲る。xyo_loop_yield は warp 時の stuck 判定込み。
    // チャンク展開の途中コピーでは検査しない（最後のコピーが代表する）。
    if (suppressWarpYield_) return;
    // コルーチン化前提では予算判定を自前で持つ — legacy codegen の
    // stacklessNormalLoop と同じ形。非 warp は毎反復、warp は 64 反復に
    // 1 回だけ watchdog/GC 予算を見て、真なら直接 yield する
    // （xyo_loop_yield はコルーチン化で拒否されるので使わない）。
    if (coroMode_) {
        mayYield = true;
        // warp 手続き内の停止点は watchdog/GC 予算の協調点だけ —
        // 意味論 yield には数えない（legacy の fnSemanticYield /
        // cooperativeOnly と同じ扱いで同期バッチへ残す）。
        if (!mfWarp_) voluntaryYield = true;
        llvm::AllocaInst* counter = yieldCounter();
        LValue* n = b.CreateAdd(
            b.CreateLoad(i64(), counter),
            llvm::ConstantInt::get(i64(), warpChunkStep_));
        b.CreateStore(n, counter);
        if (!fiberCache_) {
            llvm::BasicBlock& fentry = fn->getEntryBlock();
            IRBuilder<> ib(&fentry, fentry.begin());
            fiberCache_ = ib.CreateLoad(
                ptr(), ib.CreateConstGEP1_32(
                           i8(), ctx, uint32_t(offsetof(JitCtx, fiber))));
        }
        LValue* warp = b.CreateLoad(
            i32(), ctxField(uint32_t(offsetof(JitCtx, isWarp))));
        BasicBlock* normal = BasicBlock::Create(lc, "yield.normal", fn);
        BasicBlock* warpBB = BasicBlock::Create(lc, "yield.warp", fn);
        BasicBlock* dueBB = BasicBlock::Create(lc, "yield.due", fn);
        BasicBlock* yieldBB = BasicBlock::Create(lc, "yield.susp", fn);
        BasicBlock* done = BasicBlock::Create(lc, "yield.done", fn);
        b.CreateCondBr(b.CreateICmpEQ(warp, llvm::ConstantInt::get(i32(), 0)),
                       normal, warpBB);
        b.SetInsertPoint(normal);
        // warp 手続き内のループでは isWarp が常に 1 なので、この枝は
        // 静的に unreachable。それでも coro の停止点として置く
        // （legacy も同じ形で、両方の停止点を xyo_yield* 呼出しにする）。
        b.CreateCall(callee("xyo_yield_fast", {ptr()}, b.getVoidTy()),
                     {fiberCache_});
        b.CreateBr(done);
        b.SetInsertPoint(warpBB);
        LValue* sampled = b.CreateICmpEQ(
            b.CreateAnd(n, llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantInt::get(i64(), 0));
        b.CreateCondBr(sampled, dueBB, done);
        b.SetInsertPoint(dueBB);
        LValue* due = b.CreateCall(
            callee("xyo_warp_yield_due", {ptr()}, i32()), {ctx});
        b.CreateCondBr(
            b.CreateICmpNE(due, llvm::ConstantInt::get(i32(), 0)),
            yieldBB, done);
        b.SetInsertPoint(yieldBB);
        {
            // warp 定義内ではこの yield は watchdog/GC 予算の協調点だけ
            // （意味論的な停止点ではない）。legacy の callCooperativeYield
            // と同じく !xyo.coop を付け、stackless::lowerRecursive が
            // coro.suspend にしないよう印を付ける。それ以外（非 warp の
            // 手続きや entry）の warp 枝は意味論 yield として残す。
            auto* call = b.CreateCall(
                callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
            if (mfWarp_)
                call->setMetadata("xyo.coop", llvm::MDNode::get(lc, {}));
        }
        b.CreateBr(done);
        b.SetInsertPoint(done);
        return;
    }
    mayYield = true;
    voluntaryYield = true;
    // カウンタは関数エントリの alloca（epilogue で ctx へ書き戻す）。
    llvm::AllocaInst* counter = yieldCounter();
    LValue* n = b.CreateAdd(
        b.CreateLoad(i64(), counter),
        llvm::ConstantInt::get(i64(), warpChunkStep_));
    b.CreateStore(n, counter);
    // fiber は実行中ずっと同じ値。関数の entry で 1 回だけロードする。
    if (!fiberCache_) {
        llvm::BasicBlock& fentry = fn->getEntryBlock();
        IRBuilder<> ib(&fentry, fentry.begin());
        fiberCache_ = ib.CreateLoad(
            ptr(), ib.CreateConstGEP1_32(
                       i8(), ctx, uint32_t(offsetof(JitCtx, fiber))));
    }
    LValue* fiber = fiberCache_;
    LValue* warp = b.CreateLoad(
        i32(), ctxField(uint32_t(offsetof(JitCtx, isWarp))));

    BasicBlock* normal = BasicBlock::Create(lc, "yield.normal", fn);
    BasicBlock* warpBB = BasicBlock::Create(lc, "yield.warp", fn);
    BasicBlock* dueBB = BasicBlock::Create(lc, "yield.due", fn);
    BasicBlock* done = BasicBlock::Create(lc, "yield.done", fn);
    b.CreateCondBr(b.CreateICmpEQ(warp, llvm::ConstantInt::get(i32(), 0)),
                   normal, warpBB);
    b.SetInsertPoint(normal);
    b.CreateCall(callee("xyo_yield_fast", {ptr()}, b.getVoidTy()), {fiber});
    b.CreateBr(done);
    b.SetInsertPoint(warpBB);
    LValue* due = b.CreateICmpEQ(
        b.CreateAnd(n, llvm::ConstantInt::get(i64(), 63)),
        llvm::ConstantInt::get(i64(), 0));
    b.CreateCondBr(due, dueBB, done);
    b.SetInsertPoint(dueBB);
    // warp 枝: legacy と同じく期限超過かだけを見て、実際に譲るときだけ
    // yield + hoist 再評価（isWarp!=0 済みなので先頭検査は省略）。
    BasicBlock* handBB = BasicBlock::Create(lc, "yield.hand", fn);
    emitWarpDueCheck(handBB, done, /*warpKnown=*/true);
    b.SetInsertPoint(handBB);
    b.CreateCall(callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
    recomputeHoisted();
    b.CreateBr(done);
    b.SetInsertPoint(done);
}

namespace {
/// if/else を select に畳んでよい「安い純粋 op」か。
///
/// Scratch の if/else は値を作るだけの形が多く（collatz の偶奇分岐など）、
/// 分岐予測ミスが本体コストを支配する。両側を計算して select にすると
/// 予測が不要になる。
///
/// 除外するもの:
///  - region 付き（if/while/repeat）: 制御フローを作るので畳めない
///  - リスト読み書き・文字列生成・呼び出しを伴う重い reporter
///  - 整数除算/剰余: 0 除算が未定義（投機実行で踏む）
bool selectableIfOp(const std::string& n) {
    static const std::unordered_set<std::string> kOk = {
        "xyo.const",      "xyo.const.f64", "xyo.const.i",
        "xyo.add.f",      "xyo.sub.f",     "xyo.mul.f",
        "xyo.div.f",      "xyo.mod.f",     "xyo.add.i",
        "xyo.sub.i",      "xyo.mul.i",     "xyo.to_num",
        "xyo.to_int",     "xyo.from_int",  "xyo.from_num",
        "xyo.box.f",      "xyo.cmp.f",     "xyo.cmp.i",
        "xyo.cmp.guarded", "xyo.lt",       "xyo.gt",
        "xyo.eq",         "xyo.truthy",    "xyo.not",
        "xyo.and",        "xyo.or",        "xyo.bnot",
        "xyo.band",       "xyo.bor",       "xyo.select",
        "xyo.round.f",    "xyo.mathop.f",  "xyo.boolval",
    };
    return kOk.count(n) != 0;
}

bool ifSelectEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("XYO_MLIR_NOIFSELECT");
        return !(e && e[0] == '1');
    }();
    return on;
}

/// 両分岐が「安い純粋式 + yield」だけなら true。
bool selectableIf(Op& op) {
    if (op.regions.size() != 2) return false;
    for (auto& r : op.regions) {
        if (!r || r->blocks.size() != 1) return false;
        Block* blk = r->entry();
        if (blk->ops.empty()) return false;
        for (Op* o : blk->ops) {
            if (o->name == "xyo.yield") continue;
            if (dialect::isRegionOp(o->name)) return false;
            if (!selectableIfOp(o->name)) return false;
        }
        if (blk->lastOp()->name != "xyo.yield") return false;
    }
    return true;
}
}  // namespace

void Lowerer::emitIf(Op& op) {
    if (op.operands.size() != 1 || op.regions.size() != 2) {
        error = "malformed xyo.if";
        return;
    }
    LValue* cond = value(op.operands[0]);
    // short-circuit 前提の値（0 除算・範囲外参照など）を作る op を
    // 含まない純粋な分岐は、両側を計算して select にする。
    if (ifSelectEnabled() && selectableIf(op)) {
        BasicBlock* cur = b.GetInsertBlock();
        std::vector<LValue*> thenVals, elseVals;
        lowerRegionBody(op.region(0), cur, &thenVals);
        if (!error.empty()) return;
        lowerRegionBody(op.region(1), cur, &elseVals);
        if (!error.empty()) return;
        if (thenVals.size() == op.resultTypes.size() &&
            elseVals.size() == op.resultTypes.size()) {
            b.SetInsertPoint(cur);
            for (size_t i = 0; i < op.resultTypes.size(); ++i) {
                if (!thenVals[i] || !elseVals[i]) continue;
                vals[Key{&op, static_cast<uint32_t>(i), false}] =
                    b.CreateSelect(cond, thenVals[i], elseVals[i]);
            }
            return;
        }
        // 形が合わない（yield の数が違う）場合は通常の分岐へ。ここまで
        // 副作用は無いので、計算済みの値は捨てて作り直せばよい。
        error.clear();
        b.SetInsertPoint(cur);
    }
    BasicBlock* thenBB = BasicBlock::Create(lc, "if.then", fn);
    BasicBlock* elseBB = BasicBlock::Create(lc, "if.else", fn);
    BasicBlock* contBB = BasicBlock::Create(lc, "if.end", fn);
    b.CreateCondBr(cond, thenBB, elseBB);

    b.SetInsertPoint(thenBB);
    std::vector<LValue*> thenVals;
    lowerRegionBody(op.region(0), thenBB, &thenVals);
    BasicBlock* thenEnd = b.GetInsertBlock();
    b.CreateBr(contBB);

    b.SetInsertPoint(elseBB);
    std::vector<LValue*> elseVals;
    lowerRegionBody(op.region(1), elseBB, &elseVals);
    BasicBlock* elseEnd = b.GetInsertBlock();
    b.CreateBr(contBB);

    b.SetInsertPoint(contBB);
    for (size_t i = 0; i < op.resultTypes.size(); ++i) {
        auto* phi = b.CreatePHI(ty(op.resultTypes[i]), 2);
        if (i < thenVals.size() && thenVals[i]) phi->addIncoming(thenVals[i], thenEnd);
        if (i < elseVals.size() && elseVals[i]) phi->addIncoming(elseVals[i], elseEnd);
        vals[Key{&op, static_cast<uint32_t>(i), false}] = phi;
    }
}

void Lowerer::emitWhile(Op& op) {
    if (op.regions.size() != 2) {
        error = "malformed xyo.while";
        return;
    }
    Block* before = op.region(0)->entry();
    Block* body = op.region(1)->entry();
    if (!before || !body) {
        error = "xyo.while region has no block";
        return;
    }
    // ループ不変なリスト読みの巻き上げ用スコープ。cond+body 両方を見る。
    pushLoopScope(*body, op.attrInt("warp") != 0, before);
    struct ScopeGuard {
        Lowerer* self;
        ~ScopeGuard() { self->popLoopScope(); }
    } scopeGuard{this};
    BasicBlock* pre = b.GetInsertBlock();
    BasicBlock* condBB = BasicBlock::Create(lc, "while.cond", fn);
    BasicBlock* bodyBB = BasicBlock::Create(lc, "while.body", fn);
    BasicBlock* exitBB = BasicBlock::Create(lc, "while.end", fn);

    // cond 側の iter_args 用 phi（入口値は pre から入る）。
    b.SetInsertPoint(condBB);
    std::vector<llvm::PHINode*> condPhis;
    for (size_t i = 0; i < before->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(before->args[i].type), 2);
        vals[keyOf(before->args[i])] = phi;
        condPhis.push_back(phi);
        if (i < op.operands.size() && op.operands[i].valid()) {
            // 入口値は pre から。lowerBlock 後に addIncoming する。
        }
    }
    b.SetInsertPoint(pre);
    std::vector<LValue*> initVals;
    for (size_t i = 0; i < before->args.size() && i < op.operands.size(); ++i)
        initVals.push_back(value(op.operands[i]));
    // numFacts 用: cond 側 arg の incoming = {entry, body の yield 値}。
    {
        Op* bodyTerm = body->lastOp();
        if (bodyTerm && bodyTerm->name == "xyo.yield") {
            for (size_t i = 0; i < before->args.size(); ++i) {
                if (before->args[i].type != Type::f64()) continue;
                auto& vec = numFactsInsStore_.emplace_back();
                if (i < op.operands.size())
                    vec.push_back(&op.operands[i]);
                if (i < bodyTerm->operands.size())
                    vec.push_back(&bodyTerm->operands[i]);
                loopArgIns_[keyOf(before->args[i])] = &vec;
            }
        }
    }
    b.CreateBr(condBB);
    for (size_t i = 0; i < condPhis.size() && i < initVals.size(); ++i)
        condPhis[i]->addIncoming(initVals[i], pre);

    b.SetInsertPoint(condBB);
    lowerBlock(*before, condBB);
    Op* term = before->lastOp();
    if (!term || term->name != "xyo.condition") {
        error = "xyo.while before-region must end with xyo.condition";
        return;
    }
    LValue* cond = term->operands.empty() ? nullptr : value(term->operands[0]);
    std::vector<LValue*> forwarded;
    for (size_t i = 1; i < term->operands.size(); ++i)
        forwarded.push_back(value(term->operands[i]));
    BasicBlock* condEnd = b.GetInsertBlock();
    b.CreateCondBr(cond, bodyBB, exitBB);

    // body 側 iter_args。
    b.SetInsertPoint(bodyBB);
    std::vector<llvm::PHINode*> bodyPhis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        if (i < forwarded.size()) phi->addIncoming(forwarded[i], condEnd);
        vals[keyOf(body->args[i])] = phi;
        bodyPhis.push_back(phi);
    }
    lowerBlock(*body, bodyBB);
    Op* bodyTerm = body->lastOp();
    if (!bodyTerm || bodyTerm->name != "xyo.yield") {
        error = "xyo.while body must end with xyo.yield";
        return;
    }
    std::vector<LValue*> back;
    for (const Value& v : bodyTerm->operands) back.push_back(value(v));
    BasicBlock* bodyEnd = b.GetInsertBlock();
    b.CreateBr(condBB);
    for (size_t i = 0; i < condPhis.size(); ++i)
        if (i < back.size()) condPhis[i]->addIncoming(back[i], bodyEnd);

    b.SetInsertPoint(exitBB);
    for (size_t i = 0; i < op.resultTypes.size(); ++i) {
        vals[Key{&op, static_cast<uint32_t>(i), false}] =
            i < condPhis.size()
                ? static_cast<LValue*>(condPhis[i])
                : static_cast<LValue*>(llvm::UndefValue::get(i64()));
    }
}

std::vector<llvm::Value*> Lowerer::spillAndYield(
    const std::vector<std::string>& ids, const std::vector<std::string>& names,
    const std::vector<LValue*>& values) {
    std::vector<LValue*> reloaded;
    for (size_t i = 0; i < ids.size(); ++i) {
        LValue* var = slotVariable(ids[i], names[i], false);
        LValue* boxed =
            b.CreateCall(callee("xyo_num", {f64()}, i64()), {values[i]});
        b.CreateStore(boxed,
                      b.CreateConstGEP1_32(i8(), var,
                                           uint32_t(offsetof(Variable, value))));
    }
    b.CreateCall(callee("xyo_loop_yield", {ptr()}, b.getVoidTy()), {ctx});
    for (size_t i = 0; i < ids.size(); ++i) {
        LValue* var = slotVariable(ids[i], names[i], false);
        LValue* bits = b.CreateLoad(
            i64(), b.CreateConstGEP1_32(i8(), var,
                                        uint32_t(offsetof(Variable, value))));
        reloaded.push_back(
            b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {bits}));
    }
    return reloaded;
}

std::vector<llvm::Value*> Lowerer::spillAndYieldBits(
    const std::vector<std::string>& ids, const std::vector<std::string>& names,
    const std::vector<LValue*>& bits) {
    std::vector<LValue*> reloaded;
    for (size_t i = 0; i < ids.size(); ++i) {
        LValue* var = slotVariable(ids[i], names[i], false);
        b.CreateStore(bits[i],
                      b.CreateConstGEP1_32(i8(), var,
                                           uint32_t(offsetof(Variable, value))));
    }
    b.CreateCall(callee("xyo_loop_yield", {ptr()}, b.getVoidTy()), {ctx});
    for (size_t i = 0; i < ids.size(); ++i) {
        LValue* var = slotVariable(ids[i], names[i], false);
        reloaded.push_back(b.CreateLoad(
            i64(), b.CreateConstGEP1_32(
                       i8(), var, uint32_t(offsetof(Variable, value)))));
    }
    return reloaded;
}


Lowerer::ChunkInfo Lowerer::analyzeChunk(Block& body) {
    // warp ループの 64 反復まとめ処理は廃止した（昇格 + xyo.warp_yield が
    // 同じ「譲るのは 64 回に 1 回」を担う）。互換のため空を返す。
    (void)body;
    return ChunkInfo{};
}

/// warp repeat の 64 反復チャンク展開。本体が小さく純粋なときに限り、
/// 64 コピーを直線に並べて yield 検査を 1/64 に減らす。ほぼ自明な本体
/// （`x += 1` 等）では LLVM がコピー間の IV 最適化で閉形式に畳めるため、
/// 逐次実行に対して大幅に速くなる（意味論は不変: 反復回数・yield 間隔・
/// 変数の最終値をすべて保つ。端数は末尾 tail ループが 1 回ずつ処理）。
bool Lowerer::emitChunkedRepeat(Op& op, LValue* remaining, Block* body,
                                const std::vector<LValue*>& initVals) {
    constexpr int kChunk = 64;
    Op* term = body->lastOp();
    if (!term || term->name != "xyo.yield") {
        error = "xyo.repeat body must end with xyo.yield";
        return true;  // error set
    }
    BasicBlock* pre = b.GetInsertBlock();
    BasicBlock* head = BasicBlock::Create(lc, "rep.chunk.head", fn);
    BasicBlock* chunkBB = BasicBlock::Create(lc, "rep.chunk.body", fn);
    BasicBlock* tailHead = BasicBlock::Create(lc, "rep.tail.head", fn);
    BasicBlock* tailBB = BasicBlock::Create(lc, "rep.tail.body", fn);
    BasicBlock* exitBB = BasicBlock::Create(lc, "rep.end", fn);
    b.CreateBr(head);

    b.SetInsertPoint(head);
    auto* headRem = b.CreatePHI(f64(), 2);
    headRem->addIncoming(remaining, pre);
    std::vector<llvm::PHINode*> headPhis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        if (i < initVals.size()) phi->addIncoming(initVals[i], pre);
        headPhis.push_back(phi);
    }
    LValue* go = b.CreateFCmpOGE(
        headRem, llvm::ConstantFP::get(f64(), double(kChunk) + 0.5));
    b.CreateCondBr(go, chunkBB, tailHead);

    // 64 コピー（最後だけ yield 検査を出す）。
    b.SetInsertPoint(chunkBB);
    std::vector<LValue*> cur(headPhis.begin(), headPhis.end());
    for (int c = 0; c < kChunk; ++c) {
        // 64 コピーは同じ LLVM 基本ブロックへ並べる。numShadow_ は
        // 「同じブロック内なら再利用してよい」前提だが、それは 1 回の
        // lower につき 1 度しか実行しない場合の話で、ここでは同じ Op を
        // コピーごとに再 lower して別の SSA 値を作る。キャッシュを消さない
        // と 2 コピー目以降が先頭コピーの影（たとえば先頭反復のカウンタを
        // 数値化した値）を掴み、チャンク内の計算が固定されてしまう。
        numShadow_.clear();
        for (size_t i = 0; i < body->args.size(); ++i)
            vals[keyOf(body->args[i])] = cur[i];
        suppressWarpYield_ = (c != kChunk - 1);
        warpChunkStep_ = (c == kChunk - 1) ? kChunk : 1;
        lowerBlock(*body, b.GetInsertBlock());
        suppressWarpYield_ = false;
        if (!error.empty()) return true;
        term = body->lastOp();
        if (!term || term->name != "xyo.yield") {
            error = "xyo.repeat body must end with xyo.yield";
            return true;
        }
        cur.clear();
        for (const Value& v : term->operands) cur.push_back(value(v));
    }
    warpChunkStep_ = 1;
    LValue* nextRem = b.CreateFSub(
        headRem, llvm::ConstantFP::get(f64(), double(kChunk)));
    BasicBlock* chunkEnd = b.GetInsertBlock();
    b.CreateBr(head);
    headRem->addIncoming(nextRem, chunkEnd);
    for (size_t i = 0; i < headPhis.size() && i < cur.size(); ++i)
        headPhis[i]->addIncoming(cur[i], chunkEnd);

    // tail（端数は 1 反復ずつ）。
    b.SetInsertPoint(tailHead);
    auto* tailRem = b.CreatePHI(f64(), 2);
    tailRem->addIncoming(headRem, head);
    std::vector<llvm::PHINode*> tailPhis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        phi->addIncoming(headPhis[i], head);
        tailPhis.push_back(phi);
    }
    LValue* go2 = b.CreateFCmpOGE(tailRem,
                                  llvm::ConstantFP::get(f64(), 0.5));
    b.CreateCondBr(go2, tailBB, exitBB);

    b.SetInsertPoint(tailBB);
    for (size_t i = 0; i < body->args.size(); ++i)
        vals[keyOf(body->args[i])] = tailPhis[i];
    lowerBlock(*body, tailBB);
    if (!error.empty()) return true;
    term = body->lastOp();
    if (!term || term->name != "xyo.yield") {
        error = "xyo.repeat body must end with xyo.yield";
        return true;
    }
    std::vector<LValue*> next;
    for (const Value& v : term->operands) next.push_back(value(v));
    LValue* nextRem2 = b.CreateFSub(tailRem,
                                    llvm::ConstantFP::get(f64(), 1.0));
    BasicBlock* tailEnd = b.GetInsertBlock();
    b.CreateBr(tailHead);
    tailRem->addIncoming(nextRem2, tailEnd);
    for (size_t i = 0; i < tailPhis.size() && i < next.size(); ++i)
        tailPhis[i]->addIncoming(next[i], tailEnd);

    b.SetInsertPoint(exitBB);
    for (size_t i = 0; i < op.resultTypes.size(); ++i) {
        vals[Key{&op, static_cast<uint32_t>(i), false}] =
            i < tailPhis.size() ? tailPhis[i]
                                : static_cast<LValue*>(
                                      llvm::UndefValue::get(i64()));
    }
    return true;
}

namespace {
/// バッチループの本体に置いてよい葉 op。バッチ内で中断（yield）できる
/// op を含めると、外側の昇格値がレジスタに載ったまま他スクリプトへ
/// 実行が移り、spill/reload の契約が崩れる。呼び出し・compat・待機系・
/// 入れ子 yield はすべて除外する（入れ子ループは呼び出し側で別途拒否）。
bool batchSafeLeaf(const std::string& n) {
    static const std::unordered_set<std::string> kOk = {
        "xyo.const",      "xyo.const.f64",  "xyo.const.i",  "xyo.getvar",
        "xyo.setvar",     "xyo.add",        "xyo.sub",      "xyo.mul",
        "xyo.div",        "xyo.mod",        "xyo.round",    "xyo.to_num",
        "xyo.from_num",   "xyo.to_int",     "xyo.from_int", "xyo.box.f",
        "xyo.select",     "xyo.select.f64", "xyo.truthy",   "xyo.not",
        "xyo.and",        "xyo.or",         "xyo.band",     "xyo.bor",
        "xyo.bnot",       "xyo.boolval",    "xyo.num_ok",   "xyo.lt",
        "xyo.gt",         "xyo.eq",         "xyo.cmp.f",    "xyo.cmp.i",
        "xyo.cmp.guarded", "xyo.int_exact_ok", "xyo.add.i", "xyo.sub.i",
        "xyo.mul.i",      "xyo.div.i",      "xyo.mod.i",    "xyo.add.f",
        "xyo.sub.f",      "xyo.mul.f",      "xyo.div.f",    "xyo.mod.f",
        "xyo.mathop.f",   "xyo.round.f",    "xyo.list.ptr", "xyo.list.item",
        "xyo.list.item.i", "xyo.list.item.num", "xyo.list.item.num.i",
        "xyo.list.itemnum", "xyo.list.length", "xyo.list.contains",
        "xyo.list.contents", "xyo.list.replace", "xyo.list.replace.i",
        "xyo.warp_yield", "xyo.yield",
    };
    return kOk.count(n) != 0;
}
} // namespace

std::vector<LValue*> Lowerer::emitWarpCheck(
    Op* wy, const std::vector<LValue*>& cur) {
    // warp_yield のオペランド SSA 値を現在値へ差し替えて検査を発行し、
    // 検査結果（spill/reload 後の運搬値）を返す。呼び出し側は差し替えた
    // エントリを元に戻す必要がある（後続 lower が古い値を見ないように）。
    std::vector<std::pair<Key, LValue*>> saved;
    const size_t nOps = std::min(wy->operands.size(), cur.size());
    for (size_t i = 0; i < nOps; ++i) {
        const Key k = keyOf(wy->operands[i]);
        auto it = vals.find(k);
        saved.push_back({k, it != vals.end() ? it->second : nullptr});
        if (cur[i]) vals[k] = cur[i];
    }
    if (std::getenv("XYO_MLIR_BATCHDBG"))
        std::fprintf(stderr, "[batch] check nops=%zu cur=%zu\n", nOps,
                     cur.size());
    const bool oldSuppress = suppressWarpYield_;
    Op* oldSuppressOp = suppressWarpYieldOp_;
    suppressWarpYield_ = false;
    suppressWarpYieldOp_ = nullptr;
    emitOp(*wy);
    suppressWarpYield_ = oldSuppress;
    suppressWarpYieldOp_ = oldSuppressOp;
    if (std::getenv("XYO_MLIR_BATCHDBG"))
        std::fprintf(stderr, "[batch] check done err=%s\n", error.c_str());
    std::vector<LValue*> out;
    for (size_t i = 0; i < wy->resultTypes.size(); ++i)
        out.push_back(value(wy->result(i)));
    for (auto& [k, v] : saved) {
        if (v)
            vals[k] = v;
        else
            vals.erase(k);
    }
    return out;
}

bool Lowerer::emitBatchedRepeat(Op& op, LValue* remaining, Block* body,
                                const std::vector<LValue*>& initVals) {
    Op* term = body->lastOp();
    if (!term || term->name != "xyo.yield") return false;
    // 本体に入れ子ループがあると、yield 検査をまとめる範囲が広がり
    // スケジューリングが変わる。その場合は通常経路に任せる。
    Op* wy = nullptr;
    std::function<bool(Block&)> find = [&](Block& blk) -> bool {
        for (Op* o : blk.ops) {
            // 入れ子ループがあると、その中の yield で外側の昇格値が
            // レジスタに載ったまま中断し得る。バッチは「バッチ内では
            // 中断しない」ことが前提なので対象外にする。
            if (o->name == "xyo.repeat" || o->name == "xyo.while" ||
                o->name == "xyo.forever" || o->name == "xyo.for_each")
                return false;
            if (o->name == "xyo.warp_yield") {
                if (wy) return false;
                wy = o;
                continue;
            }
            if (o->regions.empty()) {
                if (!batchSafeLeaf(o->name)) return false;
                continue;
            }
            // リージョン付きは if だけ許可（if 自体は中断しない）。
            if (o->name != "xyo.if") return false;
            for (auto& r : o->regions) {
                if (!r) continue;
                for (auto& cb : r->blocks)
                    if (!find(*cb)) return false;
            }
        }
        return true;
    };
    if (!find(*body) || !wy) return false;
    // yield の先頭が warp_yield の結果と 1:1 で並んでいること。
    if (wy->operands.size() > body->args.size() ||
        term->operands.size() < wy->resultTypes.size())
        return false;
    // 代表的な形は 2 通り:
    //   (a) yield(wy の結果, ...): 昇格ループそのもの
    //   (b) yield(if(cond){ wy の結果 } else { 旧値 }, ...): box 版の bail
    //       if。then 側の yield が同じ順で wy の結果を運ぶことを確認する。
    for (size_t i = 0; i < wy->resultTypes.size(); ++i) {
        const Value& tv = term->operands[i];
        if (tv.op == wy) continue;
        Op* ifo = tv.op;
        Block* rb = (ifo && ifo->name == "xyo.if" && ifo->regions.size() == 2 &&
                     ifo->region(0))
                        ? ifo->region(0)->entry()
                        : nullptr;
        Op* ry = rb ? rb->lastOp() : nullptr;
        if (!ry || ry->name != "xyo.yield" || i >= ry->operands.size() ||
            ry->operands[i].op != wy)
            return false;
    }

    BasicBlock* pre = b.GetInsertBlock();
    BasicBlock* head = BasicBlock::Create(lc, "rep.batch.head", fn);
    BasicBlock* batchHead = BasicBlock::Create(lc, "rep.batch.size", fn);
    BasicBlock* innerHead = BasicBlock::Create(lc, "rep.batch.inner", fn);
    BasicBlock* innerBody = BasicBlock::Create(lc, "rep.batch.body", fn);
    BasicBlock* after = BasicBlock::Create(lc, "rep.batch.after", fn);
    BasicBlock* exitBB = BasicBlock::Create(lc, "rep.batch.end", fn);

    // リストヘッダを外側ループの phi として運ぶ（バッチ内では不変、
    // バッチ境界の yield 後に読み直す）。バッチ内のリスト読みが
    // lp→len→items の 3 段ロードではなくレジスタだけになる。
    // `pre` ではヘッダの初期値を読む（null なら null/0 を phi の初期値に）。
    std::vector<ListHeader*> phiHeaders;
    if (!loopScopes_.empty()) {
        for (ListHeader& h : loopScopes_.back().headers)
            phiHeaders.push_back(&h);
    }
    std::vector<LValue*> hdrLpInit, hdrItInit, hdrLnInit;
    for (ListHeader* h : phiHeaders) {
        LValue* lp0 = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), h->var, uint32_t(offsetof(Variable, list))));
        BasicBlock* iniNull = b.GetInsertBlock();
        BasicBlock* iniHave = BasicBlock::Create(lc, "lst.inihave", fn);
        BasicBlock* iniDone = BasicBlock::Create(lc, "lst.inidone", fn);
        b.CreateCondBr(b.CreateIsNull(lp0), iniDone, iniHave);
        b.SetInsertPoint(iniHave);
        LValue* it0 = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), lp0, uint32_t(offsetof(SList, items))));
        LValue* ln0 = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), lp0, uint32_t(offsetof(SList, len))));
        b.CreateBr(iniDone);
        b.SetInsertPoint(iniDone);
        auto* lpI = b.CreatePHI(ptr(), 2);
        lpI->addIncoming(lp0, iniHave);
        lpI->addIncoming(llvm::ConstantPointerNull::get(ptr()), iniNull);
        auto* itI = b.CreatePHI(ptr(), 2);
        itI->addIncoming(it0, iniHave);
        itI->addIncoming(llvm::ConstantPointerNull::get(ptr()), iniNull);
        auto* lnI = b.CreatePHI(i32(), 2);
        lnI->addIncoming(ln0, iniHave);
        lnI->addIncoming(llvm::ConstantInt::get(i32(), 0), iniNull);
        hdrLpInit.push_back(lpI);
        hdrItInit.push_back(itI);
        hdrLnInit.push_back(lnI);
    }
    BasicBlock* preEnd = b.GetInsertBlock();
    b.CreateBr(head);

    b.SetInsertPoint(head);
    auto* remainPhi = b.CreatePHI(f64(), 2);
    remainPhi->addIncoming(remaining, preEnd);
    // ヘッダ phi（preEnd から初期値、afterEnd からリロード値）。
    for (size_t hi = 0; hi < phiHeaders.size(); ++hi) {
        ListHeader* h = phiHeaders[hi];
        auto* lpP = b.CreatePHI(ptr(), 2);
        lpP->addIncoming(hdrLpInit[hi], preEnd);
        auto* itP = b.CreatePHI(ptr(), 2);
        itP->addIncoming(hdrItInit[hi], preEnd);
        auto* lnP = b.CreatePHI(i32(), 2);
        lnP->addIncoming(hdrLnInit[hi], preEnd);
        h->lpPhi = lpP;
        h->itemsPhi = itP;
        h->lenPhi = lnP;
    }
    std::vector<llvm::PHINode*> phis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        if (i < initVals.size()) phi->addIncoming(initVals[i], preEnd);
        phis.push_back(phi);
    }
    LValue* go = b.CreateFCmpOGE(remainPhi,
                                 llvm::ConstantFP::get(f64(), 0.5));
    b.CreateCondBr(go, batchHead, exitBB);

    // このバッチで実行する反復数（最大 64、端数は残り全部）。
    b.SetInsertPoint(batchHead);
    LValue* used = b.CreateSelect(
        b.CreateFCmpOGE(remainPhi, llvm::ConstantFP::get(f64(), 64.5)),
        llvm::ConstantFP::get(f64(), 64.0), remainPhi);
    // バッチ内の残り反復数は整数カウンタで回す。f64 だと XMM を 1 本
    // 占有し、fadd(4cy)+ucomisd がループキャリーになる。実行回数は
    // floor(used+0.5) で従来の batchPhi>=0.5 判定と同値。
    LValue* usedI = b.CreateFPToSI(
        b.CreateFAdd(used, llvm::ConstantFP::get(f64(), 0.5)), i64());
    b.CreateBr(innerHead);

    // ペア運搬の f64 影は「バッチ入場時に一度だけ」正規化する
    // （バッチ内は書き込み側で正規化するので、ここは端数の 1 回だけ）。
    b.SetInsertPoint(batchHead->getTerminator());
    std::vector<LValue*> headIn(phis.begin(), phis.end());
    for (size_t i = 0; i < body->args.size(); ++i)
        if (body->args[i].type == Type::f64())
            headIn[i] = healOnce(phis[i]);
    b.SetInsertPoint(innerHead);
    auto* batchPhi = b.CreatePHI(i64(), 2);
    batchPhi->addIncoming(usedI, batchHead);
    std::vector<llvm::PHINode*> innerPhis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        phi->addIncoming(headIn[i], batchHead);
        innerPhis.push_back(phi);
        vals[keyOf(body->args[i])] = phi;
        // 以降の読みは heal 不要（バックエッジで正規化するので非 NaN が
        // 保証される）。'b' ペアだけでなく 'd' 単体運搬も対象にする —
        // 読み側の ord+select がループキャリー鎖に乗ると実測 1.6 倍遅い
        // （abl_read_varidx: fadd→ord→sel→phi の 7cy vs legacy 3cy）。
        if (body->args[i].type == Type::f64())
            markArgNonNaN(body->args[i]);
    }    LValue* go2 = b.CreateICmpSGE(batchPhi,
                                  llvm::ConstantInt::get(i64(), 1));
    b.CreateCondBr(go2, innerBody, after);

    b.SetInsertPoint(innerBody);
    // このループ自身の warp_yield はバッチ境界で 1 回だけ検査する。
    // 入れ子ループの yield 検査はそのまま残す。
    Op* oldSuppressOp = suppressWarpYieldOp_;
    suppressWarpYieldOp_ = wy;
    lowerBlock(*body, innerBody);
    suppressWarpYieldOp_ = oldSuppressOp;
    if (!error.empty()) return true;
    term = body->lastOp();
    if (!term || term->name != "xyo.yield") {
        error = "xyo.repeat body must end with xyo.yield";
        return true;
    }
    std::vector<LValue*> nextSeq;
    for (const Value& v : term->operands) nextSeq.push_back(value(v));
    // バックエッジは書き込み側で正規化する（分岐は予測されるので
    // ループキャリーの依存連鎖から vcmpord+blend が消える）。
    for (size_t i = 0; i < innerPhis.size(); ++i)
        if (i < nextSeq.size() && body->args[i].type == Type::f64())
            nextSeq[i] = healCarryOnWrite(nextSeq[i]);
    LValue* batchNext =
        b.CreateSub(batchPhi, llvm::ConstantInt::get(i64(), 1));
    BasicBlock* innerEnd = b.GetInsertBlock();
    b.CreateBr(innerHead);
    batchPhi->addIncoming(batchNext, innerEnd);
    for (size_t i = 0; i < innerPhis.size(); ++i)
        if (i < nextSeq.size()) innerPhis[i]->addIncoming(nextSeq[i], innerEnd);

    // バッチ境界の yield 検査。運搬値はこのバッチを抜けた時点の値。
    if (std::getenv("XYO_MLIR_BATCHDBG"))
        std::fprintf(stderr, "[batch] body lowered, phis=%zu args=%zu\n",
                     innerPhis.size(), body->args.size());
    b.SetInsertPoint(after);
    std::vector<LValue*> cur(innerPhis.begin(), innerPhis.end());
    // バッチで実際に実行した反復数（端数込み）。内側カウンタと同じ
    // floor(used+0.5) を共有する。
    warpStepOverride_ = usedI;
    std::vector<LValue*> checked = emitWarpCheck(wy, cur);
    warpStepOverride_ = nullptr;
    if (!error.empty()) return true;
    std::vector<LValue*> outerNext(phis.size(), nullptr);
    for (size_t i = 0; i < phis.size(); ++i) {
        if (i < checked.size() && checked[i])
            outerNext[i] = checked[i];
        else if (i < cur.size())
            outerNext[i] = cur[i];
        else
            outerNext[i] = llvm::UndefValue::get(ty(body->args[i].type));
    }
    // バッチ境界でリストヘッダを読み直す（yield 中に他スレッドが
    // 伸縮させ得るため）。バッチごとの数命令なので、手渡し無しでも
    // 無条件に読み直す。
    std::vector<LValue*> hdrLpNew, hdrItNew, hdrLnNew;
    for (size_t hi = 0; hi < phiHeaders.size(); ++hi) {
        ListHeader* h = phiHeaders[hi];
        LValue* lpN = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), h->var, uint32_t(offsetof(Variable, list))));
        BasicBlock* relNull = b.GetInsertBlock();
        BasicBlock* relHave = BasicBlock::Create(lc, "lst.relhave", fn);
        BasicBlock* relDone = BasicBlock::Create(lc, "lst.reldone", fn);
        b.CreateCondBr(b.CreateIsNull(lpN), relDone, relHave);
        b.SetInsertPoint(relHave);
        LValue* itN = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), lpN, uint32_t(offsetof(SList, items))));
        LValue* lnN = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), lpN, uint32_t(offsetof(SList, len))));
        b.CreateBr(relDone);
        b.SetInsertPoint(relDone);
        auto* lpP = b.CreatePHI(ptr(), 2);
        lpP->addIncoming(lpN, relHave);
        lpP->addIncoming(llvm::ConstantPointerNull::get(ptr()), relNull);
        auto* itP = b.CreatePHI(ptr(), 2);
        itP->addIncoming(itN, relHave);
        itP->addIncoming(llvm::ConstantPointerNull::get(ptr()), relNull);
        auto* lnP = b.CreatePHI(i32(), 2);
        lnP->addIncoming(lnN, relHave);
        lnP->addIncoming(llvm::ConstantInt::get(i32(), 0), relNull);
        hdrLpNew.push_back(lpP);
        hdrItNew.push_back(itP);
        hdrLnNew.push_back(lnP);
    }
    BasicBlock* afterEnd = b.GetInsertBlock();
    // 終端 br を出す前に残り回数の減算を発行する（後から作ると
    // 終端命令の後ろに命令が付いて壊れた IR になる）。
    LValue* nextRemain = b.CreateFSub(remainPhi, used);
    b.CreateBr(head);
    remainPhi->addIncoming(nextRemain, afterEnd);
    for (size_t i = 0; i < phis.size(); ++i)
        phis[i]->addIncoming(outerNext[i], afterEnd);
    for (size_t hi = 0; hi < phiHeaders.size(); ++hi) {
        ListHeader* h = phiHeaders[hi];
        static_cast<llvm::PHINode*>(h->lpPhi)->addIncoming(hdrLpNew[hi],
                                                         afterEnd);
        static_cast<llvm::PHINode*>(h->itemsPhi)->addIncoming(hdrItNew[hi],
                                                            afterEnd);
        static_cast<llvm::PHINode*>(h->lenPhi)->addIncoming(hdrLnNew[hi],
                                                           afterEnd);
    }

    b.SetInsertPoint(exitBB);
    for (size_t i = 0; i < op.resultTypes.size(); ++i) {
        vals[Key{&op, static_cast<uint32_t>(i), false}] =
            i < phis.size() ? phis[i]
                            : static_cast<LValue*>(
                                  llvm::UndefValue::get(i64()));
    }
    return true;
}

void Lowerer::emitRepeat(Op& op) {
    if (op.operands.empty() || op.regions.size() != 1) {
        error = "malformed xyo.repeat";
        return;
    }
    Block* body = op.region(0)->entry();
    if (!body) {
        error = "xyo.repeat has no body block";
        return;
    }
    LValue* count = value(op.operands[0]);
    // ループ不変なリスト読みの巻き上げ用スコープ。
    pushLoopScope(*body, op.attrInt("warp") != 0);
    struct ScopeGuard {
        Lowerer* self;
        ~ScopeGuard() { self->popLoopScope(); }
    } scopeGuard{this};
    // 定数回数はコンパイル時に数値化する（実行時 to_num 呼び出しの削減）。
    // 文字列定数も xyo_to_num と同一のヘルパで畳み込む（pinner が保持）。
    LValue* remaining = nullptr;
    if (op.operands[0].op && op.operands[0].op->name == "xyo.const") {
        const uint64_t cbits = static_cast<uint64_t>(
            op.operands[0].op->attrInt("bits"));
        remaining =
            llvm::ConstantFP::get(f64(), xyo_to_num(cbits));
    } else {
        remaining =
            b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {count});
    }

    std::vector<LValue*> initVals;
    for (size_t i = 1; i < op.operands.size(); ++i) {
        initVals.push_back(value(op.operands[i]));
        if (!error.empty()) {
            error += " (repeat init operand " + std::to_string(i) + ")";
            return;
        }
    }
    // numFacts 解析用: 各 f64 block arg の incoming（entry 値 + body の
    // yield バックエッジ値）を登録する。arg → {init, back} の 2 要素。
    {
        Op* term = body->lastOp();
        if (term && term->name == "xyo.yield") {
            for (size_t i = 0; i < body->args.size(); ++i) {
                if (body->args[i].type != Type::f64()) continue;
                auto& vec = numFactsInsStore_.emplace_back();
                if (i + 1 < op.operands.size())
                    vec.push_back(&op.operands[i + 1]);
                if (i < term->operands.size())
                    vec.push_back(&term->operands[i]);
                loopArgIns_[keyOf(body->args[i])] = &vec;
            }
        }
    }
    // warp ループで本体が小さいなら 64 反復チャンクに展開する。
    // 本体が大きいと IR が膨らみコンパイル時間に見合わないので除外。
    static const bool chunkOn = [] {
        const char* e = std::getenv("XYO_MLIR_NOCHUNK");
        return !(e && e[0] == '1');
    }();
    const bool warp = op.attrInt("warp") != 0;
    // 平坦かつ純整数 SSA の本体だけをチャンク化する。
    // - 入れ子 region（if/ループ）を含むと 64 コピーで IR が指数的に膨らむ。
    // - 呼び出し系（リスト・sensing・pen 等）を含む本体はコピーしても
    //   畳めず、コード肥大で逆に遅くなる。
    static const std::set<std::string> kChunkOk = {
        "xyo.const",   "xyo.const.f64", "xyo.getvar", "xyo.setvar",
        "xyo.add",     "xyo.sub",       "xyo.mul",    "xyo.div",
        "xyo.mod",     "xyo.round",     "xyo.to_num", "xyo.from_num",
        "xyo.lt",      "xyo.gt",        "xyo.eq",     "xyo.cmp.f",
        "xyo.select",  "xyo.box.f",     "xyo.truthy", "xyo.and",
        "xyo.or",      "xyo.not",       "xyo.band",   "xyo.bor",
        "xyo.bnot",    "xyo.boolval",   "xyo.num_ok", "xyo.loop_yield",
        "xyo.warp_yield", "xyo.yield",  "xyo.select.f64", "xyo.from_int",
        "xyo.to_int",
        // 昇格後の整数/倍精度 SSA（呼び出しを含まないので 64 コピーしても
        // コードが増えるだけで、LLVM がチャンク間で最適化できる）。
        "xyo.const.i",  "xyo.add.i",    "xyo.sub.i",   "xyo.mul.i",
        "xyo.mod.i",    "xyo.cmp.i",    "xyo.add.f",   "xyo.sub.f",
        "xyo.mul.f",    "xyo.div.f",    "xyo.mod.f",   "xyo.mathop.f",
        "xyo.round.f",
    };
    bool flatBody = body->ops.size() <= 32;
    if (flatBody) {
        for (Op* bop : body->ops) {
            if (!bop->regions.empty() || !kChunkOk.count(bop->name)) {
                flatBody = false;
                break;
            }
        }
    }
    if (chunkOn && warp && flatBody) {
        if (emitChunkedRepeat(op, remaining, body, initVals)) return;
    }
    // 分岐を含む warp ループでも、yield 検査だけを 64 反復ごとのバッチ
    // 境界へ出す（本体は 1 コピー）。検査の分岐・カウンタ更新が毎反復から
    // 消え、LLVM が本体をそのまま最適化できる。
    static const bool batchOff = [] {
        const char* e = std::getenv("XYO_MLIR_NOBATCH");
        return e && e[0] == '1';
    }();
    if (warp && !batchOff &&
        emitBatchedRepeat(op, remaining, body, initVals))
        return;

    BasicBlock* pre = b.GetInsertBlock();
    BasicBlock* headk = BasicBlock::Create(lc, "repeat.head", fn);
    BasicBlock* bodyBB = BasicBlock::Create(lc, "repeat.body", fn);
    BasicBlock* exitBB = BasicBlock::Create(lc, "repeat.end", fn);
    b.CreateBr(headk);

    b.SetInsertPoint(headk);
    auto* remainPhi = b.CreatePHI(f64(), 2);
    remainPhi->addIncoming(remaining, pre);
    std::vector<llvm::PHINode*> phis;
    for (size_t i = 0; i < body->args.size(); ++i) {
        auto* phi = b.CreatePHI(ty(body->args[i].type), 2);
        if (i < initVals.size()) phi->addIncoming(initVals[i], pre);
        phis.push_back(phi);
        vals[keyOf(body->args[i])] = phi;
    }
    LValue* go = b.CreateFCmpOGE(remainPhi,
                                 llvm::ConstantFP::get(f64(), 0.5));
    b.CreateCondBr(go, bodyBB, exitBB);

    b.SetInsertPoint(bodyBB);
    lowerBlock(*body, bodyBB);
    if (!error.empty()) return;
    Op* term = body->lastOp();
    if (!term || term->name != "xyo.yield") {
        error = "xyo.repeat body must end with xyo.yield";
        return;
    }
    std::vector<LValue*> next;
    for (const Value& v : term->operands) next.push_back(value(v));
    LValue* nextRemaining =
        b.CreateFSub(remainPhi, llvm::ConstantFP::get(f64(), 1.0));
    BasicBlock* bodyEnd = b.GetInsertBlock();
    b.CreateBr(headk);
    remainPhi->addIncoming(nextRemaining, bodyEnd);
    for (size_t i = 0; i < phis.size(); ++i)
        if (i < next.size()) phis[i]->addIncoming(next[i], bodyEnd);

    b.SetInsertPoint(exitBB);
    for (size_t i = 0; i < op.resultTypes.size(); ++i) {
        vals[Key{&op, static_cast<uint32_t>(i), false}] =
            i < phis.size() ? phis[i]
                            : static_cast<LValue*>(
                                  llvm::UndefValue::get(i64()));
    }
}

void Lowerer::emitForever(Op& op) {
    if (op.regions.size() != 1) {
        error = "malformed xyo.forever";
        return;
    }
    BasicBlock* body = BasicBlock::Create(lc, "forever.body", fn);
    b.CreateBr(body);
    b.SetInsertPoint(body);
    Block* blk = op.region(0)->entry();
    if (!blk) {
        error = "xyo.forever has no body block";
        return;
    }
    lowerBlock(*blk, body);
    b.CreateBr(body);
}

void Lowerer::emitAllAtOnce(Op& op) {
    if (op.regions.size() != 1 || !op.region(0)->entry()) {
        error = "malformed xyo.all_at_once";
        return;
    }
    // run without screen refresh: 本体は isWarp=1 で走る。
    mayYield = true;
    LValue* saved = b.CreateLoad(i32(), ctxField(uint32_t(offsetof(JitCtx, isWarp))));
    b.CreateStore(llvm::ConstantInt::get(i32(), 1),
                  ctxField(uint32_t(offsetof(JitCtx, isWarp))));
    std::vector<LValue*> yielded;
    lowerRegionBody(op.region(0), b.GetInsertBlock(), &yielded);
    b.CreateStore(saved, ctxField(uint32_t(offsetof(JitCtx, isWarp))));
}

void Lowerer::emitWaitUntil(Op& op) {
    if (op.regions.size() != 1 || !op.region(0)->entry()) {
        error = "malformed xyo.wait_until";
        return;
    }
    // コルーチン化前提では event 依存最適化（xyo_wait_event_yield 内部で
    // Fiber を譲る）を使わない — 停止点は明示的な xyo_yield だけにする。
    const bool event = !coroMode_ && op.attrInt("event") != 0;
    const uint64_t site = static_cast<uint64_t>(op.attrInt("site"));
    mayYield = true;
    voluntaryYield = true;
    BasicBlock* head = BasicBlock::Create(lc, "wait.head", fn);
    BasicBlock* yieldBB = BasicBlock::Create(lc, "wait.yield", fn);
    BasicBlock* doneBB = BasicBlock::Create(lc, "wait.done", fn);
    b.CreateBr(head);
    b.SetInsertPoint(head);
    std::vector<LValue*> vals0;
    lowerRegionBody(op.region(0), head, &vals0);
    Op* term = op.region(0)->entry()->lastOp();
    if (!term || term->name != "xyo.condition" || term->operands.empty()) {
        error = "xyo.wait_until must end with xyo.condition";
        return;
    }
    LValue* cond = value(term->operands[0]);
    b.CreateCondBr(cond, doneBB, yieldBB);
    b.SetInsertPoint(yieldBB);
    if (event) {
        b.CreateCall(callee("xyo_wait_event_yield", {ptr(), i64()},
                            b.getVoidTy()),
                     {ctx, llvm::ConstantInt::get(i64(), site)});
    } else {
        LValue* should = b.CreateCall(
            callee("xyo_wait_should_yield", {ptr()}, i32()), {ctx});
        BasicBlock* doYield = BasicBlock::Create(lc, "wait.sched", fn);
        BasicBlock* after = BasicBlock::Create(lc, "wait.cont", fn);
        b.CreateCondBr(b.CreateICmpNE(should,
                                      llvm::ConstantInt::get(i32(), 0)),
                       doYield, after);
        b.SetInsertPoint(doYield);
        b.CreateCall(callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
        b.CreateBr(after);
        b.SetInsertPoint(after);
    }
    b.CreateBr(head);
    b.SetInsertPoint(doneBB);
    if (event)
        b.CreateCall(callee("xyo_wait_event_done", {ptr()}, b.getVoidTy()),
                     {ctx});
}

void Lowerer::emitForEach(Op& op) {
    if (op.regions.size() != 1 || op.operands.empty() ||
        !op.region(0)->entry()) {
        error = "malformed xyo.for_each";
        return;
    }
    const std::string id(op.attrStr("id"));
    const std::string name(op.attrStr("name"));
    Block* body = op.region(0)->entry();
    // Scratch の for each: index を 1 から VALUE まで（VALUE が端数なら
    // ceil）進め、その都度変数へ代入して本体を実行する。
    LValue* n = b.CreateCall(callee("xyo_to_num", {i64()}, f64()),
                             {value(op.operands[0])});
    llvm::AllocaInst* idx = entryAlloca(i64(), "fe.idx");
    b.CreateStore(llvm::ConstantInt::get(i64(), 1), idx);
    BasicBlock* head = BasicBlock::Create(lc, "fe.head", fn);
    BasicBlock* bodyBB = BasicBlock::Create(lc, "fe.body", fn);
    BasicBlock* doneBB = BasicBlock::Create(lc, "fe.done", fn);
    b.CreateBr(head);
    b.SetInsertPoint(head);
    LValue* i = b.CreateLoad(i64(), idx);
    // interp は「index を増やしてから index < VALUE」で回すので、
    // 端数の上限は床ではなく「i-1 < n」— 3.5 は 4 回回る（i=4 で
    // 3 < 3.5）。i <= n だと端数で 1 回足りなくなる。
    LValue* go = b.CreateFCmpOLT(
        b.CreateFSub(b.CreateSIToFP(i, f64()),
                     llvm::ConstantFP::get(f64(), 1.0)), n);
    b.CreateCondBr(go, bodyBB, doneBB);
    b.SetInsertPoint(bodyBB);
    b.CreateCall(callee("xyo_var_set", {ptr(), ptr(), ptr(), i64()},
                        b.getVoidTy()),
                 {ctx, idPtr(id), namePtr(name),
                  b.CreateCall(callee("xyo_num", {f64()}, i64()),
                               {b.CreateSIToFP(i, f64())})});
    lowerBlock(*body, bodyBB);
    BasicBlock* bodyEnd = b.GetInsertBlock();
    b.CreateStore(b.CreateAdd(b.CreateLoad(i64(), idx),
                              llvm::ConstantInt::get(i64(), 1)),
                  idx);
    b.CreateBr(head);
    b.SetInsertPoint(doneBB);
}

void Lowerer::emitStop(Op& op) {
    const std::string kind(op.attrStr("kind"));
    if (kind == "all" || kind == "script") {
        b.CreateCall(
            callee(kind == "all" ? "xyo_stop_all_begin"
                                 : "xyo_stop_script_begin",
                   {ptr()}, b.getVoidTy()),
            {ctx});
        b.CreateBr(retBlock());
        // builder が鎖を切る（terminated_）ため後続 op は原則無いが、
        // 終端 xyo.yield と region 側の br が残る。終端済みブロックへ
        // それ以上命令を足さないよう未接続の dead ブロックへ移す。
        b.SetInsertPoint(BasicBlock::Create(lc, "stop.dead", fn));
        return;
    }
    // clone の削除は条件付き（この clone だけ消える）。
    LValue* r = b.CreateCall(callee("xyo_ctl_deleteclone_begin", {ptr()}, i32()),
                             {ctx});
    BasicBlock* del = BasicBlock::Create(lc, "stop.del", fn);
    BasicBlock* cont = BasicBlock::Create(lc, "stop.cont", fn);
    b.CreateCondBr(b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0)), del,
                   cont);
    b.SetInsertPoint(del);
    b.CreateBr(retBlock());
    b.SetInsertPoint(cont);
}

/// compat 層（xyo_rt_compat）に渡すプリミティブが内部で yield し得るか。
///
/// xyo_rt_compat はスレッドの status（YIELD / PROMISE_WAIT 等）を見て
/// xyo_yield するが、**fiberless（mayYield=false の同期実行）では
/// xyo_yield が no-op** になり、スケジューラの時計も同期呼出し中は
/// 進まないため、待機系プリミティブが永久に完了しない
/// （general_bcastwait の control_wait で実測ハング）。
/// legacy codegen は helperMayYield に "xyo_rt_compat" を含めて常に
/// mayYield 扱いしているが、ここでは opcode ごとに絞る。
bool compatPrimitiveMayYield(const BlockCached* bc) {
    if (!bc) return true;
    // 自作拡張（primitiveContext を持つ）は任意の待機をし得るため
    // 保守的に yield 点として数える。
    if (bc->primitiveContext) return true;
    static const std::unordered_set<std::string> k = {
        // sb3_rewrite.cpp の isYieldOp と同じ判定 + 防御的な追加。
        "control_wait", "control_wait_until",
        "event_broadcastandwait",
        "looks_sayforsecs", "looks_thinkforsecs",
        "looks_switchbackdroptoandwait",
        "sound_playuntildone",
        "sound_seteffectto", "sound_changeeffectby",
        "sound_setvolumeto", "sound_changevolumeby",
        "sensing_askandwait",
        "motion_glideto", "motion_glidesecstoxy",
        "control_delete_this_clone",
        // music 拡張の拍系は PROMESE_WAIT の待機（waitSeconds）。
        "music_playNoteForBeats", "music_playDrumForBeats",
        "music_midiPlayDrumForBeats", "music_restForBeats",
        // stop は PROMISE_WAIT/DONE にするため後続を走らせない
        // （compat 経由でも legacy と同じく yield 点として数える）。
        "control_stop",
    };
    return k.count(bc->opcode) != 0;
}

void Lowerer::emitCompat(Op& op, bool asValue) {
    Key k0{&op, 0, false};
    const void* bc = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(op.attrInt("bc")));
    if (compatPrimitiveMayYield(static_cast<const BlockCached*>(bc))) {
        mayYield = true;
        voluntaryYield = true;
    }
    const std::string names(op.attrStr("args"));
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= names.size() && !names.empty()) {
        const size_t end = names.find('\x1e', pos);
        parts.push_back(names.substr(pos, end == std::string::npos
                                             ? std::string::npos
                                             : end - pos));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    llvm::FunctionCallee setarg = callee("xyo_rt_setarg", {ptr(), ptr(), i64()},
                                         b.getVoidTy());
    for (size_t i = 0; i < op.operands.size() && i < parts.size(); ++i) {
        LValue* a = value(op.operands[i]);
        b.CreateCall(setarg, {llvm::ConstantExpr::getIntToPtr(
                                  llvm::ConstantInt::get(i64(),
                                                         reinterpret_cast<uintptr_t>(bc)),
                                  ptr()),
                              idPtr(parts[i]), a});
    }
    LValue* r = b.CreateCall(
        callee("xyo_rt_compat", {ptr(), ptr()}, i64()),
        {ctx, llvm::ConstantExpr::getIntToPtr(
                  llvm::ConstantInt::get(i64(),
                                         reinterpret_cast<uintptr_t>(bc)),
                  ptr())});
    if (asValue) vals[k0] = r;
}

void Lowerer::emitOp(Op& op) {
    const std::string& n = op.name;
    Key k0{&op, 0, false};
    auto box = [&](LValue* x) {
        return b.CreateCall(callee("xyo_num", {f64()}, i64()), {x});
    };
    if (n == "xyo.const") {
        uint64_t bits = static_cast<uint64_t>(op.attrInt("bits"));
        vals[k0] = llvm::ConstantInt::get(i64(), bits);
        return;
    }
    if (n == "xyo.const.f64") {
        const Attr* a = op.attr("d");
        double d = a && a->isDouble() ? a->d : op.attrInt("d");
        vals[k0] = llvm::ConstantFP::get(f64(), d);
        return;
    }
    if (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
        n == "xyo.div" || n == "xyo.mod") {
        static const bool plainHelper = [] {
            const char* e = std::getenv("XYO_MLIR_NONUMFAST");
            return e && e[0] == '1';
        }();
        if (plainHelper) {
            const char* sym = n == "xyo.add" ? "xyo_add"
                            : n == "xyo.sub" ? "xyo_sub"
                            : n == "xyo.mul" ? "xyo_mul"
                            : n == "xyo.div" ? "xyo_div" : "xyo_mod";
            vals[k0] = b.CreateCall(callee(sym, {i64(), i64()}, i64()),
                                    {value(op.operands[0]),
                                     value(op.operands[1])});
            return;
        }
        LValue* fa = numberShadow(op.operands[0]);
        if (!error.empty()) return;
        LValue* fb = numberShadow(op.operands[1]);
        if (!error.empty()) return;
        LValue* r = nullptr;
        if (n == "xyo.add") r = b.CreateFAdd(fa, fb);
        else if (n == "xyo.sub") r = b.CreateFSub(fa, fb);
        else if (n == "xyo.mul") r = b.CreateFMul(fa, fb);
        else if (n == "xyo.div") {
            // 2 の冪の定数除数は乗算に置き換える（xyo.div.f と同じ）。
            double cc = 0.0;
            if (!constF64Of(op.operands[1], &cc) || !isPow2(cc) ||
                !std::isfinite(1.0 / cc))
                r = b.CreateFDiv(fa, fb);
            else
                r = b.CreateFMul(fa,
                                 llvm::ConstantFP::get(f64(), 1.0 / cc));
        } else {
            // xyo.mod.f と同じ u32 整数 fast path + frem フォールバック
            // （sha256 の x mod 2^32 連打は fmod libcall が支配的だった）。
            double cc = 0.0;
            const bool hasC = finiteConst(op.operands[1], &cc);
            r = emitModF64(fa, fb, hasC, cc);
        }
        double cd = 0.0;
        const bool cA = finiteConst(op.operands[0], &cd);
        const bool cB = finiteConst(op.operands[1], &cd);
        bool noNan = false;
        if (n == "xyo.add" || n == "xyo.sub") noNan = cA || cB;
        else if (n == "xyo.mul" || n == "xyo.div")
            noNan = (cA && cd != 0.0) || (cB && cd != 0.0);
        if (noNan) {
            vals[k0] = b.CreateBitCast(r, i64());
            numShadow_[k0] = NumShadowEntry{r, b.GetInsertBlock()};
            return;
        }
        LValue* isNan = b.CreateFCmpUNO(r, r);
        vals[k0] = b.CreateSelect(
            isNan, llvm::ConstantInt::get(i64(), kNumBitsLimit),
            b.CreateBitCast(r, i64()));
        numShadow_[k0] = NumShadowEntry{
            b.CreateSelect(isNan, llvm::ConstantFP::get(f64(), 0.0), r),
            b.GetInsertBlock()};
        return;
    }
    if (n == "xyo.add.f" || n == "xyo.sub.f" || n == "xyo.mul.f" ||
        n == "xyo.div.f" || n == "xyo.mod.f") {
        LValue* a = f64Operand(op.operands[0]);
        LValue* c = f64Operand(op.operands[1]);
        LValue* r = nullptr;
        if (n == "xyo.add.f") r = b.CreateFAdd(a, c);
        else if (n == "xyo.sub.f") r = b.CreateFSub(a, c);
        else if (n == "xyo.mul.f") r = b.CreateFMul(a, c);
        else if (n == "xyo.div.f") {
            // 2 の冪の定数除数は乗算に置き換える（IEEE で厳密に同じ）。
            // ただし逆数が表現できない場合は除算のまま — cc = 2^-1074
            // のような最小 denormal は 1/cc が +inf になり、x*inf で
            // 0 が NaN・有限値が ±inf になる（found: x/'5e-324'）。
            double cc = 0.0;
            if (!constF64Of(op.operands[1], &cc) || !isPow2(cc) ||
                !std::isfinite(1.0 / cc))
                r = b.CreateFDiv(a, c);
            else
                r = b.CreateFMul(a, llvm::ConstantFP::get(f64(), 1.0 / cc));
        } else {
            double cc = 0.0;
            r = emitModF64(a, c, constF64Of(op.operands[1], &cc), cc);
        }
        vals[k0] = r;
        return;
    }
    if (n == "xyo.num_ok") {
        LValue* bits = value(op.operands[0]);
        LValue* f = b.CreateBitCast(bits, f64());
        vals[k0] = b.CreateAnd(
            b.CreateICmpSLT(bits, llvm::ConstantInt::get(i64(), kNumBitsLimit)),
            b.CreateFCmpORD(f, f));
        return;
    }
    if (n == "xyo.int_exact_ok") {
        // 入口ガード: Value が正確な整数か（i1）。
        // nonneg 属性時は 0 <= v <= 2^30、そうでなければ |v| <= 2^30。
        // 成功時は xyo.to_int が正確に i32 を返す。
        if (op.operands.empty()) {
            error = "xyo.int_exact_ok needs 1 operand";
            return;
        }
        LValue* bits = value(op.operands[0]);
        if (!bits) {
            error = "xyo.int_exact_ok operand not lowered";
            return;
        }
        static constexpr double kBound = 1073741824.0;  // 2^30
        const bool nonNeg = op.attrInt("nonneg", 0) != 0;
        LValue* isNum = b.CreateICmpSLT(
            bits, llvm::ConstantInt::get(i64(), kNumBitsLimit));
        // 非数値（数値化できる文字列 "0" 等）も to_num が正確な整数を
        // 返すなら昇格可 — legacy の induction 輸送と同じ判定。
        // repeat/change ループの初期値が文字列リテラルでも int 化する。
        LValue* numF = b.CreateCall(callee("xyo_to_num", {i64()}, f64()),
                                    {bits});
        LValue* f = b.CreateSelect(
            isNum, b.CreateBitCast(bits, f64()), numF);
        LValue* ordered = b.CreateFCmpORD(f, f);
        // -0.0 は整数だが i32 輸送で +0 になるため除外する
        // （1/-0 == -Infinity と 1/+0 == +Infinity の区別のため）。
        // fcmp ではなくビット判定: +0.0 を除くと k_i=0 始まりのループが
        // 入口ガードで全滅し int 昇格が死ぬ。
        LValue* nonZero = b.CreateICmpNE(
            b.CreateBitCast(f, i64()),
            llvm::ConstantInt::get(
                i64(), std::numeric_limits<int64_t>::min()));
        LValue* inRange;
        if (nonNeg) {
            inRange = b.CreateAnd(
                b.CreateFCmpOGE(f, llvm::ConstantFP::get(f64(), 0.0)),
                b.CreateFCmpOLE(f, llvm::ConstantFP::get(f64(), kBound)));
        } else {
            LValue* absv = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, f);
            inRange = b.CreateFCmpOLE(
                absv, llvm::ConstantFP::get(f64(), kBound));
        }
        LValue* trunc = b.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, f);
        LValue* isInt = b.CreateFCmpOEQ(trunc, f);
        // isNum はここでは問わない（f は数値化済み）。素の double で
        // ない正確な整数（正準形の整数文字列 "0" 等）も通す。isNum を
        // 掛けると文字列 entry が全滅し、guard/box が常に else/float 側へ
        // 落ちる（found: suite list_read）。
        vals[k0] = b.CreateAnd(ordered,
                               b.CreateAnd(b.CreateAnd(inRange, isInt),
                                           nonZero));
        return;
    }
    if (n == "xyo.box.f") {
        LValue* d = value(op.operands[0]);
        // non-NaN が証明できる f64 影は canonical 化を省略して bitcast
        // のみ（ループ運搬の heal 済み 'd' で ord+cmov が消える）。
        if (numFacts(op.operands[0]) & 1) {
            vals[k0] = b.CreateBitCast(d, i64());
            return;
        }
        vals[k0] = b.CreateSelect(b.CreateFCmpUNO(d, d),
                                  llvm::ConstantInt::get(i64(), kNumBitsLimit),
                                  b.CreateBitCast(d, i64()));
        return;
    }
    // --- 整数 SSA（i64）。区間証明つきでオーバーフローは起きない ---
    if (n == "xyo.const.i") {
        llvm::Type* t = op.resultTypes.empty() ? i64() : ty(op.resultTypes[0]);
        vals[k0] = llvm::ConstantInt::get(t, op.attrInt("v"), true);
        return;
    }
    if (n == "xyo.add.i" || n == "xyo.sub.i" || n == "xyo.mul.i") {
        if (op.operands.size() != 2) {
            error = "int arith needs 2 operands";
            return;
        }
        LValue* a = value(op.operands[0]);
        LValue* c = value(op.operands[1]);
        if (!a || !c) {
            error = "int arith operand not lowered";
            return;
        }
        if (n == "xyo.add.i") vals[k0] = b.CreateAdd(a, c);
        else if (n == "xyo.sub.i") vals[k0] = b.CreateSub(a, c);
        else vals[k0] = b.CreateMul(a, c);
        return;
    }
    if (n == "xyo.div.i") {
        // 正の定数除数・非負被除数の証明済み除算（切り捨ては floor と一致）。
        if (op.operands.size() != 2) {
            error = "xyo.div.i needs 2 operands";
            return;
        }
        LValue* a = value(op.operands[0]);
        LValue* c = value(op.operands[1]);
        if (!a || !c) {
            error = "xyo.div.i operand not lowered";
            return;
        }
        llvm::ConstantInt* cm = llvm::dyn_cast<llvm::ConstantInt>(c);
        if (!cm || cm->isZero() || cm->isNegative()) {
            error = "xyo.div.i needs positive constant divisor";
            return;
        }
        if (cm->isOne()) {
            vals[k0] = a;
            return;
        }
        // 2 の冪は算術シフト（非負なので切り捨て一致）。
        const uint64_t d = cm->getZExtValue();
        if ((d & (d - 1)) == 0) {
            vals[k0] = b.CreateAShr(a, llvm::ConstantInt::get(
                                           a->getType(), std::log2(d)));
            return;
        }
        vals[k0] = b.CreateSDiv(a, c);
        return;
    }
    if (n == "xyo.mod.i") {
        // 正の定数除数。promoter の区間証明に基づき:
        // subk>=0 なら sub+select の繰り返し（除算器を使わない）、
        // subk==-2 なら urem、そうでなければ srem+符号補正。
        // 除数 0・負除数はありえない（promoter が弾く）が防御的に検証。
        if (op.operands.size() != 2) {
            error = "xyo.mod.i needs 2 operands";
            return;
        }
        LValue* a = value(op.operands[0]);
        LValue* c = value(op.operands[1]);
        if (!a || !c) {
            error = "xyo.mod.i operand not lowered";
            return;
        }
        llvm::ConstantInt* cm = llvm::dyn_cast<llvm::ConstantInt>(c);
        if (!cm || cm->isZero() || cm->isNegative()) {
            error = "xyo.mod.i needs positive constant divisor";
            return;
        }
        const int64_t subk = op.attrInt("subk", -1);
        if (subk >= 0) {
            // r = x; repeat subk times: r = r >= m ? r - m : r.
            // 被除数 < (subk+1)*m が保証されているので正確。
            LValue* r = a;
            for (int64_t k = 0; k < subk; ++k) {
                LValue* over = b.CreateICmpUGE(r, c);
                r = b.CreateSelect(over, b.CreateSub(r, c), r);
            }
            vals[k0] = r;
            return;
        }
        if (subk == -2) {
            vals[k0] = b.CreateURem(a, c);
            return;
        }
        llvm::Type* it = a->getType();
        LValue* r0 = b.CreateSRem(a, c);
        LValue* neg = b.CreateICmpSLT(
            r0, llvm::ConstantInt::get(it, 0));
        vals[k0] = b.CreateSelect(neg, b.CreateAdd(r0, c), r0);
        return;
    }
    if (n == "xyo.cmp.i") {
        const int64_t pred = op.attrInt("pred");
        static const llvm::CmpInst::Predicate kPreds[] = {
            llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SGT,
            llvm::CmpInst::ICMP_SLE, llvm::CmpInst::ICMP_SGE,
            llvm::CmpInst::ICMP_EQ, llvm::CmpInst::ICMP_NE,
            // 6: ULE（符号なし <=）。box 内判定 [0, 2^20] を 1 命令で
            // 行う（負数は符号なしで巨大値になるため自動で偽）。
            llvm::CmpInst::ICMP_ULE};
        if (pred < 0 || pred >= 7) {
            error = "xyo.cmp.i: bad predicate";
            return;
        }
        LValue* a = value(op.operands[0]);
        LValue* c = value(op.operands[1]);
        if (!a || !c) {
            error = "xyo.cmp.i operand not lowered";
            return;
        }
        vals[k0] = b.CreateICmp(kPreds[pred], a, c);
        return;
    }
    if (n == "xyo.from_int") {
        // i32 範囲の証明つき整数 → 素の double ビット（正確・NaN なし）。
        LValue* v = value(op.operands[0]);
        if (!v) {
            error = "xyo.from_int operand not lowered";
            return;
        }
        vals[k0] = b.CreateBitCast(b.CreateSIToFP(v, f64()), i64());
        return;
    }
    if (n == "xyo.to_int") {
        // 呼び出し側が整数値であることを保証する（整数ループの
        // spill/reload 経路専用）。正準形の整数文字列があり得るため、
        // タグを見て素の double だけ高速変換し、それ以外は to_num 経由。
        LValue* bits = value(op.operands[0]);
        if (!bits) {
            error = "xyo.to_int operand not lowered";
            return;
        }
        llvm::Type* dt = op.resultTypes.empty() ? i64() : ty(op.resultTypes[0]);
        // !xyo.f64 期待でない値が f64 降格されて来た場合は直接変換
        // （numberShadow と同じく toNumber 境界なので NaN→0 相当の
        // 素直な fptosi でよい: f64 NaN は Value 経路なら to_num=0）。
        if (bits->getType() == f64()) {
            LValue* z = b.CreateSelect(
                b.CreateFCmpUNO(bits, bits),
                llvm::ConstantFP::get(f64(), 0.0), bits);
            vals[k0] = b.CreateFPToSI(z, dt);
            return;
        }
        vals[k0] = toIntChecked(bits, dt);
        return;
    }
    if (n == "xyo.cmp.f") {
        const int64_t pred = op.attrInt("pred");
        LValue* a = f64Operand(op.operands[0]);
        LValue* c = f64Operand(op.operands[1]);
        static const llvm::CmpInst::Predicate kPreds[] = {
            llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OGT,
            llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGE,
            llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_ONE};
        if (pred < 0 || pred >= 6) {
            error = "xyo.cmp.f: bad predicate";
            return;
        }
        vals[k0] = b.CreateFCmp(kPreds[pred], a, c);
        return;
    }
    if (n == "xyo.cmp.guarded") {
        const int64_t pred = op.attrInt("pred");
        // letter of idx of s == <1 文字定数> は文字列を作らずに比較する。
        if (pred == 4) {
            auto oneChar = [&](const Value& v, int32_t* ch) -> bool {
                if (!v.op || v.op->name != "xyo.const") return false;
                const Attr* a = v.op->attr("bits");
                if (!a || !a->isInt()) return false;
                const xyo::Value cv{static_cast<uint64_t>(a->i)};
                if (!cv.isString()) return false;
                const SStr* str = cv.asStrPtr();
                if (!str || str->len != 1) return false;
                *ch = static_cast<int32_t>(str->unitAt(0));
                return true;
            };
            for (int side = 0; side < 2; ++side) {
                const Value& a0 = op.operands[side];
                const Value& a1 = op.operands[1 - side];
                if (!a0.op || a0.op->name != "xyo.letterof" ||
                    a0.op->operands.size() != 2)
                    continue;
                int32_t ch = 0;
                if (!oneChar(a1, &ch)) continue;
                // 添字が整数 SSA なら toNumber を挟まない版を使う。
                Value idx0 = a0.op->operands[0];
                // 値として box された整数（from_int）は i32 を取り出す。
                if (idx0.op && idx0.op->name == "xyo.from_int" &&
                    !idx0.op->operands.empty())
                    idx0 = idx0.op->operands[0];
                const Type it = (idx0.op && idx0.index <
                                 idx0.op->resultTypes.size())
                                    ? idx0.op->resultTypes[idx0.index]
                                    : idx0.type;
                LValue* r = nullptr;
                if (it == Type::i32()) {
                    r = b.CreateCall(
                        callee("xyo_letter_eq_i", {i32(), i64(), i32()}, i32()),
                        {value(idx0), value(a0.op->operands[1]),
                         llvm::ConstantInt::get(i32(), ch)});
                } else {
                    r = b.CreateCall(
                        callee("xyo_letter_eq", {i64(), i64(), i32()}, i32()),
                        {value(idx0), value(a0.op->operands[1]),
                         llvm::ConstantInt::get(i32(), ch)});
                }
                vals[k0] = b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0));
                return;
            }
        }
        static const llvm::CmpInst::Predicate kPreds[] = {
            llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OGT,
            llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGE,
            llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_ONE};
        const char* sym = pred == 0 ? "xyo_lt"
                        : pred == 1 ? "xyo_gt"
                        : pred == 4 ? "xyo_eq" : nullptr;
        if (pred < 0 || pred >= 6 || !sym) {
            error = "xyo.cmp.guarded: unsupported predicate";
            return;
        }
        LValue* af = value(op.operands[2]);
        LValue* bf = value(op.operands[3]);
        LValue* limit = llvm::ConstantInt::get(i64(), kNumBitsLimit);
        // ビット側が「常に素の double」と分かる値ならタグ検査は不要。
        // from_int は常に正準な数値、box.f は NaN のときだけ NaN ビットに
        // なる（d の fcmp ord が同じことを見るので冗長）。
        auto plainBits = [](const Value& v) -> bool {
            if (!v.op) return false;
            const std::string& n = v.op->name;
            if (n == "xyo.from_int" || n == "xyo.to_int") return true;
            if (n == "xyo.box.f") return true;
            // numeric/intvalued リストからの読みは素の double が確定
            // （frontend が全書き込みを数値と証明）。タグ検査不要。
            if ((n == "xyo.list.item" || n == "xyo.list.item.i") &&
                !v.op->operands.empty() && v.op->operands[0].op &&
                v.op->operands[0].op->name == "xyo.list.ptr") {
                Op* ptr = v.op->operands[0].op;
                if (ptr->attrInt("numeric", 0) != 0 ||
                    ptr->attrInt("intvalued", 0) != 0)
                    return true;
            }
            if (n == "xyo.const") {
                const Attr* a = v.op->attr("bits");
                if (!a || !a->isInt()) return false;
                const uint64_t cb = static_cast<uint64_t>(a->i);
                const xyo::Value cv{cb};
                if (cv.isNumber()) return true;
                // numericCompareOperand と同じ判定で数値文字列定数
                // （numState 1=数値, 4=Infinity リテラル）だけ畳む。
                // "false" や空白文字列は to_num しても 0 になるが
                // 比較では文字列扱いなので fast に乗せられない。
                if (!cv.isString()) return false;
                SStr* cs = cv.asStrPtr();
                if (!cs) return false;
                uint32_t st = cs->loadNumState();
                if (st == 0) {
                    (void)cast::parseNumberStringUncached(cs);
                    st = cs->loadNumState();
                }
                return st == 1 || st == 4;
            }
            return false;
        };
        auto sideOk = [&](const Value& bits, LValue* f) -> LValue* {
            if (plainBits(bits))
                return llvm::ConstantInt::get(b.getInt1Ty(), 1);
            return b.CreateAnd(b.CreateICmpSLT(value(bits), limit),
                               b.CreateFCmpORD(f, f));
        };
        // 片側が数値文字列の定数（"1000" 等）のとき、静的には
        // 「素の double」と言えず側判定が落ちて slow の xyo_* 呼出しが
        // 毎反復走る。文字列が数値として読めて、かつ相手が確定数値なら
        // 数値比較で合流できる（Scratch では両側が数値化可能なら
        // 数値比較 — 定数側は to_num で読めることを確認済み）。
        auto constNumericStr = [&](const Value& v) -> bool {
            if (!v.op || v.op->name != "xyo.const") return false;
            const Attr* a = v.op->attr("bits");
            if (!a || !a->isInt()) return false;
            const uint64_t cb = static_cast<uint64_t>(a->i);
            const xyo::Value cv{cb};
            if (cv.isNumber()) return true;
            if (!cv.isString()) return false;
            return !std::isnan(xyo_to_num(cb));
        };
        LValue* ok = b.CreateAnd(sideOk(op.operands[0], af),
                                 sideOk(op.operands[1], bf));
        BasicBlock* fast = BasicBlock::Create(lc, "cmpg.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "cmpg.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "cmpg.join", fn);
        b.CreateCondBr(ok, fast, slow, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* fr = b.CreateFCmp(kPreds[pred], af, bf);
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        // bits は slow 経路でだけ materialize する（box.f producer を
        // fast 側で作らない）。
        LValue* abits = slowBits(op.operands[0]);
        LValue* bbits = slowBits(op.operands[1]);
        LValue* sr = b.CreateICmpNE(
            b.CreateCall(callee(sym, {i64(), i64()}, i32()), {abits, bbits}),
            llvm::ConstantInt::get(i32(), 0));
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(b.getInt1Ty(), 2);
        phi->addIncoming(fr, fastEnd);
        phi->addIncoming(sr, slowEnd);
        vals[k0] = phi;
        return;
    }
    if (n == "xyo.eq") {
        // letter of idx of s == <1 文字定数> は文字列を作らずに比較する。
        auto oneChar = [&](const Value& v, int32_t* ch) -> bool {
            if (!v.op || v.op->name != "xyo.const") return false;
            const Attr* a = v.op->attr("bits");
            if (!a || !a->isInt()) return false;
            const xyo::Value cv{static_cast<uint64_t>(a->i)};
            if (!cv.isString()) return false;
            const SStr* str = cv.asStrPtr();
            if (!str || str->len != 1) return false;
            *ch = static_cast<int32_t>(str->unitAt(0));
            return true;
        };
        for (int side = 0; side < 2; ++side) {
            const Value& a0 = op.operands[side];
            const Value& a1 = op.operands[1 - side];
            if (!a0.op || a0.op->name != "xyo.letterof" ||
                a0.op->operands.size() != 2)
                continue;
            int32_t ch = 0;
            if (!oneChar(a1, &ch)) continue;
            // 添字が整数 SSA なら toNumber を挟まない版を使う。
            Value idx0 = a0.op->operands[0];
            if (idx0.op && idx0.op->name == "xyo.from_int" &&
                !idx0.op->operands.empty())
                idx0 = idx0.op->operands[0];
            const Type it = (idx0.op && idx0.index <
                             idx0.op->resultTypes.size())
                                ? idx0.op->resultTypes[idx0.index]
                                : idx0.type;
            LValue* r = nullptr;
            if (it == Type::i32()) {
                r = b.CreateCall(
                    callee("xyo_letter_eq_i", {i32(), i64(), i32()}, i32()),
                    {value(idx0), value(a0.op->operands[1]),
                     llvm::ConstantInt::get(i32(), ch)});
            } else {
                r = b.CreateCall(
                    callee("xyo_letter_eq", {i64(), i64(), i32()}, i32()),
                    {value(idx0), value(a0.op->operands[1]),
                     llvm::ConstantInt::get(i32(), ch)});
            }
            vals[k0] = b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0));
            return;
        }
    }
    if (n == "xyo.lt" || n == "xyo.gt" || n == "xyo.eq") {
        const char* sym = n == "xyo.lt" ? "xyo_lt"
                        : n == "xyo.gt" ? "xyo_gt" : "xyo_eq";
        // const の数値文字列（Scratch の数値リテラル shadow）は実行時
        // タグ検査を必ず失敗する（SStr* ポインタのため）。数値演算では
        // to_num と同じ値になるので、コンパイル時に数値化して plain
        // double として扱う（fib の `n < 2` 等が毎回 helper になっていた）。
        auto constNum = [&](const Value& v, double* out) -> bool {
            if (!v.op) return false;
            if (v.op->name == "xyo.const.f64") {
                const Attr* a = v.op->attr("d");
                if (!a || !a->isDouble()) return false;
                *out = a->d;
                return true;
            }
            if (v.op->name != "xyo.const") return false;
            const Attr* at = v.op->attr("bits");
            if (!at || !at->isInt()) return false;
            const xyo::Value cv{static_cast<uint64_t>(at->i)};
            if (cv.isNumber()) {
                *out = cv.asNumberUnchecked();
                return true;
            }
            if (!cv.isString()) return false;
            SStr* str = cv.asStrPtr();
            if (!str) return false;
            // 数値として読めなければ数値演算でもない（呼ばない）。
            uint32_t st = str->loadNumState();
            if (st == 0) {
                (void)cast::parseNumberStringUncached(str);
                st = str->loadNumState();
            }
            if (st != 1 && st != 4) return false;
            *out = str->loadNumCache();
            return true;
        };
        LValue* a = value(op.operands[0]);
        LValue* c = value(op.operands[1]);
        double da = 0.0, dc = 0.0;
        const bool aConst = constNum(op.operands[0], &da);
        const bool cConst = constNum(op.operands[1], &dc);

        // 両辺とも素の数値定数（NaN 除外）なら畳む。文字列定数は
        // bits 一致や文字列比較があり得るので runtime に残す。
        if (aConst && cConst && !std::isnan(da) && !std::isnan(dc)) {
            // 素の double 同士か確認（文字列由来は畳まない）。
            bool aPlain = false, cPlain = false;
            if (op.operands[0].op) {
                const Attr* at = op.operands[0].op->name == "xyo.const.f64"
                    ? op.operands[0].op->attr("d")
                    : op.operands[0].op->attr("bits");
                if (at) {
                    if (op.operands[0].op->name == "xyo.const.f64")
                        aPlain = true;
                    else {
                        const xyo::Value cv{
                            static_cast<uint64_t>(at->i)};
                        aPlain = cv.isNumber();
                    }
                }
            }
            if (op.operands[1].op) {
                const Attr* at = op.operands[1].op->name == "xyo.const.f64"
                    ? op.operands[1].op->attr("d")
                    : op.operands[1].op->attr("bits");
                if (at) {
                    if (op.operands[1].op->name == "xyo.const.f64")
                        cPlain = true;
                    else {
                        const xyo::Value cv{
                            static_cast<uint64_t>(at->i)};
                        cPlain = cv.isNumber();
                    }
                }
            }
            if (aPlain && cPlain) {
                bool r = false;
                if (n == "xyo.eq") r = (da == dc);
                else if (n == "xyo.lt") r = (da < dc);
                else r = (da > dc);
                vals[k0] = llvm::ConstantInt::get(b.getInt1Ty(), r ? 1 : 0);
                return;
            }
        }
        // 両辺が「素の double」（符号付きで kNan 未満）なら helper の数値
        // 経路と厳密に同じ結果になるので、呼び出しを fcmp に置き換える。
        //   xyo_lt/xyo_gt = cast::compare = 両方数値なら数値比較
        //   xyo_eq        = bits 一致 → 真、次に両方数値なら数値比較
        // NaN は kNan そのもの（タグ域）なので fast path に入らず helper に
        // 落ちる。eq だけは helper と同じ bits 一致の早期真を足す。
        static const bool inlineCmp = [] {
            const char* e = std::getenv("XYO_MLIR_NOINLINECMP");
            return !(e && e[0] == '1');
        }();
        if (inlineCmp) {
            LValue* limit = llvm::ConstantInt::get(i64(), kNumBitsLimit);
            BasicBlock* fast = BasicBlock::Create(lc, "cmp.fast", fn);
            BasicBlock* slow = BasicBlock::Create(lc, "cmp.slow", fn);
            BasicBlock* join = BasicBlock::Create(lc, "cmp.join", fn);
            // 定数側が数値（数値文字列含む）なら tag 検査不要。
            // var 側だけ検査すれば fast/slow が決まる。
            LValue* aOk = aConst ? llvm::ConstantInt::get(b.getInt1Ty(), 1)
                                 : b.CreateICmpSLT(a, limit);
            LValue* cOk = cConst ? llvm::ConstantInt::get(b.getInt1Ty(), 1)
                                 : b.CreateICmpSLT(c, limit);
            LValue* both = b.CreateAnd(aOk, cOk);
            b.CreateCondBr(both, fast, slow, llvm::MDNode::get(lc, {
                llvm::MDString::get(lc, "branch_weights"),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64(), 63)),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64(), 1))}));
            b.SetInsertPoint(fast);
            LValue* fa = aConst ? llvm::ConstantFP::get(f64(), da)
                                : b.CreateBitCast(a, f64());
            LValue* fc = cConst ? llvm::ConstantFP::get(f64(), dc)
                                : b.CreateBitCast(c, f64());
            LValue* fr = n == "xyo.eq" ? b.CreateFCmpOEQ(fa, fc)
                      : n == "xyo.lt" ? b.CreateFCmpOLT(fa, fc)
                                      : b.CreateFCmpOGT(fa, fc);
            BasicBlock* fastEnd = b.GetInsertBlock();
            b.CreateBr(join);
            b.SetInsertPoint(slow);
            LValue* r = b.CreateCall(callee(sym, {i64(), i64()}, i32()),
                                     {a, c});
            LValue* sr = b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0));
            BasicBlock* slowEnd = b.GetInsertBlock();
            b.CreateBr(join);
            b.SetInsertPoint(join);
            auto* phi = b.CreatePHI(i1(), 2);
            phi->addIncoming(fr, fastEnd);
            phi->addIncoming(sr, slowEnd);
            vals[k0] = n == "xyo.eq"
                ? b.CreateOr(phi, b.CreateICmpEQ(a, c))
                : phi;
            return;
        }
        LValue* r = b.CreateCall(callee(sym, {i64(), i64()}, i32()), {a, c});
        vals[k0] = b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0));
        return;
    }
    if (n == "xyo.and" || n == "xyo.or" || n == "xyo.not" ||
        n == "xyo.band" || n == "xyo.bor" || n == "xyo.bnot") {
        // Scratch の and/or/not は条件（i1）同士の論理演算。
        LValue* a = value(op.operands[0]);
        LValue* c = op.operands.size() > 1 ? value(op.operands[1]) : nullptr;
        const bool isNot = n == "xyo.not" || n == "xyo.bnot";
        const bool isAnd = n == "xyo.and" || n == "xyo.band";
        if (isNot) vals[k0] = b.CreateNot(a);
        else if (isAnd) vals[k0] = b.CreateAnd(a, c);
        else vals[k0] = b.CreateOr(a, c);
        return;
    }
    if (n == "xyo.boolval") {
        LValue* c = value(op.operands[0]);
        vals[k0] = b.CreateSelect(c,
                                  llvm::ConstantInt::get(i64(), 0x7FF8000000000002ull),
                                  llvm::ConstantInt::get(i64(), 0x7FF8000000000001ull));
        return;
    }
    if (n == "xyo.truthy") {
        LValue* r = b.CreateCall(callee("xyo_truthy", {i64()}, i32()),
                                 {value(op.operands[0])});
        vals[k0] = b.CreateICmpNE(r, llvm::ConstantInt::get(i32(), 0));
        return;
    }
    if (n == "xyo.warp_yield") {
        static const bool noYieldCheck = [] {
            const char* e = std::getenv("XYO_MLIR_NOYIELD");
            return e && e[0] == '1';
        }();
        if (noYieldCheck) {
            for (size_t i = 0; i < op.resultTypes.size() &&
                               i < op.operands.size(); ++i)
                vals[Key{&op, static_cast<uint32_t>(i), false}] =
                    value(op.operands[i]);
            return;
        }
        // チャンク展開の途中コピー: 検査せずオペランドを通すだけ。
        if (suppressWarpYield_ || &op == suppressWarpYieldOp_) {
            for (size_t i = 0; i < op.resultTypes.size() &&
                               i < op.operands.size(); ++i)
                vals[Key{&op, static_cast<uint32_t>(i), false}] =
                    value(op.operands[i]);
            return;
        }
        // 意味: 昇格した変数群 (bits, f64 影) を実メモリへ spill し、本当に
        // 譲る 64 回に 1 回だけ xyo_loop_yield を呼んで reload する。
        const std::string vars(op.attrStr("vars"));
        std::vector<std::pair<std::string, std::string>> namePairs;
        size_t pos = 0;
        while (pos <= vars.size() && !vars.empty()) {
            const size_t end = vars.find('\x1e', pos);
            const std::string entry =
                vars.substr(pos, end == std::string::npos
                                     ? std::string::npos
                                     : end - pos);
            const size_t sep = entry.find('\x1f');
            if (sep == std::string::npos) {
                error = "xyo.warp_yield: malformed vars";
                return;
            }
            namePairs.emplace_back(entry.substr(0, sep), entry.substr(sep + 1));
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        mayYield = true;
        llvm::AllocaInst* cnt = yieldCounter();
        LValue* prev = b.CreateLoad(i64(), cnt);
        LValue* step =
            warpStepOverride_
                ? warpStepOverride_
                : static_cast<LValue*>(
                      llvm::ConstantInt::get(i64(), warpChunkStep_));
        LValue* n = b.CreateAdd(prev, step);
        b.CreateStore(n, cnt);
        BasicBlock* pre = b.GetInsertBlock();
        BasicBlock* dueBB = BasicBlock::Create(lc, "wyield.due", fn);
        BasicBlock* done = BasicBlock::Create(lc, "wyield.done", fn);
        // 64 回ごとの検査: 直前位置に step を足して 64 の倍数を跨いだとき。
        // step==1 では従来の (n&63)==0 と同値。端数のバッチでも跨いだ
        // ときだけ検査する（跨がなければ検査しない）。
        LValue* crossed = b.CreateAdd(
            b.CreateAnd(prev, llvm::ConstantInt::get(i64(), 63)), step);
        LValue* due = b.CreateICmpUGE(
            crossed, llvm::ConstantInt::get(i64(), 64));
        // 変数ごとの運搬表現（kinds="i"/"d"/"b"）を解釈する。kinds が
        // 無い古い IR は従来の判定（全部整数 / 全部 d / (bits,d)）に落とす。
        std::string kinds(op.attrStr("kinds"));
        std::vector<char> kind(namePairs.size(), 'b');
        bool kindsOk = kinds.size() == namePairs.size();
        for (size_t i = 0; kindsOk && i < namePairs.size(); ++i) {
            if (kinds[i] != 'i' && kinds[i] != 'd' && kinds[i] != 'b')
                kindsOk = false;
            else
                kind[i] = kinds[i];
        }
        if (!kindsOk) {
            bool allInt = namePairs.size() == op.operands.size() &&
                          !op.operands.empty();
            for (const Value& v : op.operands) {
                if (v.type != Type::i32() && v.type != Type::i64()) {
                    allInt = false;
                    break;
                }
            }
            const bool dOnlyLegacy =
                !allInt && namePairs.size() == op.operands.size();
            for (size_t i = 0; i < namePairs.size(); ++i)
                kind[i] = allInt ? 'i' : (dOnlyLegacy ? 'd' : 'b');
        }
        size_t oi = 0;
        std::vector<LValue*> preBits(namePairs.size(), nullptr);
        std::vector<LValue*> preD(namePairs.size(), nullptr);
        std::vector<LValue*> inVals(namePairs.size(), nullptr);
        std::vector<llvm::Type*> intTys(namePairs.size(), nullptr);
        for (size_t i = 0; i < namePairs.size(); ++i) {
            if (kind[i] == 'i') {
                if (oi >= op.operands.size()) {
                    error = "xyo.warp_yield: kinds mismatch";
                    return;
                }
                LValue* iv = value(op.operands[oi++]);
                inVals[i] = iv;
                intTys[i] = iv->getType();
                // 整数ループ: i64/i32 を spill する。スロットには正確な
                // double ビットを書く（変数スロットの契約）。
                preBits[i] = b.CreateBitCast(b.CreateSIToFP(iv, f64()), i64());
            } else if (kind[i] == 'd') {
                if (oi >= op.operands.size()) {
                    error = "xyo.warp_yield: kinds mismatch";
                    return;
                }
                preD[i] = value(op.operands[oi++]);
            } else {
                if (oi + 1 >= op.operands.size()) {
                    error = "xyo.warp_yield: kinds mismatch";
                    return;
                }
                preBits[i] = value(op.operands[oi++]);
                preD[i] = value(op.operands[oi++]);
            }
        }
        if (oi != op.operands.size()) {
            error = "xyo.warp_yield: kinds mismatch";
            return;
        }
        b.CreateCondBr(due, dueBB, done, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(dueBB);
        std::vector<std::string> ids, names;
        for (auto& [id, name] : namePairs) {
            ids.push_back(id);
            names.push_back(name);
        }
        // 実際にスケジューラへ譲る場合だけ spill/reload する
        //（譲らない限りレジスタのまま回り、メモリ往復も to_num も
        // 発生しない）。検査自体は sampled warp clock（アトミック
        // 2 ロード）か xyo_warp_yield_due。
        // ハンドオフ時は slots が最新になる（GC ルートとしても正しい）。
        BasicBlock* handBB = BasicBlock::Create(lc, "wyield.handoff", fn);
        BasicBlock* noHandBB =
            emitWarpDueCheck(handBB, done, /*warpKnown=*/false);
        b.SetInsertPoint(handBB);
        // 数値のみの変数は実際に譲る 1 回だけ box.f でビットを作る。
        for (size_t i = 0; i < namePairs.size(); ++i) {
            if (kind[i] != 'd') continue;
            preBits[i] = b.CreateSelect(
                b.CreateFCmpUNO(preD[i], preD[i]),
                llvm::ConstantInt::get(i64(), kNumBitsLimit),
                b.CreateBitCast(preD[i], i64()));
        }
        std::vector<LValue*> reloadedBits =
            spillAndYieldBits(ids, names, preBits);
        std::vector<LValue*> reloadedD(namePairs.size(), nullptr);
        std::vector<LValue*> reloadedI(namePairs.size(), nullptr);
        for (size_t i = 0; i < namePairs.size(); ++i) {
            if (kind[i] == 'i') {
                if (op.attrInt("box", 0) != 0) {
                    // ボックス版: yield 中に他スクリプトが非整数・範囲外を
                    // 書いた可能性があるため、reload で検証する。無効なら
                    // ボックス外の番兵 INT32_MIN を入れてループ条件を偽に
                    // し、直後の float ループへ引き継ぐ（意味論は不変）。
                    static constexpr double kBox = 1048576.0;
                    LValue* bits = reloadedBits[i];
                    LValue* f = b.CreateBitCast(bits, f64());
                    LValue* isNum = b.CreateICmpSLT(
                        bits, llvm::ConstantInt::get(i64(), kNumBitsLimit));
                    LValue* ok = b.CreateAnd(
                        isNum, b.CreateFCmpORD(f, f));
                    ok = b.CreateAnd(
                        ok, b.CreateFCmpOGE(f,
                                            llvm::ConstantFP::get(f64(), 0.0)));
                    ok = b.CreateAnd(
                        ok, b.CreateFCmpOLE(f,
                                            llvm::ConstantFP::get(f64(), kBox)));
                    ok = b.CreateAnd(
                        ok, b.CreateFCmpONE(f,
                                            llvm::ConstantFP::get(f64(), 0.0)));
                    ok = b.CreateAnd(
                        ok, b.CreateFCmpOEQ(
                                b.CreateUnaryIntrinsic(
                                    llvm::Intrinsic::trunc, f), f));
                    LValue* safe = b.CreateSelect(
                        ok, f, llvm::ConstantFP::get(f64(), 0.0));
                    LValue* conv = b.CreateFPToSI(safe, intTys[i] ? intTys[i]
                                                                  : i64());
                    reloadedI[i] = b.CreateSelect(
                        ok, conv,
                        llvm::ConstantInt::get(conv->getType(),
                                               INT32_MIN, /*isSigned=*/true));
                } else {
                    // 健全性は promoter が保証する（全書き込みが整数・範囲内）。
                    // 素の double なら直接 fptosi、正準文字列は to_num 経由。
                    reloadedI[i] = toIntChecked(reloadedBits[i], intTys[i]);
                }
            } else {
                reloadedD[i] = b.CreateCall(
                    callee("xyo_to_num", {i64()}, f64()), {reloadedBits[i]});
            }
        }
        BasicBlock* handEnd = b.GetInsertBlock();
        b.CreateBr(done);
        b.SetInsertPoint(done);
        size_t ri = 0;
        for (size_t i = 0; i < namePairs.size(); ++i) {
            if (kind[i] == 'i') {
                auto* iPhi = b.CreatePHI(intTys[i], 3);
                iPhi->addIncoming(inVals[i], pre);
                iPhi->addIncoming(inVals[i], noHandBB);
                iPhi->addIncoming(reloadedI[i], handEnd);
                vals[keyOf(op.result(ri++))] = iPhi;
                continue;
            }
            if (kind[i] == 'd') {
                auto* dPhi = b.CreatePHI(f64(), 3);
                dPhi->addIncoming(preD[i], pre);
                dPhi->addIncoming(preD[i], noHandBB);
                dPhi->addIncoming(reloadedD[i], handEnd);
                vals[keyOf(op.result(ri++))] = dPhi;
                continue;
            }
            auto* bitsPhi = b.CreatePHI(i64(), 3);
            bitsPhi->addIncoming(preBits[i], pre);
            bitsPhi->addIncoming(preBits[i], noHandBB);
            bitsPhi->addIncoming(reloadedBits[i], handEnd);
            vals[keyOf(op.result(ri++))] = bitsPhi;
            auto* dPhi = b.CreatePHI(f64(), 3);
            dPhi->addIncoming(preD[i], pre);
            dPhi->addIncoming(preD[i], noHandBB);
            dPhi->addIncoming(reloadedD[i], handEnd);
            vals[keyOf(op.result(ri++))] = dPhi;
        }
        return;
    }
    if (n == "xyo.bcastwait") {
        // broadcast and wait: begin してから step が 0 を返すまで待つ。
        // 1 = yield して再試行、2 = warp のビジーポーリング。
        if (op.operands.size() != 1) {
            error = "xyo.bcastwait needs 1 operand";
            return;
        }
        b.CreateCall(callee("xyo_bcastwait_begin_v", {ptr(), i64()},
                            b.getVoidTy()),
                     {ctx, value(op.operands[0])});
        BasicBlock* head = BasicBlock::Create(lc, "bwait.head", fn);
        BasicBlock* afterZero = BasicBlock::Create(lc, "bwait.chk", fn);
        BasicBlock* yieldBB = BasicBlock::Create(lc, "bwait.yield", fn);
        BasicBlock* busyBB = BasicBlock::Create(lc, "bwait.busy", fn);
        BasicBlock* done = BasicBlock::Create(lc, "bwait.done", fn);
        b.CreateBr(head);
        b.SetInsertPoint(head);
        LValue* r = b.CreateCall(callee("xyo_bcastwait_step", {ptr()}, i32()),
                                 {ctx});
        b.CreateCondBr(b.CreateICmpEQ(r, llvm::ConstantInt::get(i32(), 0)),
                       done, afterZero);
        b.SetInsertPoint(afterZero);
        b.CreateCondBr(b.CreateICmpEQ(r, llvm::ConstantInt::get(i32(), 1)),
                       yieldBB, busyBB);
        b.SetInsertPoint(yieldBB);
        b.CreateCall(callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
        b.CreateBr(head);
        b.SetInsertPoint(busyBB);
        b.CreateBr(head);
        b.SetInsertPoint(done);
        mayYield = true;
        voluntaryYield = true;
        return;
    }
    if (n == "xyo.select") {
        vals[k0] = b.CreateSelect(value(op.operands[0]),
                                  value(op.operands[1]),
                                  value(op.operands[2]));
        return;
    }
    if (n == "xyo.to_num") {
        vals[k0] = numberShadow(op.operands[0]);
        return;
    }
    if (n == "xyo.from_num") {
        vals[k0] = box(value(op.operands[0]));
        return;
    }
    if (n == "xyo.round" || n == "xyo.round.f") {
        // Math.round をインライン展開する（呼び出し境界を消す）。
        // jsnum::jsRound と同値: floor(x+0.5)、ただし
        //   0 < x < 0.5     -> +0（0.49999999999999994 対策）
        //   -0.5 <= x < 0   -> -0
        //   x == 0          -> x（符号保持）
        LValue* x = n == "xyo.round" ? numberShadow(op.operands[0])
                                     : f64Operand(op.operands[0]);
        if (!x) {
            error = "xyo.round: operand not lowered";
            return;
        }
        LValue* zero = llvm::ConstantFP::get(f64(), 0.0);
        LValue* negZero = llvm::ConstantFP::get(f64(), -0.0);
        LValue* r = b.CreateUnaryIntrinsic(
            llvm::Intrinsic::floor, b.CreateFAdd(x, llvm::ConstantFP::get(f64(), 0.5)));
        LValue* c1 = b.CreateAnd(
            b.CreateFCmpOGT(x, zero),
            b.CreateFCmpOLT(x, llvm::ConstantFP::get(f64(), 0.5)));
        r = b.CreateSelect(c1, zero, r);
        LValue* c2 = b.CreateAnd(
            b.CreateFCmpOGE(x, llvm::ConstantFP::get(f64(), -0.5)),
            b.CreateFCmpOLT(x, zero));
        r = b.CreateSelect(c2, negZero, r);
        r = b.CreateSelect(b.CreateFCmpOEQ(x, zero), x, r);
        vals[k0] = n == "xyo.round" ? box(r) : r;
        return;
    }
    if (n == "xyo.mathop" || n == "xyo.mathop.f") {
        // op が確定しているので switch を持つ汎用 helper ではなく、
        // op 専用の関数を直接呼ぶ（呼び出しごとの分岐と ctx 受け渡しを削減）。
        static const char* const kMathopSyms[14] = {
            "xyo_math_abs",  "xyo_math_floor", "xyo_math_ceil",
            "xyo_math_sqrt", "xyo_math_sin",   "xyo_math_cos",
            "xyo_math_tan",  "xyo_math_asin",  "xyo_math_acos",
            "xyo_math_atan", "xyo_math_ln",    "xyo_math_log",
            "xyo_math_exp",  "xyo_math_pow10",
        };
        const int64_t fn = op.attrInt("op");
        LValue* x = n == "xyo.mathop" ? numberShadow(op.operands[0])
                                      : f64Operand(op.operands[0]);
        if (!x) {
            error = "xyo.mathop: operand not lowered";
            return;
        }
        LValue* r;
        // tan 以外の三角関数は legacy と同じく libm 呼び出し＋丸めを
        // インライン展開する（呼び出し境界が消え、ループが最適化できる）。
        auto libm1 = [&](const char* nm, LValue* a0) {
            return b.CreateCall(callee(nm, {f64()}, f64()), {a0});
        };
        if (fn == 4 || fn == 5) {
            // round(f(pi*x/180)*1e10)/1e10（掛け順は Scratch と同じ）。
            LValue* rad = b.CreateFDiv(
                b.CreateFMul(llvm::ConstantFP::get(f64(), 3.14159265358979323846),
                             x),
                llvm::ConstantFP::get(f64(), 180.0));
            LValue* v = libm1(fn == 4 ? "sin" : "cos", rad);
            v = b.CreateUnaryIntrinsic(
                llvm::Intrinsic::round,
                b.CreateFMul(v, llvm::ConstantFP::get(f64(), 1e10)));
            LValue* rounded =
                b.CreateFDiv(v, llvm::ConstantFP::get(f64(), 1e10));
            if (fn == 4)
                rounded = b.CreateSelect(
                    b.CreateFCmpOEQ(x, llvm::ConstantFP::get(f64(), 0.0)),
                    llvm::ConstantFP::get(f64(), 0.0), rounded);
            vals[k0] = n == "xyo.mathop" ? box(rounded) : rounded;
            return;
        }
        // abs/floor/ceil/sqrt は LLVM の組込み命令と厳密に一致するので
        // intrinsic に落とす（呼び出しが消え、ループが純浮動小数になる）。
        if (fn == 0) {
            r = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
            vals[k0] = n == "xyo.mathop" ? box(r) : r;
            return;
        }
        if (fn == 1) {
            r = b.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
            vals[k0] = n == "xyo.mathop" ? box(r) : r;
            return;
        }
        if (fn == 2) {
            r = b.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
            vals[k0] = n == "xyo.mathop" ? box(r) : r;
            return;
        }
        if (fn == 3) {
            r = b.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, x);
            vals[k0] = n == "xyo.mathop" ? box(r) : r;
            return;
        }
        if (fn >= 0 && fn < 14) {
            r = b.CreateCall(callee(kMathopSyms[fn], {ptr(), f64()}, f64()),
                             {ctx, x});
        } else {
            r = b.CreateCall(
                callee("xyo_mathop", {ptr(), i32(), f64()}, f64()),
                {ctx, llvm::ConstantInt::get(i32(), fn), x});
        }
        vals[k0] = n == "xyo.mathop" ? box(r) : r;
        return;
    }
    if (n == "xyo.join" || n == "xyo.letterof" || n == "xyo.strlen" ||
        n == "xyo.strcontains") {
        // 4 つとも Value（i64 ビット）を返す。
        if (n == "xyo.join") {
            const Value& rhs = op.operands[1];
            const bool rhsLiteral =
                rhs.op && rhs.op->name == "xyo.const" &&
                (static_cast<uint64_t>(rhs.op->attrInt("bits")) >> 48) ==
                    (static_cast<uint64_t>(val::kTagStr) >> 48);
            if (rhsLiteral) {
                vals[k0] = b.CreateCall(
                    callee("xyo_join_suffix", {i64(), i64()}, i64()),
                    {value(op.operands[0]), value(rhs)});
                return;
            }
        }
        const char* sym = n == "xyo.join" ? "xyo_join"
                        : n == "xyo.letterof" ? "xyo_letterof"
                        : n == "xyo.strlen" ? "xyo_strlen" : "xyo_strcontains";
        std::vector<LValue*> args;
        for (const Value& v : op.operands) args.push_back(value(v));
        vals[k0] = b.CreateCall(
            callee(sym, std::vector<llvm::Type*>(args.size(), i64()), i64()),
            args);
        return;
    }
    if (n == "xyo.getvar") {
        LValue* var = slotVariable(op, false);
        vals[k0] = b.CreateLoad(
            i64(), b.CreateConstGEP1_32(i8(), var,
                                        uint32_t(offsetof(Variable, value))));
        return;
    }
    if (n == "xyo.setvar") {
        LValue* var = slotVariable(op, false);
        b.CreateStore(value(op.operands[0]),
                      b.CreateConstGEP1_32(i8(), var,
                                           uint32_t(offsetof(Variable, value))));
        return;
    }
    if (n == "xyo.list.ptr") {
        vals[k0] = slotVariable(op, true);
        return;
    }
    if (n == "xyo.list.item.i") {
        // 索引が整数 SSA に昇格済み: 浮動小数の範囲/整数検査を省き、
        // i32 の 1..len 比較だけで直接読む。外れたら helper に任せる。
        LValue* var = value(op.operands[0]);
        LValue* idx = value(op.operands[1]);
        ListHeader* lh = nullptr;
        LValue* list = listPtrFor(op, var, &lh);
        BasicBlock* chk1 = BasicBlock::Create(lc, "lii.chk1", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "lii.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "lii.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "lii.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, chk1,
                       llvm::MDNode::get(lc, {
                           llvm::MDString::get(lc, "branch_weights"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 1)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 63))}));
        BasicBlock* chk2 = BasicBlock::Create(lc, "lii.chk2", fn);
        b.SetInsertPoint(chk1);
        if (!flatListStore_) {
            LValue* extra = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), list, uint32_t(offsetof(SList, extra))));
            b.CreateCondBr(b.CreateIsNull(extra), chk2, slow);
            b.SetInsertPoint(chk2);
        } else {
            b.CreateBr(chk2);
            b.SetInsertPoint(chk2);
        }
        LValue* len =
            lh && lh->lenPhi ? lh->lenPhi
            : lh ? b.CreateLoad(i32(), lh->lenSlot)
                 : b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, len))));
        // [1,len] の両側判定を符号なし 1 命令に畳む: (uint)(idx-1) < (uint)len。
        // idx<1 なら idx-1 は wrap して巨大値になり、idx>len なら idx-1>=len。
        // XYO_MLIR_NOULEITEM=1 で旧形式（SGE+SLE+AND）に戻す（A/B 用）。
        static const bool noUleItem = [] {
            const char* e = std::getenv("XYO_MLIR_NOULEITEM");
            return e && e[0] == '1';
        }();
        LValue* ok = noUleItem
            ? b.CreateAnd(
                  b.CreateICmpSGE(idx, llvm::ConstantInt::get(i32(), 1)),
                  b.CreateICmpSLE(idx, len))
            : b.CreateICmpULT(
                  b.CreateSub(idx, llvm::ConstantInt::get(i32(), 1)), len);
        BasicBlock* oob = BasicBlock::Create(lc, "lii.oob", fn);
        b.CreateCondBr(ok, fast, oob, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items =
            lh && lh->itemsPhi ? lh->itemsPhi
            : lh ? b.CreateLoad(ptr(), lh->itemsSlot)
                 : b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, items))));
        LValue* off = b.CreateSub(b.CreateSExt(idx, i64()),
                                  llvm::ConstantInt::get(i64(), 1));
        LValue* item = b.CreateLoad(i64(), b.CreateGEP(i64(), items, off));
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(oob);
        BasicBlock* oobEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* idxBits = b.CreateBitCast(b.CreateSIToFP(idx, f64()), i64());
        LValue* slowVal = b.CreateCall(
            callee("xyo_list_item", {ptr(), ptr(), i64()}, i64()),
            {ctx, var, idxBits});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(i64(), 3);
        phi->addIncoming(item, fastEnd);
        phi->addIncoming(llvm::ConstantInt::get(i64(), val::kEmptyStr),
                         oobEnd);
        phi->addIncoming(slowVal, slowEnd);
        vals[k0] = phi;
        return;
    }
    if (n == "xyo.list.item.num.i") {
        // 数値リストの読みを f64 として直接返す（タグ検査なし）。
        LValue* var = value(op.operands[0]);
        LValue* idx = value(op.operands[1]);
        ListHeader* lh = nullptr;
        LValue* list = listPtrFor(op, var, &lh);
        BasicBlock* chk1 = BasicBlock::Create(lc, "lin.chk1", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "lin.fast", fn);
        BasicBlock* oob = BasicBlock::Create(lc, "lin.oob", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "lin.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "lin.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), oob, chk1,
                       llvm::MDNode::get(lc, {
                           llvm::MDString::get(lc, "branch_weights"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 1)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 63))}));
        BasicBlock* chk2 = BasicBlock::Create(lc, "lin.chk2", fn);
        b.SetInsertPoint(chk1);
        b.CreateBr(chk2);
        b.SetInsertPoint(chk2);
        LValue* len =
            lh && lh->lenPhi ? lh->lenPhi
            : lh ? b.CreateLoad(i32(), lh->lenSlot)
                 : b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, len))));
        // [1,len] の両側判定を符号なし 1 命令に畳む: (uint)(idx-1) < (uint)len。
        // idx<1 なら idx-1 は wrap して巨大値になり、idx>len なら idx-1>=len。
        // XYO_MLIR_NOULEITEM=1 で旧形式（SGE+SLE+AND）に戻す（A/B 用）。
        static const bool noUleItem = [] {
            const char* e = std::getenv("XYO_MLIR_NOULEITEM");
            return e && e[0] == '1';
        }();
        LValue* ok = noUleItem
            ? b.CreateAnd(
                  b.CreateICmpSGE(idx, llvm::ConstantInt::get(i32(), 1)),
                  b.CreateICmpSLE(idx, len))
            : b.CreateICmpULT(
                  b.CreateSub(idx, llvm::ConstantInt::get(i32(), 1)), len);
        // 範囲外は helper を呼ばず 0.0（to_num("")=0 と同値）。
        b.CreateCondBr(ok, fast, oob, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items =
            lh && lh->itemsPhi ? lh->itemsPhi
            : lh ? b.CreateLoad(ptr(), lh->itemsSlot)
                 : b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, items))));
        LValue* off = b.CreateSub(b.CreateSExt(idx, i64()),
                                  llvm::ConstantInt::get(i64(), 1));
        LValue* item = b.CreateLoad(i64(), b.CreateGEP(i64(), items, off));
        // numeric/intvalued フラグは「プロジェクト内の書き込みが全て数値」
        // という静的証明に過ぎず、ホスト（テストの SList::push 等）が実行
        // の合間に文字列を挿入し得る。タグを見ずに bitcast するとポインタ
        // を浮動小数と誤読するので、plain double は fast、タグ付きは
        // numericStringFast（SSr 数値キャッシュ）を試みてから slow へ。
        BasicBlock* fastNum = BasicBlock::Create(lc, "lin.fastnum", fn);
        BasicBlock* tagBB = BasicBlock::Create(lc, "lin.tag", fn);
        b.CreateCondBr(b.CreateICmpSLT(
                           item, llvm::ConstantInt::get(i64(), kNumBitsLimit)),
                       fastNum, tagBB);
        b.SetInsertPoint(fastNum);
        LValue* fv = b.CreateBitCast(item, f64());
        // 整数値リスト（intvalued=1）は NaN を含まないことが静的に証明
        // されているので NaN→0 正規化を省く。XYO_MLIR_NOINTVAL=1 で無効化。
        static const bool noIntVal = [] {
            const char* e = std::getenv("XYO_MLIR_NOINTVAL");
            return e && e[0] == '1';
        }();
        bool skipNan = false;
        if (!noIntVal && !op.operands.empty() && op.operands[0].op &&
            op.operands[0].op->name == "xyo.list.ptr") {
            skipNan = (op.operands[0].op->attrInt("intvalued", 0) != 0);
        }
        LValue* fd = skipNan ? fv
                             : b.CreateSelect(b.CreateFCmpUNO(fv, fv),
                                              llvm::ConstantFP::get(f64(), 0.0),
                                              fv);
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(tagBB);
        BasicBlock* strEnd = nullptr;
        LValue* strVal =
            numericStringFast(item, slow, join, &strEnd);
        b.SetInsertPoint(oob);
        BasicBlock* oobEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* idxBits = b.CreateBitCast(b.CreateSIToFP(idx, f64()), i64());
        LValue* raw = b.CreateCall(
            callee("xyo_list_item", {ptr(), ptr(), i64()}, i64()),
            {ctx, var, idxBits});
        LValue* sd = b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {raw});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(f64(), 4);
        phi->addIncoming(fd, fastEnd);
        phi->addIncoming(strVal, strEnd);
        phi->addIncoming(llvm::ConstantFP::get(f64(), 0.0), oobEnd);
        phi->addIncoming(sd, slowEnd);
        vals[k0] = phi;
        return;
    }
    if (n == "xyo.list.item.num") {
        // 数値リストの読み（Value 索引）。範囲/整数検査は item と同一で、
        // 値はタグ検査なしで数値化する。
        LValue* var = value(op.operands[0]);
        // box.f 昇格値なら bits 化をスキップし slow でだけ materialize する。
        LValue* f = indexF(op.operands[1]);
        ListHeader* lh = nullptr;
        LValue* list = listPtrFor(op, var, &lh);
        BasicBlock* chk1 = BasicBlock::Create(lc, "lnv.chk1", fn);
        BasicBlock* chk2 = BasicBlock::Create(lc, "lnv.chk2", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "lnv.fast", fn);
        // ホットな並び (fast → fastNum → join) を連続させ、slow は最後に
        // 置く。slow を fast と fastNum の間に挟むと fast 経路が無条件 jmp
        // で飛び越す形になり、ループ本体が 2 キャッシュラインに跨がって
        // 実測 2.3 倍遅かった（定数索引のリスト読み）。
        BasicBlock* fastNum = BasicBlock::Create(lc, "lnv.fastnum", fn);
        BasicBlock* join = BasicBlock::Create(lc, "lnv.join", fn);
        BasicBlock* oob = BasicBlock::Create(lc, "lnv.oob", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "lnv.slow", fn);
        b.CreateCondBr(b.CreateIsNull(list), oob, chk1,
                       llvm::MDNode::get(lc, {
                           llvm::MDString::get(lc, "branch_weights"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 1)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(chk1);
        b.CreateBr(chk2);
        b.SetInsertPoint(chk2);
        LValue* len =
            lh && lh->lenPhi ? lh->lenPhi
            : lh ? b.CreateLoad(i32(), lh->lenSlot)
                 : b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, len))));
        // 整数化範囲検査（legacy と同形）: off = cvttsd2si(f)-1 を
        // (uint64)off < len で判定。toListIndexD は floor + [1,len] だが、
        // 正の有限値で floor==trunc、負/範囲外/NaN/タグ値は cvtt の結果が
        // unsigned で len を超えて必ず oob へ落ちるため等価。
        // f64 の 3 比較 + floor + AND 列が cvtt+sub+ult の 3 命令になる。
        LValue* i0 = b.CreateSub(b.CreateFPToSI(f, i64()),
                                 llvm::ConstantInt::get(i64(), 1));
        LValue* ok = b.CreateICmpULT(i0, b.CreateZExt(len, i64()));
        // 範囲外は helper 経由。0.0 を直接返すとタグ付き索引（文字列
        // "3"、bool 等）を 0 と誤読する — bitcast→cvttsd2si が非数値
        // ビットを巨大値/0 にするため fast path の範囲判定を通らず、
        // slow 側の xyo_list_item が正しく変換する分岐に届かない。
        b.CreateCondBr(ok, fast, oob, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items =
            lh && lh->itemsPhi ? lh->itemsPhi
            : lh ? b.CreateLoad(ptr(), lh->itemsSlot)
                 : b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, items))));
        LValue* item = b.CreateLoad(i64(), b.CreateGEP(i64(), items, i0));
        // 索引が数値確定（box.f/from_int の昇格値、または数値 const）なら
        // OOB は常に to_num("") = 0.0 — helper を呼ばず inline で返す。
        // タグ付き索引（文字列 "3" 等）は slow の xyo_list_item が正しく
        // 変換するため従来通り helper へ落とす。
        bool numIdx = false;
        if (op.operands.size() >= 2 && op.operands[1].op) {
            const std::string& on = op.operands[1].op->name;
            if (on == "xyo.box.f" || on == "xyo.from_int")
                numIdx = true;
            else if (on == "xyo.const") {
                numIdx = static_cast<uint64_t>(
                             op.operands[1].op->attrInt("bits")) <
                         static_cast<uint64_t>(val::kNan);
            }
        }
        // item.num.i と同じ理由（ホストがプロジェクト外から非数値を挿入
        // し得る）で、要素のタグを確認してから bitcast する。
        // タグ付き要素は numericStringFast（SStr 数値キャッシュ）で
        // inline 化を試み、外れたら slow へ。
        BasicBlock* tagBB = BasicBlock::Create(lc, "lnv.tag", fn);
        b.CreateCondBr(
            b.CreateICmpSLT(item,
                            llvm::ConstantInt::get(i64(), kNumBitsLimit)),
            fastNum, tagBB,
            llvm::MDNode::get(
                lc, {llvm::MDString::get(lc, "branch_weights"),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(i64(), 63)),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fastNum);
        LValue* fv = b.CreateBitCast(item, f64());
        // 整数値リスト（intvalued=1）は NaN を含まないことが静的に証明
        // されているので NaN→0 正規化を省く（item.num.i と同様）。
        static const bool noIntVal2 = [] {
            const char* e = std::getenv("XYO_MLIR_NOINTVAL");
            return e && e[0] == '1';
        }();
        bool skipNan2 = false;
        if (!noIntVal2 && !op.operands.empty() && op.operands[0].op &&
            op.operands[0].op->name == "xyo.list.ptr") {
            skipNan2 = (op.operands[0].op->attrInt("intvalued", 0) != 0);
        }
        LValue* fd = skipNan2
                         ? fv
                         : b.CreateSelect(b.CreateFCmpUNO(fv, fv),
                                          llvm::ConstantFP::get(f64(), 0.0),
                                          fv);
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(tagBB);
        BasicBlock* strEnd = nullptr;
        LValue* strVal =
            numericStringFast(item, slow, join, &strEnd);
        b.SetInsertPoint(oob);
        BasicBlock* oobEnd = nullptr;
        if (numIdx) {
            // 数値索引の OOB = to_num("") = 0.0。helper 不要。
            b.CreateBr(join);
            oobEnd = oob;
        } else {
            b.CreateBr(slow);
        }
        b.SetInsertPoint(slow);
        LValue* raw = b.CreateCall(
            callee("xyo_list_item", {ptr(), ptr(), i64()}, i64()),
            {ctx, var, slowBits(op.operands[1])});
        LValue* sd = b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {raw});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(f64(), numIdx ? 4 : 3);
        phi->addIncoming(fd, fastEnd);
        phi->addIncoming(strVal, strEnd);
        phi->addIncoming(sd, slowEnd);
        if (numIdx)
            phi->addIncoming(llvm::ConstantFP::get(f64(), 0.0), oobEnd);
        vals[k0] = phi;
        return;
    }
    if (n == "xyo.list.replace.i") {
        // 索引が整数 SSA: i32 の範囲比較だけで items[idx-1] を直接書く。
        LValue* var = value(op.operands[0]);
        LValue* idx = value(op.operands[1]);
        LValue* item = value(op.operands[2]);
        ListHeader* lh = nullptr;
        LValue* list = listPtrFor(op, var, &lh);
        BasicBlock* chk1 = BasicBlock::Create(lc, "lri.chk1", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "lri.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "lri.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "lri.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, chk1,
                       llvm::MDNode::get(lc, {
                           llvm::MDString::get(lc, "branch_weights"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 1)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 63))}));
        BasicBlock* chk2 = BasicBlock::Create(lc, "lri.chk2", fn);
        b.SetInsertPoint(chk1);
        if (!flatListStore_) {
            LValue* extra = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), list, uint32_t(offsetof(SList, extra))));
            b.CreateCondBr(b.CreateIsNull(extra), chk2, slow,
                           llvm::MDNode::get(lc, {
                               llvm::MDString::get(lc, "branch_weights"),
                               llvm::ConstantAsMetadata::get(
                                   llvm::ConstantInt::get(i64(), 63)),
                               llvm::ConstantAsMetadata::get(
                                   llvm::ConstantInt::get(i64(), 1))}));
            b.SetInsertPoint(chk2);
        } else {
            b.CreateBr(chk2);
            b.SetInsertPoint(chk2);
        }
        LValue* len =
            lh && lh->lenPhi ? lh->lenPhi
            : lh ? b.CreateLoad(i32(), lh->lenSlot)
                 : b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, len))));
        // [1,len] の両側判定を符号なし 1 命令に畳む: (uint)(idx-1) < (uint)len。
        // idx<1 なら idx-1 は wrap して巨大値になり、idx>len なら idx-1>=len。
        // XYO_MLIR_NOULEITEM=1 で旧形式（SGE+SLE+AND）に戻す（A/B 用）。
        static const bool noUleItem = [] {
            const char* e = std::getenv("XYO_MLIR_NOULEITEM");
            return e && e[0] == '1';
        }();
        LValue* ok = noUleItem
            ? b.CreateAnd(
                  b.CreateICmpSGE(idx, llvm::ConstantInt::get(i32(), 1)),
                  b.CreateICmpSLE(idx, len))
            : b.CreateICmpULT(
                  b.CreateSub(idx, llvm::ConstantInt::get(i32(), 1)), len);
        b.CreateCondBr(ok, fast, slow, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items =
            lh && lh->itemsPhi ? lh->itemsPhi
            : lh ? b.CreateLoad(ptr(), lh->itemsSlot)
                 : b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                          i8(), list,
                                          uint32_t(offsetof(SList, items))));
        LValue* off = b.CreateSub(b.CreateSExt(idx, i64()),
                                  llvm::ConstantInt::get(i64(), 1));
        b.CreateStore(item, b.CreateGEP(i64(), items, off));
        // SList::monitorDirty は読み手が存在しないため fast path では
        // 書かない（他の replace/add fast path と同じ）。
        // 書き込み値が数値と証明できるなら numericOnly は変化しない
        //（読み+比較+AND+store の 4 命令を省く）。
        if (!guaranteedNumericBits(op.operands[op.operands.size() - 1])) {
            LValue* oldN = b.CreateLoad(
                i8(), b.CreateConstGEP1_32(
                          i8(), list,
                          uint32_t(offsetof(SList, numericOnly))));
            LValue* isNum = b.CreateZExt(
                b.CreateICmpSLT(item,
                                llvm::ConstantInt::get(i64(), kNumBitsLimit)),
                i8());
            b.CreateStore(b.CreateAnd(oldN, isNum),
                          b.CreateConstGEP1_32(
                              i8(), list,
                              uint32_t(offsetof(SList, numericOnly))));
        }
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* idxBits = b.CreateBitCast(b.CreateSIToFP(idx, f64()), i64());
        b.CreateCall(callee("xyo_list_replace", {ptr(), ptr(), i64(), i64()},
                            b.getVoidTy()),
                     {ctx, var, idxBits, item});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        return;
    }
    if (n == "xyo.list.add") {
        // legacy と同じ: list != null かつ extra == nullptr かつ len < cap で
        // 長さ上限に達していなければ items[len] に直接書く。
        LValue* var = value(op.operands[0]);
        LValue* item = value(op.operands[1]);
        LValue* list = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), var, uint32_t(offsetof(Variable, list))));
        BasicBlock* have = BasicBlock::Create(lc, "la.have", fn);
        BasicBlock* chk = BasicBlock::Create(lc, "la.chk", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "la.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "la.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "la.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, have, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(have);
        if (!flatListStore_) {
            LValue* extra = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), list, uint32_t(offsetof(SList, extra))));
            b.CreateCondBr(b.CreateIsNull(extra), chk, slow);
            b.SetInsertPoint(chk);
        } else {
            b.CreateBr(chk);
            b.SetInsertPoint(chk);
        }
        LValue* len = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, len))));
        LValue* cap = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, cap))));
        // 長さ上限（0 = 無制限）に達していたら helper（上限で落とす）。
        if (!limitGlobal_) {
            // 関数ごとに Lowerer を作るので、モジュールに既に宣言済みなら
            // それを使う（重複宣言は LLVM が `.1` を付けて改名し、JIT の
            // シンボル解決に失敗してインタプリタへ落ちる）。
            limitGlobal_ = mod.getGlobalVariable("xyo_list_limit");
            if (!limitGlobal_)
                limitGlobal_ = new llvm::GlobalVariable(
                    mod, i32(), false, llvm::GlobalValue::ExternalLinkage,
                    nullptr, "xyo_list_limit");
        }
        LValue* lim = b.CreateLoad(i32(), limitGlobal_);
        LValue* room = b.CreateOr(
            b.CreateICmpEQ(lim, llvm::ConstantInt::get(i32(), 0)),
            b.CreateICmpULT(len, lim));
        LValue* ok = b.CreateAnd(
            b.CreateICmpULT(len, cap), room);
        b.CreateCondBr(ok, fast, slow, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, items))));
        b.CreateStore(item, b.CreateGEP(i64(), items, b.CreateZExt(len, i64())));
        b.CreateStore(b.CreateAdd(len, llvm::ConstantInt::get(i32(), 1)),
                      b.CreateConstGEP1_32(
                          i8(), list, uint32_t(offsetof(SList, len))));
        // SList::monitorDirty は読み手が存在しないため fast path では書かない
        //（helper 経路が設定する）。numericOnly は単調なので維持する。
        // 書き込み値が数値と証明できるなら numericOnly は変化しない
        //（読み+比較+AND+store の 4 命令を省く）。
        if (!guaranteedNumericBits(op.operands[op.operands.size() - 1])) {
            LValue* oldN = b.CreateLoad(
                i8(), b.CreateConstGEP1_32(
                          i8(), list,
                          uint32_t(offsetof(SList, numericOnly))));
            LValue* isNum = b.CreateZExt(
                b.CreateICmpSLT(item,
                                llvm::ConstantInt::get(i64(), kNumBitsLimit)),
                i8());
            b.CreateStore(b.CreateAnd(oldN, isNum),
                          b.CreateConstGEP1_32(
                              i8(), list,
                              uint32_t(offsetof(SList, numericOnly))));
        }
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        b.CreateCall(callee("xyo_list_add", {ptr(), ptr(), i64()},
                            b.getVoidTy()),
                     {ctx, var, item});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        return;
    }
    if (n == "xyo.list.replace") {
        // legacy と同じ: idx が 1..len の整数なら items[idx-1] を直接書く。
        LValue* var = value(op.operands[0]);
        // box.f 昇格値なら bits 化をスキップ（slow でだけ materialize）。
        LValue* f = indexF(op.operands[1]);
        LValue* item = value(op.operands[2]);
        LValue* list = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), var, uint32_t(offsetof(Variable, list))));
        BasicBlock* have = BasicBlock::Create(lc, "lr.have", fn);
        BasicBlock* chk = BasicBlock::Create(lc, "lr.chk", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "lr.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "lr.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "lr.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, have, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(have);
        if (!flatListStore_) {
            LValue* extra = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), list, uint32_t(offsetof(SList, extra))));
            b.CreateCondBr(b.CreateIsNull(extra), chk, slow);
            b.SetInsertPoint(chk);
        } else {
            b.CreateBr(chk);
            b.SetInsertPoint(chk);
        }
        LValue* len = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, len))));
        // 索引の判定: item と同じく off = cvttsd2si(f)-1 < len の整数化。
        LValue* i0 = b.CreateSub(b.CreateFPToSI(f, i64()),
                                 llvm::ConstantInt::get(i64(), 1));
        LValue* ok = b.CreateICmpULT(i0, b.CreateZExt(len, i64()));
        b.CreateCondBr(ok, fast, slow, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, items))));
        b.CreateStore(item, b.CreateGEP(i64(), items, i0));
        // SList::monitorDirty は読み手が存在しないため fast path では書かない。
        // 書き込み値が数値と証明できるなら numericOnly は変化しない
        //（読み+比較+AND+store の 4 命令を省く）。
        if (!guaranteedNumericBits(op.operands[op.operands.size() - 1])) {
            LValue* oldN = b.CreateLoad(
                i8(), b.CreateConstGEP1_32(
                          i8(), list,
                          uint32_t(offsetof(SList, numericOnly))));
            LValue* isNum = b.CreateZExt(
                b.CreateICmpSLT(item,
                                llvm::ConstantInt::get(i64(), kNumBitsLimit)),
                i8());
            b.CreateStore(b.CreateAnd(oldN, isNum),
                          b.CreateConstGEP1_32(
                              i8(), list,
                              uint32_t(offsetof(SList, numericOnly))));
        }
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        b.CreateCall(callee("xyo_list_replace", {ptr(), ptr(), i64(), i64()},
                            b.getVoidTy()),
                     {ctx, var, slowBits(op.operands[1]), item});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        return;
    }
    if (n == "xyo.list.delete" ||
        n == "xyo.list.deleteall" || n == "xyo.list.insert" ||
        n == "xyo.list.replace") {
        const char* sym = n == "xyo.list.add" ? "xyo_list_add"
                        : n == "xyo.list.delete" ? "xyo_list_delete"
                        : n == "xyo.list.deleteall" ? "xyo_list_deleteall"
                        : n == "xyo.list.insert" ? "xyo_list_insert"
                                                 : "xyo_list_replace";
        std::vector<LValue*> args{ctx, value(op.operands[0])};
        std::vector<llvm::Type*> sig{ptr(), ptr()};
        for (size_t i = 1; i < op.operands.size(); ++i) {
            args.push_back(value(op.operands[i]));
            sig.push_back(i64());
        }
        b.CreateCall(callee(sym, sig, b.getVoidTy()), args);
        return;
    }
    if (n == "xyo.arg") {
        // reporter 手続きは第 1 引数が戻り値スロット。
        const uint32_t idx = static_cast<uint32_t>(op.attrInt("index"));
        vals[k0] = fn->getArg(argBase + idx);
        return;
    }
    if (n == "xyo.call") {
        llvm::Function* target =
            mod.getFunction(std::string(op.attrStr("callee")));
        if (!target) {
            error = "call to unknown function " +
                    std::string(op.attrStr("callee"));
            return;
        }
        const bool reporterCallee = op.attrInt("ret") != 0;
        const bool wantsValue = !op.resultTypes.empty();
        // 呼出しグラフが循環する callee（builder が rec=1 を付けた）:
        // legacy の rewriteRecursivePush と同じ形
        //   r = xyo_proc_push_ex(ctx, bid, bc, defwarp, code, depth, 1, 1)
        //   if (r && xyo_recursion_yield_due(ctx)) xyo_yield(ctx)
        //   call; xyo_proc_pop(ctx)
        // で非 warp の再帰呼出しが譲る（interpreter との parity）。
        // 明示 yield は stackless::lowerRecursive がそのまま
        // コルーチンの停止点として使える。
        const bool framed = op.attrInt("rec") != 0;
        if (framed) {
            mayYield = true;
            if (!mfWarp_) voluntaryYield = true;
            const void* bc = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(op.attrInt("bc")));
            LValue* rec = b.CreateCall(
                callee("xyo_proc_push_ex",
                       {ptr(), ptr(), ptr(), i32(), ptr(), i32(), i32(),
                        i32()},
                       i32()),
                {ctx, idPtr(std::string(op.attrStr("bid"))),
                 llvm::ConstantExpr::getIntToPtr(
                     llvm::ConstantInt::get(
                         i64(), reinterpret_cast<uintptr_t>(bc)),
                     ptr()),
                 llvm::ConstantInt::get(i32(), op.attrInt("defwarp")),
                 idPtr(std::string(op.attrStr("code"))),
                 llvm::ConstantInt::get(i32(), op.attrInt("depth")),
                 llvm::ConstantInt::get(i32(), 1),
                 llvm::ConstantInt::get(i32(), 1)});
            LValue* due = b.CreateCall(
                callee("xyo_recursion_yield_due", {ptr()}, i32()), {ctx});
            LValue* need = b.CreateICmpNE(
                b.CreateAnd(rec, due), llvm::ConstantInt::get(i32(), 0));
            BasicBlock* yieldBB = BasicBlock::Create(lc, "rec.yield", fn);
            BasicBlock* callBB = BasicBlock::Create(lc, "rec.call", fn);
            b.CreateCondBr(need, yieldBB, callBB);
            b.SetInsertPoint(yieldBB);
            {
                // warp 手続き内では runtime の isWarp が常に 1 で
                // due 判定が必ず偽 — 実行上は no-op の協調点。
                // legacy の callCooperativeYield と同じく !xyo.coop
                // を付け、コルーチンの停止点にしない。
                auto* call = b.CreateCall(
                    callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
                if (mfWarp_)
                    call->setMetadata("xyo.coop",
                                      llvm::MDNode::get(lc, {}));
            }
            b.CreateBr(callBB);
            b.SetInsertPoint(callBB);
        }
        std::vector<LValue*> args;
        llvm::AllocaInst* outSlot = nullptr;
        args.push_back(ctx);
        if (reporterCallee) {
            outSlot = entryAlloca(i64(), "call.out");
            b.CreateStore(llvm::ConstantInt::get(i64(), val::kUndefined),
                          outSlot);
            args.push_back(outSlot);
        }
        for (const Value& v : op.operands) args.push_back(value(v));
        b.CreateCall(target, args);
        if (framed)
            b.CreateCall(callee("xyo_proc_pop", {ptr()}, b.getVoidTy()),
                         {ctx});
        // 手続き内の stop all / stop this script / delete this clone は
        // callee 側の xyo_*_begin が c->stopped を立てる。legacy codegen
        // の emitStoppedUnwind と同じく、呼出し後に検査してスクリプト
        // 全体を共通出口へ巻き戻す（warp 復元・yield カウンタ書き戻し
        // のため emitStopped の RetVoid ではなく retBlock へ分岐）。
        {
            LValue* st = b.CreateLoad(
                i32(), b.CreateConstGEP1_32(
                    i8(), ctx, uint32_t(offsetof(JitCtx, stopped))));
            BasicBlock* stopBB = BasicBlock::Create(lc, "call.stop", fn);
            BasicBlock* contBB = BasicBlock::Create(lc, "call.cont", fn);
            b.CreateCondBr(
                b.CreateICmpNE(st, llvm::ConstantInt::get(i32(), 0)),
                stopBB, contBB);
            b.SetInsertPoint(stopBB);
            b.CreateBr(retBlock());
            b.SetInsertPoint(contBB);
        }
        if (wantsValue)
            vals[k0] = reporterCallee
                           ? static_cast<LValue*>(b.CreateLoad(i64(), outSlot))
                           : llvm::ConstantInt::get(i64(), val::kUndefined);
        return;
    }
    if (n == "xyo.procret") {
        LValue* out = fn->getArg(1);
        LValue* v = value(op.operands[0]);
        BasicBlock* store = BasicBlock::Create(lc, "ret.store", fn);
        BasicBlock* done = BasicBlock::Create(lc, "ret.done", fn);
        b.CreateCondBr(
            b.CreateICmpNE(out, llvm::ConstantPointerNull::get(
                                    llvm::PointerType::getUnqual(lc))),
            store, done);
        b.SetInsertPoint(store);
        b.CreateStore(v, out);
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateBr(retBlock());
        // 終端済みブロックへ以後の op/region 終端 br を足さないよう
        // dead ブロックへ挿入点を移す（call.stop と同じ形）。
        b.SetInsertPoint(BasicBlock::Create(lc, "ret.dead", fn));
        return;
    }
    if (n == "xyo.snd.volume") {
        vals[k0] = b.CreateCall(callee("xyo_snd_getvolume", {ptr()}, i64()),
                                {ctx});
        return;
    }
    if (n == "xyo.snd.setvolume" || n == "xyo.snd.changevolume") {
        const char* sym = n == "xyo.snd.setvolume" ? "xyo_snd_setvolume"
                                                   : "xyo_snd_changevolume";
        b.CreateCall(callee(sym, {ptr(), i64()}, b.getVoidTy()),
                     {ctx, value(op.operands[0])});
        return;
    }
    if (n == "xyo.pen.setsize") {
        // 既存 codegen と同じ helper — 呼出し回数カウンタ
        // （xyo_pen_size_calls）の計上もここで行われる。
        b.CreateCall(callee("xyo_pen_setsize", {ptr(), i64()}, b.getVoidTy()),
                     {ctx, value(op.operands[0])});
        return;
    }
    if (n == "xyo.direct" || n == "xyo.direct.get") {
        // build 側の directMap と同じ契約: sym 名のランタイム helper に
        // (ctx, 入力値..., フィールド値...) をそのまま渡す。
        const std::string sym(op.attrStr("sym"));
        if (sym.empty()) {
            error = "xyo.direct: missing sym";
            return;
        }
        if (op.attrInt("yields") != 0) {
            mayYield = true;
            voluntaryYield = true;
        }
        // コルーチン化前提では、内部で Fiber を yield する blocking helper
        // を begin/poll 形へ展開する（stackless::lower は
        // xyo_mot_glidexy / xyo_looks_saysecs 等を「内部 yield」として拒否）。
        if (coroMode_ && op.attrInt("yields") != 0) {
            if (emitCoroBlockingDirect(sym, op)) return;
        }
        std::vector<llvm::Type*> sig{ptr()};
        std::vector<LValue*> args{ctx};
        for (const Value& v : op.operands) {
            sig.push_back(i64());
            args.push_back(value(v));
        }
        LValue* r = b.CreateCall(
            callee(sym.c_str(), sig,
                   n == "xyo.direct" ? b.getVoidTy() : i64()),
            args);
        if (n == "xyo.direct.get") vals[k0] = r;
        return;
    }
    if (n == "xyo.monitor") {
        // モニタ表示切替（legacy の xyo_var_monitor と同じ helper）。
        const std::string id(op.attrStr("id"));
        b.CreateCall(
            callee("xyo_var_monitor", {ptr(), ptr(), i32()}, b.getVoidTy()),
            {ctx, idPtr(id),
             llvm::ConstantInt::get(i32(), op.attrInt("vis"))});
        return;
    }
    if (n == "xyo.wait") {
        // legacy codegen と同じ waitBegin → poll/yield ループ。
        mayYield = true;
        voluntaryYield = true;
        b.CreateCall(callee("xyo_wait_begin", {ptr(), f64()}, b.getVoidTy()),
                     {ctx, f64Operand(op.operands[0])});
        BasicBlock* head = BasicBlock::Create(lc, "wait.head", fn);
        BasicBlock* yieldBB = BasicBlock::Create(lc, "wait.yield", fn);
        BasicBlock* doneBB = BasicBlock::Create(lc, "wait.done", fn);
        b.CreateBr(head);
        b.SetInsertPoint(head);
        LValue* more = b.CreateCall(
            callee("xyo_wait_poll", {ptr()}, i32()), {ctx});
        b.CreateCondBr(
            b.CreateICmpNE(more, llvm::ConstantInt::get(i32(), 0)),
            yieldBB, doneBB);
        b.SetInsertPoint(yieldBB);
        b.CreateCall(callee("xyo_yield", {ptr()}, b.getVoidTy()), {ctx});
        b.CreateBr(head);
        b.SetInsertPoint(doneBB);
        return;
    }
    if (n == "xyo.days2000" || n == "xyo.timer") {
        const char* sym = n == "xyo.timer" ? "xyo_timer"
                                           : "xyo_sns_dayssince2000_f64";
        LValue* d = b.CreateCall(callee(sym, {ptr()}, f64()), {ctx});
        vals[k0] = b.CreateCall(callee("xyo_num", {f64()}, i64()), {d});
        return;
    }
    if (n == "xyo.empty_repeat") {
        mayYield = true;
        b.CreateCall(callee("xyo_empty_repeat_warp", {ptr(), f64()},
                            b.getVoidTy()),
                     {ctx, b.CreateCall(callee("xyo_to_num", {i64()}, f64()),
                                        {value(op.operands[0])})});
        return;
    }
    if (n == "xyo.compat.call") { emitCompat(op, false); return; }
    if (n == "xyo.compat.get") { emitCompat(op, true); return; }
    if (n == "xyo.all_at_once") { emitAllAtOnce(op); return; }
    if (n == "xyo.for_each") { emitForEach(op); return; }
    if (n == "xyo.stop") { emitStop(op); return; }
    if (n == "xyo.wait_until") { emitWaitUntil(op); return; }
    if (n == "xyo.list.contents") {
        // legacy の data_listcontents と同じ helper（Variable* を受ける）。
        vals[k0] = b.CreateCall(
            callee("xyo_list_contents", {ptr(), ptr()}, i64()),
            {ctx, value(op.operands[0])});
        return;
    }
    if (n == "xyo.list.length") {
        LValue* var = value(op.operands[0]);
        LValue* list = b.CreateLoad(
            ptr(), b.CreateConstGEP1_32(
                       i8(), var, uint32_t(offsetof(Variable, list))));
        BasicBlock* fast = BasicBlock::Create(lc, "len.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "len.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "len.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, fast, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(fast);
        LValue* len = b.CreateLoad(
            i32(), b.CreateConstGEP1_32(
                       i8(), list, uint32_t(offsetof(SList, len))));
        // len は u32 由来で NaN に成り得ない — bitcast で十分
        // （box() の xyo_num 呼出しを inner loop から外す）。
        LValue* lenF64 = b.CreateUIToFP(len, f64());
        LValue* boxed = b.CreateBitCast(lenF64, i64());
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* slowVal = b.CreateCall(
            callee("xyo_list_length", {ptr(), ptr()}, i64()),
            {ctx, var});
        LValue* slowF64 =
            b.CreateCall(callee("xyo_to_num", {i64()}, f64()), {slowVal});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(i64(), 2);
        phi->addIncoming(boxed, fastEnd);
        phi->addIncoming(slowVal, slowEnd);
        vals[k0] = phi;
        // f64 影を join で合流させ、後続の to_num 往復を消す。
        auto* fphi = b.CreatePHI(f64(), 2);
        fphi->addIncoming(lenF64, fastEnd);
        fphi->addIncoming(slowF64, slowEnd);
        numShadow_[k0] = NumShadowEntry{fphi, join};
        return;
    }
    if (n == "xyo.list.item") {
        // 直接読み: SList が JIT 直アクセス可（extra == nullptr）で、索引が
        // 1..len の整数のときだけ items[idx-1] を読む。それ以外は helper。
        LValue* var = value(op.operands[0]);
        // box.f 昇格値なら bits 化をスキップ（slow でだけ materialize）。
        LValue* f = indexF(op.operands[1]);
        ListHeader* lh = nullptr;
        LValue* list = listPtrFor(op, var, &lh);
        BasicBlock* pre = b.GetInsertBlock();
        BasicBlock* chk1 = BasicBlock::Create(lc, "li.chk1", fn);
        BasicBlock* chk2 = BasicBlock::Create(lc, "li.chk2", fn);
        BasicBlock* fast = BasicBlock::Create(lc, "li.fast", fn);
        BasicBlock* slow = BasicBlock::Create(lc, "li.slow", fn);
        BasicBlock* join = BasicBlock::Create(lc, "li.join", fn);
        b.CreateCondBr(b.CreateIsNull(list), slow, chk1,
                       llvm::MDNode::get(lc, {
                           llvm::MDString::get(lc, "branch_weights"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 1)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(i64(), 63))}));
        b.SetInsertPoint(chk1);
        // ListHeader があるループは「extra 無しの平坦ストレージ」のとき
        // だけ作られるので、そのときは extra 検査自体を省ける。
        if (!flatListStore_ && !lh) {
            LValue* extra = b.CreateLoad(
                ptr(), b.CreateConstGEP1_32(
                           i8(), list, uint32_t(offsetof(SList, extra))));
            b.CreateCondBr(b.CreateIsNull(extra), chk2, slow);
            b.SetInsertPoint(chk2);
            chk2 = b.GetInsertBlock();
        } else {
            b.CreateBr(chk2);
            b.SetInsertPoint(chk2);
        }
        LValue* len =
            lh && lh->lenPhi ? lh->lenPhi
            : lh ? b.CreateLoad(i32(), lh->lenSlot)
                 : b.CreateLoad(i32(), b.CreateConstGEP1_32(
                                           i8(), list,
                                           uint32_t(offsetof(SList, len))));
        // 索引の判定: item.num と同じく off = cvttsd2si(f)-1 < len の
        // 整数化（等価性は同じ）。3 比較 + floor が cvtt+sub+ult になる。
        LValue* i0 = b.CreateSub(b.CreateFPToSI(f, i64()),
                                 llvm::ConstantInt::get(i64(), 1));
        LValue* ok = b.CreateICmpULT(i0, b.CreateZExt(len, i64()));
        b.CreateCondBr(ok, fast, slow, llvm::MDNode::get(lc, {
            llvm::MDString::get(lc, "branch_weights"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 63)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64(), 1))}));
        b.SetInsertPoint(fast);
        LValue* items =
            lh && lh->itemsPhi ? lh->itemsPhi
            : lh ? b.CreateLoad(ptr(), lh->itemsSlot)
                 : b.CreateLoad(ptr(), b.CreateConstGEP1_32(
                                           i8(), list,
                                           uint32_t(offsetof(SList, items))));
        LValue* item = b.CreateLoad(i64(),
                                    b.CreateGEP(i64(), items, i0));
        BasicBlock* fastEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        LValue* slowVal = b.CreateCall(
            callee("xyo_list_item", {ptr(), ptr(), i64()}, i64()),
            {ctx, var, slowBits(op.operands[1])});
        BasicBlock* slowEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        auto* phi = b.CreatePHI(i64(), 2);
        phi->addIncoming(item, fastEnd);
        phi->addIncoming(slowVal, slowEnd);
        vals[k0] = phi;
        return;
    }
    if (n == "xyo.list.itemnum" || n == "xyo.list.contains") {
        // ループ不変なら前置へ巻き上げ、yield 後だけ再評価する。
        if (tryHoistListRead(op)) return;
    }
    if (n == "xyo.list.itemnum" ||
        n == "xyo.list.contains" || n == "xyo.list.contents") {
        const char* sym = n == "xyo.list.itemnum" ? "xyo_list_itemnum"
                        : n == "xyo.list.contains" ? "xyo_list_contains"
                                                   : "xyo_list_contents";
        std::vector<LValue*> args{ctx, value(op.operands[0])};
        if (n != "xyo.list.contents") args.push_back(value(op.operands[1]));
        std::vector<llvm::Type*> sig{ptr(), ptr()};
        if (args.size() == 3) sig.push_back(i64());
        vals[k0] = b.CreateCall(callee(sym, sig, i64()), args);
        return;
    }
    if (n == "xyo.loop_yield") {
        if (op.attrInt("warp") != 0) emitWarpLoopYield();
        else emitLoopYield();
        return;
    }
    if (n == "xyo.if") { emitIf(op); return; }
    if (n == "xyo.while") { emitWhile(op); return; }
    if (n == "xyo.repeat") { emitRepeat(op); return; }
    if (n == "xyo.forever") { emitForever(op); return; }
    if (n == "xyo.yield" || n == "xyo.condition" || n == "func.return")
        return;
    error = "unsupported op: " + n;
}

llvm::Value* Lowerer::numericStringFast(LValue* bits, BasicBlock* missBB,
                                        BasicBlock* joinBB,
                                        BasicBlock** fastEndOut) {
    // legacy が data_itemoflist / emitToNumF64 で生成する形:
    //   bits>>48 == 0x7FF9 (タグ付き SStr) → numState(acquire) != 0 →
    //   numState==2 ? 0.0 : numCache。
    // 非文字列・state==0 は missBB へ。数値が確定したら fastEndOut の
    // ブロックから joinBB へ。
    LValue* tag = b.CreateLShr(bits, llvm::ConstantInt::get(i64(), 48));
    BasicBlock* strBB = BasicBlock::Create(lc, "lns.str", fn);
    BasicBlock* stateBB = BasicBlock::Create(lc, "lns.state", fn);
    b.CreateCondBr(
        b.CreateICmpEQ(tag, llvm::ConstantInt::get(i64(), 0x7FF9)), strBB,
        missBB,
        llvm::MDNode::get(
            lc, {llvm::MDString::get(lc, "branch_weights"),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(i64(), 1)),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(i64(), 63))}));
    b.SetInsertPoint(strBB);
    LValue* str = b.CreateIntToPtr(
        b.CreateAnd(bits, llvm::ConstantInt::get(i64(), val::kPtrMask)),
        ptr());
    auto* state = b.CreateLoad(
        i32(), b.CreateConstGEP1_32(i8(), str,
                                    uint32_t(offsetof(SStr, numState))));
    state->setAtomic(llvm::AtomicOrdering::Acquire);
    b.CreateCondBr(
        b.CreateICmpEQ(state, llvm::ConstantInt::get(i32(), 0)), missBB,
        stateBB,
        llvm::MDNode::get(
            lc, {llvm::MDString::get(lc, "branch_weights"),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(i64(), 1)),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(i64(), 63))}));
    b.SetInsertPoint(stateBB);
    auto* cache = b.CreateLoad(
        f64(), b.CreateConstGEP1_32(i8(), str,
                                    uint32_t(offsetof(SStr, numCache))));
    cache->setAtomic(llvm::AtomicOrdering::Monotonic);
    // state==2 は NaN（"abc" 等）→ 0.0。それ以外はキャッシュ値
    // （state==3 は比較で非数値扱いだが to_num としては 0、
    // state==4 は Infinity — numCache が正しい値を持つ）。
    LValue* d = b.CreateSelect(
        b.CreateICmpEQ(state, llvm::ConstantInt::get(i32(), 2)),
        llvm::ConstantFP::get(f64(), 0.0), cache);
    *fastEndOut = b.GetInsertBlock();
    b.CreateBr(joinBB);
    return d;
}

void Lowerer::lowerBlock(Block& blk, BasicBlock* bb) {
    b.SetInsertPoint(bb);
    for (Op* op : blk.ops) {
        emitOp(*op);
        if (!error.empty()) {
            if (error.find(" (at ") == std::string::npos)
                error += " (at " + op->name + ")";
            return;
        }
    }
}

} // namespace

LoweredFunction lowerToLLVM(Module& source, llvm::Module& dest,
                            llvm::LLVMContext& lc,
                            const std::string& entryName, bool coroMode) {
    LoweredFunction out;
    Shared shared;
    shared.numericVars = &source.numericVars;
    llvm::Function* entryFn = nullptr;
    bool anyYield = false;
    bool anyVoluntary = false;
    // 手続きは相互再帰し得るので、先に全関数の宣言を作ってから本体を
    // lower する（call が前方の関数を参照しても解決できる）。
    std::vector<std::pair<Module::Function*, llvm::Function*>> order;
    for (auto& f : source.functions()) {
        std::vector<llvm::Type*> argTypes;
        for (Type t : f.argTypes) {
            if (t == Type::ctx() || t == Type::ptr())
                argTypes.push_back(llvm::PointerType::getUnqual(lc));
            else if (t == Type::f64())
                argTypes.push_back(llvm::Type::getDoubleTy(lc));
            else
                argTypes.push_back(llvm::Type::getInt64Ty(lc));
        }
        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(lc),
                                           argTypes, false);
        llvm::Function* fn =
            llvm::Function::Create(ft, llvm::GlobalValue::ExternalLinkage,
                                   f.name, dest);
        if (!f.body) continue;
        if (f.name == entryName) entryFn = fn;
        order.push_back({&f, fn});
    }

    for (auto& [mf, fn] : order) {
        BasicBlock* entry = BasicBlock::Create(lc, "entry", fn);
        Lowerer l{dest, lc, shared};
        l.fn = fn;
        // コルーチン化前提はスクリプトの全関数へ適用する。手続き関数は
        // 融合で entry へ畳まれるか、再帰グラフなら
        // stackless::lowerRecursive が各手続きをコルーチン化する
        // （xyo_loop_yield を残すと internal yield として拒否されるため、
        // 手続き側も同じ形にする。lower 失敗時も明示 yield の形は
        // Fiber 上で意味論が同じ）。
        l.coroMode_ = coroMode;
        l.mfWarp_ = mf->warp();
        l.ctx = fn->getArg(0);
        if (mf->op && mf->op->attrInt("ret") != 0) l.argBase = 2;
        if (mf->body->args.empty()) {
            out.error = "entry block has no JitCtx argument";
            return out;
        }
        // entry block の第 0 引数は ctx。
        l.vals[keyOf(mf->body->args[0])] = fn->getArg(0);
        l.b.SetInsertPoint(entry);
        BasicBlock* bodyStart = entry;
        // proc 引数の to_num は呼出し中不変 — index ごとに 1 回だけ
        // 求めて使い回す（legacy の引数数値影。各使用点で fast/slow
        // チェーンを繰り返すと sha256 の p9 が 2.7 倍遅かった）。
        {
            std::unordered_map<uint32_t, llvm::AllocaInst*> byIndex;
            std::function<void(Block&)> scan = [&](Block& blk) {
                for (Op* o : blk.ops) {
                    if (o->name == "xyo.arg") {
                        const uint32_t idx = static_cast<uint32_t>(
                            o->attrInt("index"));
                        llvm::AllocaInst*& slot = byIndex[idx];
                        if (!slot) {
                            slot = l.entryAlloca(l.f64(), "anc.d");
                            l.b.CreateStore(
                                l.emitToNumInline(
                                    fn->getArg(l.argBase + idx)),
                                slot);
                        }
                        l.argNumCache_.emplace(o, slot);
                    }
                    for (auto& r : o->regions)
                        if (r)
                            for (auto& rcb : r->blocks) scan(*rcb);
                }
            };
            scan(*mf->body);
            // emitToNumInline は挿入点を直列の condbr→join へ伸ばす。
            // 最後の join（未終端）を本体の開始ブロックとして渡す。
            bodyStart = l.b.GetInsertBlock();
        }
        // warp 手続きは呼出し中 isWarp=1 を保証する。呼出し元の値を
        // 保存し、return で戻す（既存 codegen と同じ契約）。
        llvm::Value* savedWarp = nullptr;
        if (mf->warp()) {
            savedWarp = l.b.CreateLoad(
                l.i32(), l.ctxField(uint32_t(offsetof(JitCtx, isWarp))));
            l.b.CreateStore(llvm::ConstantInt::get(l.i32(), 1),
                            l.ctxField(uint32_t(offsetof(JitCtx, isWarp))));
        }
        l.lowerBlock(*mf->body, bodyStart);
        if (!l.error.empty()) {
            // 途中まで lowering した module は不完全（本体の無い手続き関数が
            // 残る）ので、必ず失敗として扱う。
            out.error = std::move(l.error);
            out.ok = false;
            out.mayYield = anyYield;
            out.voluntaryYield = anyVoluntary;
            return out;
        }
        // warp ループの yield カウンタ（レジスタ）を ctx へ書き戻す。
        {
            // 注意: BasicBlock::getTerminator() ではなく back() の
            // isTerminator() を見る（この LLVM ビルドでは getTerminator が
            // 非終端命令を返す経路があり、br の挿入が落ちる）。
            llvm::BasicBlock* curBLK = l.b.GetInsertBlock();
            if (curBLK->empty() || !curBLK->back().isTerminator())
                l.b.CreateBr(l.retBlock());
        }
        l.b.SetInsertPoint(l.retBlock());
        if (savedWarp)
            l.b.CreateStore(savedWarp,
                            l.ctxField(uint32_t(offsetof(JitCtx, isWarp))));
        if (l.yieldCnt_)
            l.b.CreateStore(
                l.b.CreateLoad(l.i64(), l.yieldCnt_),
                l.ctxField(uint32_t(offsetof(JitCtx, yieldCheckN))));
        l.b.CreateRetVoid();
        anyYield |= l.mayYield;
        anyVoluntary |= l.voluntaryYield;
        if (std::getenv("XYO_MLIR_FNTRACE"))
            std::fprintf(stderr, "[mlir-fn] %s warp=%d mayYield=%d err=%s\n",
                         mf->name.c_str(), mf->warp() ? 1 : 0,
                         l.mayYield ? 1 : 0, l.error.c_str());
        if (mf->name == entryName) {
            out.ok = true;
            out.fn = fn;
        }
    }
    // スロット初期化は entry 関数の先頭で 1 回だけ（手続き関数は
    // そのスロットを共有して直接 load する）。
    if (entryFn) {
        Lowerer il{dest, lc, shared};
        il.fn = entryFn;
        il.ctx = entryFn->getArg(0);
        il.emitSlotInits();
    }
    out.mayYield = anyYield;
    out.voluntaryYield = anyVoluntary;
    out.mainThread = shared.mainThread;
    if (std::getenv("XYO_MLIR_FNTRACE"))
        std::fprintf(stderr, "[mlir-total] anyYield=%d ok=%d\n",
                     anyYield ? 1 : 0, out.ok ? 1 : 0);
    if (!out.ok && out.error.empty())
        out.error = "entry function not found: " + entryName;
    return out;
}

} // namespace xyo::mlir

#endif // XYO_JIT && !__EMSCRIPTEN__
