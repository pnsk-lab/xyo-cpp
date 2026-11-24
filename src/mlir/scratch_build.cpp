// xyo-cpp: Scratch ブロック木 → MLIR 風 IR のビルダー実装。
//
// この TU には xyo::Value（Scratch の値）と xyo::mlir::Value（IR の SSA
// 値）が同居するため、Scratch 側は常に xyo:: を明示する。例外はビルド
// 全体で無効（-fno-exceptions）なので、失敗は error_ に記録して伝播する。
//
// 手続き（procedures_definition / procedures_call）は既存 codegen と同じ
// ABI で別関数へ落とす: `void fn(JitCtx*, i64 arg0, ...)`。callee の
// エントリで `ctx->isWarp` を保存して warp 定義なら 1 を立て、return で
// 戻す。これにより warp 手続き本体のループは isWarp==1 が静的に確定する。
#include "scratch_build.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_set>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include "../jit/matpow.hpp"
#include "../jit/parfor.hpp"
#include "../model/blocks.hpp"

namespace xyo::mlir {
namespace {

bool isNumericShadow(const std::string& opcode) {
    return opcode == "math_number" || opcode == "math_integer" ||
           opcode == "math_whole_number" ||
           opcode == "math_positive_number" || opcode == "math_angle";
}

/// ハット。トップ block 自体は実行しない（入力も評価しない）。
bool isStartHat(const std::string& opcode) {
    return opcode == "event_whenflagclicked" ||
           opcode == "event_whenkeypressed" ||
           opcode == "event_whenbroadcastreceived" ||
           opcode == "event_whenthisspriteclicked" ||
           opcode == "event_whenstageclicked" ||
           opcode == "event_whenbackdropswitchesto" ||
           opcode == "event_whengreaterthan" ||
           opcode == "event_whentouchingobject" ||
           opcode == "control_start_as_clone";
}

bool isArgumentReporter(const std::string& opcode) {
    return opcode == "argument_reporter_string_number" ||
           opcode == "argument_reporter_boolean" || opcode == "argument_reporter";
}

std::string fieldName(const xyo::BlockField& f) {
    if (!f.value.isString()) return {};
    return SStr::toUtf8(f.value.asStrPtr());
}

class ScriptBuilder {
public:
    ScriptBuilder(Module& m, Blocks& blocks, const StringPinner& pin,
                  const std::string& entryName, bool allowCompat)
        : m_(m), blocks_(blocks), pin_(pin), entryName_(entryName),
          allowCompat_(allowCompat) {}

    bool build(const std::string& topId);
    bool hasLoop() const { return hasLoop_; }
    /// スタックレス・コルーチン化を前提とした IR を作る。
    void setCoroMode(bool on) { coroMode_ = on; }
    /// 内部で yield する helper（compat・glide 等）を使ってしまったか。
    bool usedInternalYield() const { return internalYield_; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    const std::map<std::string, std::string>& procNames() const {
        return procNames_;
    }

private:
    struct ProcSig {
        std::string vkey;
        std::string fnName;
        Module::Function* fn = nullptr;
        const xyo::Block* def = nullptr;
        bool warp = false;
        bool reporter = false;
        bool emitted = false;
        std::vector<std::u16string> names;
        std::vector<std::string> ids;
        std::vector<xyo::Value> defaults;
    };

    Module& m_;
    Blocks& blocks_;
    const StringPinner& pin_;
    std::string entryName_;
    std::unordered_set<std::string> seen_;
    std::map<std::string, ProcSig> procs_;
    std::deque<std::string> pending_;
    std::map<std::string, std::string> procNames_;
    std::map<std::u16string, size_t> argIndex_;
    const ProcSig* current_ = nullptr;
    bool inWarp_ = false;
    bool allowCompat_ = false;
    /// stop / return を emit したら以降の鎖は到達不能（Scratch と同じ）。
    bool terminated_ = false;
    /// デバッグ用: これらの opcode は native を使わず compat へ回す。
    static bool forcedCompat(const std::string& op) {
        static const std::unordered_set<std::string> set = [] {
            std::unordered_set<std::string> out;
            const char* e = std::getenv("XYO_MLIR_COMPAT_FORCE");
            if (!e) return out;
            std::string all(e);
            size_t pos = 0;
            while (pos <= all.size()) {
                size_t comma = all.find(',', pos);
                std::string item = all.substr(
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (!item.empty()) out.insert(item);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            return out;
        }();
        return !set.empty() && set.count(op) != 0;
    }
    bool hasLoop_ = false;
    /// スタックレス・コルーチン化前提の emission（backend の
    /// stackless::lower が受理できる形にする）。
    bool coroMode_ = false;
    /// compat 等、内部で Fiber を yield する helper を使った。
    bool internalYield_ = false;
    /// legacy codegen の nestedBranchDepth と同じ数え方: あらゆる
    /// C-block サブスタックの中にいる深さ（再帰検出の ancestor 上限に
    /// 使われる）。
    int branchDepth_ = 0;
    /// 手続きが呼出しグラフ上で循環するかのメモ化（vkey → 0/1）。
    std::unordered_map<std::string, int> cyclicCache_;
    /// par-for ワーカー本体の emit 中は空でない。カウンタ変数 id を
    /// 保持し、data_variable 読み出しを xyo.pfctr へ差し替える。
    std::string pfCounterId_;
    std::string error_;

    bool fail(const std::string& why) {
        if (error_.empty()) error_ = why;
        return false;
    }

    static const xyo::BlockInput* findInput(const xyo::Block& blk,
                                            const char* name) {
        return blk.inputs.find(name);
    }

    const xyo::BlockField* findField(const xyo::Block& blk, const char* name,
                                     const char* what) {
        auto it = blk.fields.find(name);
        if (it == blk.fields.end()) {
            fail(std::string("missing ") + what);
            return nullptr;
        }
        return &it->second;
    }

    Value undefinedValue(Builder& b) {
        return b.cst(xyo::Value::undefined().bits);
    }

    /// Scratch の Value をそのまま IR の定数にする（数値変換しない）。
    Value constFromValue(Builder& b, const xyo::Value& v) {
        if (v.isNumber())
            return b.cst(xyo::Value::numCanonical(v.asNumberUnchecked()).bits);
        if (v.isBool()) return b.cst(v.bits);
        if (v.isString()) {
            SStr* str = v.asStrPtr();
            if (!str) return b.cst(xyo::Value::emptyStr().bits);
            if (!pin_) {
                fail("string literal without a pinner");
                return {};
            }
            // 既存の SStr をそのまま pin する — 新しい SStr を作ると
            // shadow の Value とビットがずれ、「literal storage が正確な
            // boxed bits を保持する」保証が壊れる。
            return b.cst(pin_(v));
        }
        return b.cst(xyo::Value::undefined().bits);
    }

    /// 数値リテラル shadow の値。**生の Value を保持する** — 既存 codegen
    /// の `constValue`/`shadowConstant` と interpreter の `shadowValue` は
    /// NUM="1" を文字列 "1" のまま `argValues` へ入れ、`data_setvariableto`
    /// は生値を変数へ格納する。f64 化してしまうと set が型を失う
    /// （`interp=s:1 vs jit=n:1` の差分が出る）。算術側は `xyo.add` 等が
    /// toNumber を担うので正しさは変わらない。
    Value numericLiteralValue(Builder& b, const xyo::Value& v) {
        return constFromValue(b, v);
    }

    Value emitInputValue(Builder& b, const xyo::Block& parent,
                         const char* name) {
        const xyo::BlockInput* in = findInput(parent, name);
        std::string id;
        if (in) {
            if (!in->block.empty()) id = in->block;
            else if (!in->shadow.empty()) id = in->shadow;
        }
        // JS irgen の createConstantInput(0): 欠落入力は数値 0。
        if (id.empty()) return b.cst(xyo::Value::num(0).bits);
        const xyo::Block* child = blocks_.getBlock(id);
        if (!child) {
            fail("missing input block " + id);
            return {};
        }
        // shadow ブロックはフィールド値がそのまま定数になる（メニュー・
        // 色・テキスト・数値すべて。既存 codegen の shadowConstant と
        // 同じ規則: 数値変換しない）。ただし圧縮入力（[12,name,id] /
        // [13,name,id]）が作る data_variable / data_listcontents の shadow は
        // *参照* であって定数ではない — 名前を値にしてはいけない。
        const bool referenceShadow = child->shadow &&
            (child->opcode == "data_variable" ||
             child->opcode == "data_listcontents");
        if (child->shadow && !child->fields.empty() && !referenceShadow) {
            const xyo::Value& fv = child->fields.begin()->second.value;
            if (isNumericShadow(child->opcode))
                return numericLiteralValue(b, fv);
            return constFromValue(b, fv);
        }
        Value v = emitReporter(b, *child);
        if (failed()) return {};
        // Scratch の boolean も第一級値。値として使う文脈では box する。
        if (v.type == Type::boolean())
            v = b.create("xyo.boolval", {v}, {Type::value()})->result(0);
        return v;
    }

    Value listPtr(Builder& b, const xyo::BlockField& f) {
        return b.create("xyo.list.ptr", {}, {Type::ptr()},
                        {{"id", Attr::str(f.id)},
                         {"name", Attr::str(fieldName(f))}})
            ->result(0);
    }

    Value emitReporter(Builder& b, const xyo::Block& blk);
    Value emitCondition(Builder& b, const xyo::Block& blk, const char* name) {
        Value v = emitInputValue(b, blk, name);
        if (failed()) return {};
        return b.truthy(v);
    }

    bool emitChain(Builder& b, std::string id, int depth = 0);
    bool emitStatement(Builder& b, const xyo::Block& blk);
    /// repeat until / while の行列累乗変換を一時的に止める
    /// （変換の else 分岐で元のループを同じ形で再 emit するため）。
    bool suppressMatPowCond_ = false;
    bool emitSubstack(Builder& b, const xyo::Block& blk, const char* name) {
        const xyo::BlockInput* sub = findInput(blk, name);
        if (sub && !sub->block.empty()) {
            // 分岐本体内の return / stop はその本体だけを終わらせる。
            // terminated_ は「今の鎖の後ろは実行されない」を表すので、
            // 本体を抜けたら必ず外側の値へ戻す（戻さないと、if の then で
            // return した後に続く文が丸ごと消える）。
            const bool savedTerm = terminated_;
            terminated_ = false;
            ++branchDepth_;
            const bool ok = emitChain(b, sub->block);
            --branchDepth_;
            terminated_ = savedTerm;
            return ok;
        }
        return true;
    }

    /// 呼出し先が呼出しグラフ上で循環するか（legacy codegen の
    /// elideLeafProcFrames の cyclic 判定と同じ意味）。再帰・相互再帰は
    /// 呼出し時に interpreter スタックで再帰を検出して yield する必要が
    /// あるため、呼出しサイトに印を付ける。
    bool procMayRecurse(const ProcSig& callee) {
        const std::string& target = callee.vkey;
        auto it = cyclicCache_.find(target);
        if (it != cyclicCache_.end()) return it->second != 0;
        // callee の定義本体から到達できる全 procedures_call を走査し、
        // target 自身（vkey）へ戻る経路があるか調べる。
        const std::string wantCode = target.substr(0, target.size() - 2);
        std::unordered_set<std::string> seenBlocks;
        std::vector<const xyo::Block*> stack;
        if (callee.def && !callee.def->next.empty())
            if (const xyo::Block* n = blocks_.getBlock(callee.def->next))
                stack.push_back(n);
        bool cyclic = false;
        size_t visited = 0;
        while (!stack.empty() && !cyclic) {
            const xyo::Block* blk = stack.back();
            stack.pop_back();
            if (!blk || !seenBlocks.insert(blk->id).second) continue;
            if (++visited > 20000) break;
            if (blk->opcode == "procedures_call" && blk->mutation) {
                const std::string code = blk->mutation->attr("proccode");
                if (code == wantCode) {
                    cyclic = true;
                    break;
                }
                // 呼出し先の定義へ潜って推移的に辿る。
                const std::string defId =
                    blocks_.getProcedureDefinition(code);
                if (!defId.empty())
                    if (const xyo::Block* d = blocks_.getBlock(defId)) {
                        if (!d->next.empty())
                            if (const xyo::Block* n =
                                    blocks_.getBlock(d->next))
                                stack.push_back(n);
                    }
            }
            if (!blk->next.empty())
                if (const xyo::Block* n = blocks_.getBlock(blk->next))
                    stack.push_back(n);
            for (const auto& [name, in] : blk->inputs.items) {
                (void)name;
                if (!in.block.empty())
                    if (const xyo::Block* n = blocks_.getBlock(in.block))
                        stack.push_back(n);
                if (!in.shadow.empty() && in.shadow != in.block)
                    if (const xyo::Block* n = blocks_.getBlock(in.shadow))
                        stack.push_back(n);
            }
        }
        cyclicCache_[target] = cyclic ? 1 : 0;
        return cyclic;
    }

    /// legacy `canElideWarpRecursiveFrames` と同じ判定: callee が warp
    /// 定義で、定義本体（procedures_call を推移的に辿った全て）の op が
    /// スタック透過 allowlist に収まるなら、再帰検出フレームも再帰時
    /// yield も不要 — plain 呼出しでよい（warp 中は runtime の isWarp=1
    /// で yield 判定が常に偽になる）。
    bool procSafeRecursive(const ProcSig& sig) {
        static const std::unordered_set<std::string_view> kStackInvisible = {
            "data_variable", "data_setvariableto", "data_changevariableby",
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_lt", "operator_gt", "operator_equals",
            "operator_and", "operator_or", "operator_not",
            "argument_reporter_string_number", "argument_reporter_boolean",
            "argument_reporter", "control_if", "control_if_else",
            "data_addtolist", "data_deletealloflist", "data_itemoflist",
            "data_lengthoflist", "data_replaceitemoflist",
            "operator_letter_of", "operator_length", "operator_contains",
            "control_repeat_until", "control_repeat",
            "procedures_call",
            "control_stop",
        };
        const char* setting = std::getenv("XYO_JIT_RECURSIVE_FRAME_ELIDE");
        if (setting && setting[0] == '0') return false;
        std::unordered_set<std::string> seenBlocks;
        std::unordered_map<std::string, bool> memo;
        std::unordered_set<std::string> visiting;
        std::function<bool(const ProcSig&)> check =
            [&](const ProcSig& s) -> bool {
            // warp 定義だけが対象（non-warp callee が混ざる時点で unsafe）。
            if (!s.warp) return false;
            auto it = memo.find(s.vkey);
            if (it != memo.end()) return it->second;
            // 循環の再訪は暫定 safe（legacy の seenProcs と同じ扱い）。
            if (!visiting.insert(s.vkey).second) return true;
            bool ok = true;
            std::vector<const xyo::Block*> stack;
            if (s.def && !s.def->next.empty())
                if (const xyo::Block* n = blocks_.getBlock(s.def->next))
                    stack.push_back(n);
            size_t visited = 0;
            while (!stack.empty() && ok) {
                const xyo::Block* blk = stack.back();
                stack.pop_back();
                if (!blk) { ok = false; break; }
                if (blk->shadow) continue;
                if (!seenBlocks.insert(blk->id).second) continue;
                if (++visited > 20000) { ok = false; break; }
                if (!kStackInvisible.count(blk->opcode)) {
                    ok = false;
                    break;
                }
                if (blk->opcode == "procedures_call" && blk->mutation) {
                    const std::string code = blk->mutation->attr("proccode");
                    ProcSig* callee = nullptr;
                    if (!resolveProc(code, &callee) || !check(*callee)) {
                        ok = false;
                        break;
                    }
                }
                if (!blk->next.empty())
                    if (const xyo::Block* n = blocks_.getBlock(blk->next))
                        stack.push_back(n);
                for (const auto& [name, in] : blk->inputs.items) {
                    (void)name;
                    if (!in.block.empty())
                        if (const xyo::Block* n = blocks_.getBlock(in.block))
                            stack.push_back(n);
                    if (!in.shadow.empty() && in.shadow != in.block)
                        if (const xyo::Block* n =
                                blocks_.getBlock(in.shadow))
                            stack.push_back(n);
                }
            }
            visiting.erase(s.vkey);
            memo[s.vkey] = ok;
            return ok;
        };
        return check(sig);
    }

    /// false: 未定義手続き（Scratch の no-op 呼出し）。fail() 済みなら
    /// failed() が真になる。
    bool resolveProc(const std::string& proccode, ProcSig** out);
    bool emitCall(Builder& b, const xyo::Block& blk, Value* out = nullptr);
    /// 未対応 opcode を compat 層（プリミティブ直呼び）で実行する。
    bool emitCompatStatement(Builder& b, const xyo::Block& blk);
    Value emitCompatValue(Builder& b, const xyo::Block& blk);
    /// 直接 helper に落とせるスタック/reporter。legacy codegen の
    /// directInputs/directStack と同じ対応表。対象なら true。
    bool emitDirect(Builder& b, const xyo::Block& blk);
    Value emitDirectGet(Builder& b, const xyo::Block& blk);
    bool emitPendingProcedures();
};

bool ScriptBuilder::emitChain(Builder& b, std::string id, int depth) {
    if (depth > 256) return fail("chain too deep");
    while (!id.empty()) {
        const xyo::Block* blk = blocks_.getBlock(id);
        if (!blk) return fail("missing block " + id);
        if (!emitStatement(b, *blk)) return false;
        if (terminated_) break;  // stop / return の後ろは実行されない
        id = blk->next;
    }
    return true;
}

Value ScriptBuilder::emitReporter(Builder& b, const xyo::Block& blk) {
    if (forcedCompat(blk.opcode)) return emitCompatValue(b, blk);
    const std::string& op = blk.opcode;
    if (op == "procedures_call") {
        Value v;
        if (!emitCall(b, blk, &v)) return {};
        return v;
    }
    if (isNumericShadow(op) || op == "text") {
        const char* fieldNameStr = isNumericShadow(op) ? "NUM" : "TEXT";
        const xyo::BlockField* f = findField(blk, fieldNameStr, fieldNameStr);
        if (!f) return {};
        // 数値リテラルは数値、テキストは文字列のまま定数にする。
        if (isNumericShadow(op)) return numericLiteralValue(b, f->value);
        return constFromValue(b, f->value);
    }
    if (op == "data_variable") {
        const xyo::BlockField* f = findField(blk, "VARIABLE", "VARIABLE");
        if (!f) return {};
        // par-for ワーカー内のカウンタ読み出しは反復ローカルの
        // xyo.pfctr（実変数ではない。legacy の varOverride 相当）。
        if (pfCounterId_ == f->id)
            return b.create("xyo.pfctr", {}, {Type::value()})->result(0);
        return b.getVar(f->id, fieldName(*f));
    }
    if (isArgumentReporter(op)) {
        const xyo::BlockField* f = nullptr;
        auto it = blk.fields.find("VALUE");
        if (it != blk.fields.end()) f = &it->second;
        else if (!blk.fields.empty()) f = &blk.fields.begin()->second;
        if (!f || !f->value.isString() || !f->value.asStrPtr()) {
            fail("argument reporter without a name");
            return {};
        }
        const std::u16string pname = std::u16string(f->value.asStrPtr()->view());
        auto idx = argIndex_.find(pname);
        if (idx == argIndex_.end()) {
            // 未定義の引数名（退行した定義や組み込み名）: legacy と同じく
            // インタプリタ reporter 経由の compat 値へフォールバックする
            // （ビルド失敗ではない）。
            return emitCompatValue(b, blk);
        }
        return b.create("xyo.arg", {}, {Type::value()},
                        {{"index", Attr::integer(int64_t(idx->second))}})
            ->result(0);
    }
    if (op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_divide" ||
        op == "operator_mod") {
        Value a = emitInputValue(b, blk, "NUM1");
        Value c = emitInputValue(b, blk, "NUM2");
        if (failed()) return {};
        if (op == "operator_add") return b.add(a, c);
        if (op == "operator_subtract") return b.sub(a, c);
        if (op == "operator_multiply") return b.mul(a, c);
        if (op == "operator_divide") return b.div(a, c);
        return b.mod(a, c);
    }
    if (op == "operator_lt" || op == "operator_gt" ||
        op == "operator_equals") {
        Value a = emitInputValue(b, blk, "OPERAND1");
        Value c = emitInputValue(b, blk, "OPERAND2");
        if (failed()) return {};
        if (op == "operator_lt") return b.lt(a, c);
        if (op == "operator_gt") return b.gt(a, c);
        return b.eq(a, c);
    }
    if (op == "operator_and" || op == "operator_or") {
        Value a = emitInputValue(b, blk, "OPERAND1");
        Value c = emitInputValue(b, blk, "OPERAND2");
        if (failed()) return {};
        Value ta = b.truthy(a);
        Value tc = b.truthy(c);
        return b.create(op == "operator_and" ? "xyo.band" : "xyo.bor",
                        {ta, tc}, {Type::boolean()})
            ->result(0);
    }
    if (op == "operator_not") {
        Value a = emitInputValue(b, blk, "OPERAND");
        if (failed()) return {};
        Value ta = b.truthy(a);
        return b.create("xyo.bnot", {ta}, {Type::boolean()})->result(0);
    }
    if (op == "operator_round") {
        Value a = emitInputValue(b, blk, "NUM");
        if (failed()) return {};
        return b.create("xyo.round", {a}, {Type::value()})->result(0);
    }
    if (op == "operator_mathop") {
        const xyo::BlockField* f = findField(blk, "OPERATOR", "OPERATOR");
        if (!f) return {};
        std::string name = fieldName(*f);
        for (char& ch : name)
            if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
        static const std::map<std::string, int64_t> kOps = {
            {"abs", 0},   {"floor", 1}, {"ceiling", 2}, {"sqrt", 3},
            {"sin", 4},   {"cos", 5},   {"tan", 6},     {"asin", 7},
            {"acos", 8},  {"atan", 9},  {"ln", 10},     {"log", 11},
            {"e ^", 12},  {"10 ^", 13},
        };
        auto it = kOps.find(name);
        if (it == kOps.end()) {
            fail("unknown mathop: " + name);
            return {};
        }
        Value a = emitInputValue(b, blk, "NUM");
        if (failed()) return {};
        return b.create("xyo.mathop", {a}, {Type::value()},
                        {{"op", Attr::integer(it->second)}})
            ->result(0);
    }
    if (op == "operator_join") {
        Value a = emitInputValue(b, blk, "STRING1");
        Value c = emitInputValue(b, blk, "STRING2");
        if (failed()) return {};
        return b.create("xyo.join", {a, c}, {Type::value()})->result(0);
    }
    if (op == "operator_letter_of") {
        Value idx = emitInputValue(b, blk, "LETTER");
        Value s = emitInputValue(b, blk, "STRING");
        if (failed()) return {};
        return b.create("xyo.letterof", {idx, s}, {Type::value()})->result(0);
    }
    if (op == "operator_length") {
        Value s = emitInputValue(b, blk, "STRING");
        if (failed()) return {};
        return b.create("xyo.strlen", {s}, {Type::value()})->result(0);
    }
    if (op == "operator_contains") {
        Value s = emitInputValue(b, blk, "STRING1");
        Value c = emitInputValue(b, blk, "STRING2");
        if (failed()) return {};
        return b.create("xyo.strcontains", {s, c}, {Type::value()})
            ->result(0);
    }
    // ホットループの条件で使われる時計系は native に落とす（compat 経由の
    // プリミティブ呼出しは 1 回あたり数百 ns かかる）。
    if (op == "sound_volume")
        return b.create("xyo.snd.volume", {}, {Type::value()})->result(0);
    if (op == "sensing_dayssince2000")
        return b.create("xyo.days2000", {}, {Type::value()})->result(0);
    if (op == "sensing_timer")
        return b.create("xyo.timer", {}, {Type::value()})->result(0);
    if (Value v = emitDirectGet(b, blk); v.valid() || failed())
        return v;
    if (op == "data_itemoflist" || op == "data_itemnumoflist" ||
        op == "data_listlength" || op == "data_listcontainsitem" ||
        op == "data_lengthoflist" || op == "data_listcontents") {
        const xyo::BlockField* f = findField(blk, "LIST", "LIST");
        if (!f) {
            // フィールド欠落 = インタプリタの obj()==nullptr 経路。
            return op == "data_listcontents"
                       ? b.cst(xyo::Value::emptyStr().bits)
                       : Value{};
        }
        Value list = listPtr(b, *f);
        if (op == "data_listcontents")
            return b.create("xyo.list.contents", {list}, {Type::value()})
                ->result(0);
        if (op == "data_lengthoflist" || op == "data_listlength")
            return b.create("xyo.list.length", {list}, {Type::value()})
                ->result(0);
        Value idx = emitInputValue(
            b, blk, op == "data_itemoflist" ? "INDEX" : "ITEM");
        if (failed()) return {};
        const char* name = op == "data_itemoflist" ? "xyo.list.item"
                         : op == "data_itemnumoflist" ? "xyo.list.itemnum"
                                                     : "xyo.list.contains";
        return b.create(name, {list, idx}, {Type::value()})->result(0);
    }
    return emitCompatValue(b, blk);
}

bool ScriptBuilder::emitCompatStatement(Builder& b, const xyo::Block& blk) {
    if (std::getenv("XYO_MLIR_COMPAT_TRACE"))
        std::fprintf(stderr, "[mlir-compat] %s\n", blk.opcode.c_str());
    // compat のプリミティブは内部で Fiber を yield し得る（xyo_rt_compat の
    // resume ループ）。コルーチンはそれを越えて停止できない。
    internalYield_ = true;
    if (!allowCompat_)
        return fail("unsupported statement (strict): " + blk.opcode);
    if (blk.inputs.has("SUBSTACK") || blk.inputs.has("SUBSTACK2"))
        return fail("compat statement with a substack: " + blk.opcode);
    BlockCached* bc = blocks_.getCachedExecute(blk.id);
    if (!bc || !bc->blockFunction)
        return fail("no primitive for " + blk.opcode);
    std::vector<Value> args;
    std::string names;
    for (const auto& [name, in] : blk.inputs.items) {
        (void)in;
        if (name == "SUBSTACK" || name == "SUBSTACK2") continue;
        Value v = emitInputValue(b, blk, name.c_str());
        if (failed()) return false;
        if (!names.empty()) names.push_back('\x1e');
        names += name;
        args.push_back(v);
    }
    b.create("xyo.compat.call", std::move(args), {},
             {{"bc", Attr::integer(static_cast<int64_t>(
                  reinterpret_cast<uintptr_t>(bc)))},
              {"args", Attr::str(names)}});
    return true;
}

Value ScriptBuilder::emitCompatValue(Builder& b, const xyo::Block& blk) {
    if (std::getenv("XYO_MLIR_COMPAT_TRACE"))
        std::fprintf(stderr, "[mlir-compat-r] %s\n", blk.opcode.c_str());
    internalYield_ = true;
    if (!allowCompat_) {
        fail("unsupported reporter (strict): " + blk.opcode);
        return {};
    }
    if (blk.inputs.has("SUBSTACK") || blk.inputs.has("SUBSTACK2")) {
        fail("compat reporter with a substack: " + blk.opcode);
        return {};
    }
    BlockCached* bc = blocks_.getCachedExecute(blk.id);
    if (!bc || !bc->blockFunction) {
        fail("no primitive for " + blk.opcode);
        return {};
    }
    std::vector<Value> args;
    std::string names;
    for (const auto& [name, in] : blk.inputs.items) {
        (void)in;
        if (name == "SUBSTACK" || name == "SUBSTACK2") continue;
        Value v = emitInputValue(b, blk, name.c_str());
        if (failed()) return {};
        if (!names.empty()) names.push_back('\x1e');
        names += name;
        args.push_back(v);
    }
    return b.create("xyo.compat.get", std::move(args), {Type::value()},
                    {{"bc", Attr::integer(static_cast<int64_t>(
                          reinterpret_cast<uintptr_t>(bc)))},
                     {"args", Attr::str(names)}})
        ->result(0);
}

bool ScriptBuilder::resolveProc(const std::string& proccode, ProcSig** out) {
    *out = nullptr;
    // 既存 codegen と同じ順序: 先に prototype（mutation）を parse させる。
    // getProcedureDefinition は定義ブロック側の proccode 属性を走査する
    // ため、parse 前だと必ず「未定義」に見える。
    ProcSig sig;
    if (!blocks_.getProcedureParamNamesIdsAndDefaults(
            proccode, sig.names, sig.ids, sig.defaults)) {
        return false;  // 未定義: 呼出しは no-op（Scratch と同じ）
    }
    const std::string defId = blocks_.getProcedureDefinition(proccode);
    const xyo::Block* def = defId.empty() ? nullptr : blocks_.getBlock(defId);
    if (!def) return false;  // 未定義: no-op
    const xyo::BlockInput* cb = def->inputs.find("custom_block");
    const xyo::Block* proto = cb ? blocks_.getBlock(cb->block) : nullptr;
    const bool warp = (proto && proto->mutation) ? proto->mutation->warp()
                                                 : false;
    const std::string vkey = proccode + (warp ? "|w" : "|n");
    auto found = procs_.find(vkey);
    if (found != procs_.end()) {
        *out = &found->second;
        return true;
    }

    sig.vkey = vkey;
    sig.def = def;
    sig.warp = warp;
    const xyo::BlockInput* cbp = def->inputs.find("custom_block");
    const xyo::Block* protop = cbp ? blocks_.getBlock(cbp->block) : nullptr;
    sig.reporter = (protop && protop->mutation) ? protop->mutation->isReporterCall()
                                               : false;
    sig.fnName = entryName_ + "_p" + std::to_string(procs_.size());
    std::vector<Type> argTypes{Type::ctx()};
    // reporter 手続きは戻り値スロットへのポインタを第 2 引数で受け取る
    // （void 関数のままなので epilogue/型の変更が要らない）。
    if (sig.reporter) argTypes.push_back(Type::ptr());
    for (size_t i = 0; i < sig.ids.size(); ++i) argTypes.push_back(Type::i64());
    AttrMap fnAttrs{{"warp", Attr::integer(warp ? 1 : 0)},
                     {"proc", Attr::integer(1)}};
    if (sig.reporter) fnAttrs["ret"] = Attr::integer(1);
    sig.fn = m_.createFunction(sig.fnName, argTypes, std::move(fnAttrs));
    procNames_[vkey] = sig.fnName;
    auto [it, inserted] = procs_.emplace(vkey, std::move(sig));
    (void)inserted;
    pending_.push_back(vkey);
    *out = &it->second;
    return true;
}

bool ScriptBuilder::emitCall(Builder& b, const xyo::Block& blk, Value* out) {
    if (!blk.mutation) return fail("procedures_call without mutation");
    const std::string proccode = blk.mutation->attr("proccode");
    ProcSig* sig = nullptr;
    if (!resolveProc(proccode, &sig)) {
        // 未定義手続き呼出しは no-op（引数も評価しない）。既存 codegen と
        // Scratch の procedures_call と同じ挙動。
        return !failed();
    }
    std::vector<Value> args;
    args.reserve(sig->ids.size());
    for (size_t i = 0; i < sig->ids.size(); ++i) {
        const xyo::BlockInput* in = findInput(blk, sig->ids[i].c_str());
        if (in && (!in->block.empty() || !in->shadow.empty())) {
            Value v = emitInputValue(b, blk, sig->ids[i].c_str());
            if (failed()) return false;
            args.push_back(v);
        } else if (i < sig->defaults.size()) {
            Value v = constFromValue(b, sig->defaults[i]);
            if (failed()) return false;
            args.push_back(v);
        } else {
            args.push_back(b.cst(xyo::Value::num(0).bits));
        }
    }
    AttrMap callAttrs{{"callee", Attr::str(sig->fnName)}};
    if (sig->reporter) callAttrs["ret"] = Attr::integer(1);
    // 呼出しグラフが循環する callee は、非 warp での再帰呼出しが yield
    // する（Scratch の isRecursiveCall）。インタプリタスタックの検出に
    // 必要なので、legacy と同じく xyo_proc_push_ex/pop のフレームを
    // 呼出しの前後へ emit するよう lowering に印を付ける。
    // これは stackless::lowerRecursive（手続きコルーチン化）の停止点にも
    // なるため **coro モードのときだけ**付ける — Fiber/hotspot 経路では
    // フレームの push/pop が純粋なオーバーヘッドになる（再帰自体は
    // フレーム無しでも正しく動く: 従来の MLIR 経路は常にフレーム無しで
    // 全テストを通過している）。
    if (coroMode_ && procMayRecurse(*sig) && !procSafeRecursive(*sig)) {
        BlockCached* bc = blocks_.getCachedExecute(blk.id);
        if (!bc)
            return fail("recursive call without cached block");
        callAttrs["rec"] = Attr::integer(1);
        callAttrs["bc"] = Attr::integer(static_cast<int64_t>(
            reinterpret_cast<uintptr_t>(bc)));
        callAttrs["code"] = Attr::str(proccode);
        callAttrs["bid"] = Attr::str(blk.id);
        callAttrs["defwarp"] = Attr::integer(sig->warp ? 1 : 0);
        callAttrs["depth"] = Attr::integer(branchDepth_);
    }
    const bool wantValue = out != nullptr && sig->reporter;
    Op* callOp = wantValue
                     ? b.create("xyo.call", std::move(args), {Type::value()},
                                std::move(callAttrs))
                     : b.create("xyo.call", std::move(args), {},
                                std::move(callAttrs));
    if (wantValue) *out = callOp->result(0);
    return true;
}

namespace {

/// legacy codegen の directInputs/directStack と同じ対応表。
/// sym="" は no-op ブロック。yielding は内部で xyo_yield する helper。
/// 注意: inputs/fields は **std::vector** で持つ。initializer_list を
/// 静的オブジェクトのメンバにすると、初期化式の終わりに backing array
/// が破棄されて dangling になる（null ポインタ読み出しで落ちる）。
struct DirectMap {
    const char* sym;
    std::vector<const char*> inputs;
    std::vector<const char*> fields;
    bool yields;
};

const std::unordered_map<std::string_view, DirectMap>& directStackMap() {
    static const std::unordered_map<std::string_view, DirectMap> m = {
        {"motion_movesteps",    {"xyo_mot_movesteps",{"STEPS"},{},false}},
        {"motion_goto",         {"xyo_mot_goto",{"TO"},{},false}},
        {"motion_turnright",    {"xyo_mot_turnright",{"DEGREES"},{},false}},
        {"motion_turnleft",     {"xyo_mot_turnleft",{"DEGREES"},{},false}},
        {"motion_pointindirection",{"xyo_mot_pointdir",{"DIRECTION"},{},false}},
        {"motion_pointtowards", {"xyo_mot_pointtowards",{"TOWARDS"},{},false}},
        {"motion_changexby",    {"xyo_mot_changex",{"DX"},{},false}},
        {"motion_setx",         {"xyo_mot_setx",{"X"},{},false}},
        {"motion_changeyby",    {"xyo_mot_changey",{"DY"},{},false}},
        {"motion_sety",         {"xyo_mot_sety",{"Y"},{},false}},
        {"motion_ifonedgebounce",{"xyo_mot_edgebounce",{},{},false}},
        {"motion_gotoxy",       {"xyo_mot_gotoxy",{"X","Y"},{},false}},
        {"motion_glidesecstoxy",{"xyo_mot_glidexy",{"SECS","X","Y"},{},true}},
        {"motion_glideto",      {"xyo_mot_glideto",{"SECS","TO"},{},true}},
        {"motion_setrotationstyle",{"xyo_mot_setrotstyle",{},{"STYLE"},false}},
        {"looks_say",           {"xyo_looks_say",{"MESSAGE"},{},false}},
        {"looks_think",         {"xyo_looks_think",{"MESSAGE"},{},false}},
        {"looks_sayforsecs",    {"xyo_looks_saysecs",{"MESSAGE","SECS"},{},true}},
        {"looks_thinkforsecs",  {"xyo_looks_thinksecs",{"MESSAGE","SECS"},{},true}},
        {"looks_show",          {"xyo_looks_show",{},{},false}},
        {"looks_hide",          {"xyo_looks_hide",{},{},false}},
        {"looks_switchcostumeto",{"xyo_looks_switchcostume",{"COSTUME"},{},false}},
        {"looks_nextcostume",   {"xyo_looks_nextcostume",{},{},false}},
        {"looks_switchbackdropto",{"xyo_looks_switchbackdrop",{"BACKDROP"},{},false}},
        {"looks_nextbackdrop",  {"xyo_looks_nextbackdrop",{},{},false}},
        {"looks_switchbackdroptoandwait",
         {"xyo_looks_switchbackdropwait",{"BACKDROP"},{},true}},
        {"looks_changeeffectby",{"xyo_looks_changeeffect",{"CHANGE"},{"EFFECT"},false}},
        {"looks_seteffectto",   {"xyo_looks_seteffect",{"VALUE"},{"EFFECT"},false}},
        {"looks_cleargraphiceffects",{"xyo_looks_cleareffects",{},{},false}},
        {"looks_changesizeby",  {"xyo_looks_changesize",{"CHANGE"},{},false}},
        {"looks_setsizeto",     {"xyo_looks_setsize",{"SIZE"},{},false}},
        {"looks_gotofrontback", {"xyo_looks_frontback",{},{"FRONT_BACK"},false}},
        {"looks_goforwardbackwardlayers",
         {"xyo_looks_fwdbacklayers",{"NUM"},{"FORWARD_BACKWARD"},false}},
        {"event_broadcast",     {"xyo_broadcast_v",{"BROADCAST_INPUT"},{},false}},
        {"control_incr_counter",{"xyo_ctl_incrcounter",{},{},false}},
        {"control_clear_counter",{"xyo_ctl_clearcounter",{},{},false}},
        {"control_create_clone_of",{"xyo_ctl_createclone",{"CLONE_OPTION"},{},false}},
        {"sensing_setdragmode", {"xyo_sns_setdragmode",{"DRAG_MODE"},{},false}},
        {"sensing_resettimer",  {"xyo_sns_resettimer",{},{},false}},
        {"sensing_askandwait",  {"xyo_sns_askwait",{"QUESTION"},{},true}},
        {"sound_play",          {"xyo_snd_play",{"SOUND_MENU"},{},false}},
        {"sound_playuntildone", {"xyo_snd_playwait",{"SOUND_MENU"},{},true}},
        {"sound_stopallsounds", {"xyo_snd_stopall",{},{},false}},
        {"sound_seteffectto",   {"xyo_snd_seteffect",{"VALUE"},{"EFFECT"},true}},
        {"sound_changeeffectby",{"xyo_snd_changeeffect",{"VALUE"},{"EFFECT"},true}},
        {"sound_cleareffects",  {"xyo_snd_cleareffects",{},{},false}},
        {"pen_penDown",         {"xyo_pen_down",{},{},false}},
        {"pen_penUp",           {"xyo_pen_up",{},{},false}},
        {"pen_clear",           {"xyo_pen_clear",{},{},false}},
        {"pen_stamp",           {"xyo_pen_stamp",{},{},false}},
        {"pen_setPenColorToColor",{"xyo_pen_setcolor",{"COLOR"},{},false}},
        {"pen_changePenColorParamBy",{"xyo_pen_changeparam",{"COLOR_PARAM","VALUE"},{},false}},
        {"pen_setPenColorParamTo",{"xyo_pen_setparam",{"COLOR_PARAM","VALUE"},{},false}},
        {"pen_changePenSizeBy", {"xyo_pen_changesize",{"SIZE"},{},false}},
        // 実害のない no-op（インタプリタも noop 登録）。
        {"looks_hideallsprites",{"",{},{},false}},
        {"looks_changestretchby",{"",{},{},false}},
        {"looks_setstretchto",  {"",{},{},false}},
        {"motion_scroll_right", {"",{},{},false}},
        {"motion_scroll_up",    {"",{},{},false}},
        {"motion_align_scene",  {"",{},{},false}},
        {"motion_xscroll",      {"",{},{},false}},
        {"motion_yscroll",      {"",{},{},false}},
    };
    return m;
}

const std::unordered_map<std::string_view, DirectMap>& directGetMap() {
    static const std::unordered_map<std::string_view, DirectMap> m = {
        {"control_get_counter", {"xyo_ctl_getcounter",{},{},false}},
        {"motion_xposition",    {"xyo_mot_getx",{},{},false}},
        {"motion_yposition",    {"xyo_mot_gety",{},{},false}},
        {"motion_direction",    {"xyo_mot_getdir",{},{},false}},
        {"looks_size",          {"xyo_looks_size",{},{},false}},
        {"looks_costumenumbername",{"xyo_looks_costumenumname",{},{"NUMBER_NAME"},false}},
        {"looks_backdropnumbername",{"xyo_looks_backdropnumname",{},{"NUMBER_NAME"},false}},
        {"sensing_mousex",      {"xyo_sns_mousex",{},{},false}},
        {"sensing_mousey",      {"xyo_sns_mousey",{},{},false}},
        {"sensing_mousedown",   {"xyo_sns_mousedown",{},{},false}},
        {"sensing_keypressed",  {"xyo_sns_keypressed",{"KEY_OPTION"},{},false}},
        {"sensing_touchingobject",{"xyo_sns_touchingobj",{"TOUCHINGOBJECTMENU"},{},false}},
        {"sensing_touchingcolor",{"xyo_sns_touchingcolor",{"COLOR"},{},false}},
        {"sensing_coloristouchingcolor",{"xyo_sns_coloristouching",{"COLOR2","COLOR"},{},false}},
        {"sensing_distanceto",  {"xyo_sns_distanceto",{"DISTANCETOMENU"},{},false}},
        {"sensing_loudness",    {"xyo_sns_loudness",{},{},false}},
        {"sensing_loud",        {"xyo_sns_loud",{},{},false}},
        {"sensing_answer",      {"xyo_sns_answer",{},{},false}},
        {"sensing_username",    {"xyo_sns_username",{},{},false}},
        {"sensing_online",      {"xyo_sns_online",{},{},false}},
        {"sensing_current",     {"xyo_sns_current",{},{"CURRENTMENU"},false}},
        {"sensing_dayssince2000",{"xyo_sns_dayssince2000",{},{},false}},
        {"sensing_of",          {"xyo_sns_of",{"OBJECT"},{"PROPERTY"},false}},
        {"sound_volume",        {"xyo_snd_getvolume",{},{},false}},
        {"operator_random",     {"xyo_random",{"FROM","TO"},{},false}},
    };
    return m;
}

} // namespace

bool ScriptBuilder::emitDirect(Builder& b, const xyo::Block& blk) {
    static const bool disabled = [] {
        const char* e = std::getenv("XYO_MLIR_DIRECT");
        return e && e[0] == '0';
    }();
    if (disabled) return false;
    auto it = directStackMap().find(blk.opcode);
    if (it == directStackMap().end()) return false;
    const DirectMap& d = it->second;
    if (!d.sym || !*d.sym) return true;  // no-op ブロック
    std::vector<Value> args;
    for (const char* n : d.inputs) {
        if (!n) {
            fail("direct op with null input name: " + blk.opcode);
            return false;
        }
        Value v = emitInputValue(b, blk, n);
        if (failed()) return false;
        args.push_back(v);
    }
    for (const char* n : d.fields) {
        const xyo::BlockField* f = findField(blk, n, n);
        if (!f) { fail("missing field "); return false; }
        args.push_back(constFromValue(b, f->value));
        if (failed()) return false;
    }
    AttrMap attrs{{"sym", Attr::str(d.sym)}};
    if (d.yields) attrs["yields"] = Attr::integer(1);
    b.create("xyo.direct", std::move(args), {}, attrs);
    return true;
}

Value ScriptBuilder::emitDirectGet(Builder& b, const xyo::Block& blk) {
    static const bool disabled = [] {
        const char* e = std::getenv("XYO_MLIR_DIRECT_GET");
        return e && e[0] == '0';
    }();
    if (disabled) return {};
    auto it = directGetMap().find(blk.opcode);
    if (it == directGetMap().end()) return {};
    const DirectMap& d = it->second;
    std::vector<Value> args;
    for (const char* n : d.inputs) {
        Value v = emitInputValue(b, blk, n);
        if (failed()) return {};
        args.push_back(v);
    }
    for (const char* n : d.fields) {
        const xyo::BlockField* f = findField(blk, n, n);
        if (!f) { fail("missing field "); return {}; }
        args.push_back(constFromValue(b, f->value));
        if (failed()) return {};
    }
    return b.create("xyo.direct.get", std::move(args), {Type::value()},
                    {{"sym", Attr::str(d.sym)}})->result(0);
}

bool ScriptBuilder::emitStatement(Builder& b, const xyo::Block& blk) {
    if (std::getenv("XYO_MLIR_OP_TRACE"))
        std::fprintf(stderr, "[op] %s\n", blk.opcode.c_str());
    if (forcedCompat(blk.opcode) && blk.opcode != "procedures_call")
        return emitCompatStatement(b, blk);
    const std::string& op = blk.opcode;
    if (op == "procedures_call") return emitCall(b, blk);
    if (op == "data_setvariableto") {
        const xyo::BlockField* f = findField(blk, "VARIABLE", "VARIABLE");
        if (!f) return false;
        Value v = emitInputValue(b, blk, "VALUE");
        if (failed()) return false;
        b.setVar(f->id, fieldName(*f), v);
        return true;
    }
    // サウンドの音量系は compat を通さず native に落とす。
    // compat 経由だと、直前のブロックがオーディオ待ち（PROMISE_WAIT）に
    // 入ったときに戻り値が空文字になり、clone 内の `volume` が '' に
    // なる（parity の Sound: Clone Volume が落ちる原因）。
    if (op == "sound_setvolumeto" || op == "sound_changevolumeby") {
        Value v = emitInputValue(b, blk, "VOLUME");
        if (failed()) return false;
        b.create(op == "sound_setvolumeto" ? "xyo.snd.setvolume"
                                           : "xyo.snd.changevolume",
                 {v}, {});
        return true;
    }
    // pen size は既存 codegen の xyo_pen_setsize と同じ helper に落とす
    // （テストが呼出し回数カウンタ xyo_pen_size_calls を検査する）。
    if (op == "pen_setPenSizeTo") {
        Value v = emitInputValue(b, blk, "SIZE");
        if (failed()) return false;
        b.create("xyo.pen.setsize", {v}, {});
        return true;
    }
    if (op == "data_changevariableby") {
        const xyo::BlockField* f = findField(blk, "VARIABLE", "VARIABLE");
        if (!f) return false;
        // Scratch は引数を先に評価し、その後で変数を読む。
        Value delta = emitInputValue(b, blk, "VALUE");
        if (failed()) return false;
        Value cur = b.getVar(f->id, fieldName(*f));
        b.setVar(f->id, fieldName(*f), b.add(cur, delta));
        return true;
    }
    if (op == "data_addtolist" || op == "data_deleteoflist" ||
        op == "data_deletealloflist" || op == "data_insertatlist" ||
        op == "data_replaceitemoflist") {
        const xyo::BlockField* f = findField(blk, "LIST", "LIST");
        if (!f) return false;
        Value list = listPtr(b, *f);
        if (op == "data_deletealloflist") {
            b.create("xyo.list.deleteall", {list}, {});
            return true;
        }
        if (op == "data_addtolist") {
            Value item = emitInputValue(b, blk, "ITEM");
            if (failed()) return false;
            b.create("xyo.list.add", {list, item}, {});
            return true;
        }
        Value idx = emitInputValue(b, blk, "INDEX");
        if (failed()) return false;
        if (op == "data_deleteoflist") {
            b.create("xyo.list.delete", {list, idx}, {});
            return true;
        }
        Value item = emitInputValue(b, blk, "ITEM");
        if (failed()) return false;
        b.create(op == "data_insertatlist" ? "xyo.list.insert"
                                           : "xyo.list.replace",
                 {list, idx, item}, {});
        return true;
    }
    if (op == "control_if" || op == "control_if_else") {
        Value cond = emitCondition(b, blk, "CONDITION");
        if (failed()) return false;
        Op* ifOp = b.create("xyo.if", {cond}, {});
        for (int r = 0; r < 2; ++r) {
            Region* region = ifOp->addRegion();
            Block* body = region->addBlock();
            Builder nb(m_);
            nb.setBlock(body);
            if (r == 0) {
                if (!emitSubstack(nb, blk, "SUBSTACK")) return false;
            } else if (op == "control_if_else") {
                if (!emitSubstack(nb, blk, "SUBSTACK2")) return false;
            }
            nb.createTerminator("xyo.yield", {});
        }
        return true;
    }
    if (op == "control_repeat" || op == "control_forever" ||
        op == "control_repeat_until" || op == "control_while") {
        hasLoop_ = true;
        const bool repeat = op == "control_repeat";
        const bool forever = op == "control_forever";
        const bool until = op == "control_repeat_until";
        AttrMap attrs;
        // warp 手続き内のループは全て「静的に warp」と印付ける。
        // while 系（repeat until / while）は脱出値が cond の phi から来る
        // ため promotion の書き戻しはそちらを使う（runWhile）。印を付ける
        // と反復ごとの spill/reload が消え、ハンドオフ時だけ
        // xyo.warp_yield が spill/reload する。
        // 実行時 warp 判定に任せる従来動作へは XYO_MLIR_WHILEWARP=0。
        static const bool whileWarp = [] {
            const char* e = std::getenv("XYO_MLIR_WHILEWARP");
            return !(e && e[0] == '0');
        }();
        if (inWarp_ && (repeat || forever || whileWarp))
            attrs["warp"] = Attr::integer(1);
        std::vector<Value> operands;
        Value count;
        if (repeat) {
            count = emitInputValue(b, blk, "TIMES");
            if (failed()) return false;
            operands.push_back(count);
        }
        // 線形漸化式ループ（行列累乗 O(log n)）。本体が状態変数の affine
        // 更新だけなら xyo.matpow.try が真を返して逐次ループをスキップする。
        // 範囲証明が立たないときは else の通常 xyo.repeat へ落ちる。
        // （MEXP-001: ここから matpow ブロック — 全書き換え時も保持すること）
        if (repeat && inWarp_ && !suppressMatPowCond_) {
            const xyo::BlockInput* mpSub = findInput(blk, "SUBSTACK");
            static const bool matPowOn = [] {
                const char* e = std::getenv("XYO_MATPOW");
                return !e || e[0] != '0';
            }();
            std::string mpWhy;
            auto mpOpt = (matPowOn && mpSub && !mpSub->block.empty())
                ? jit::analyzeMatPow(blocks_, &blk, &mpWhy)
                : std::optional<jit::MatPowPlan>{};
            if (std::getenv("XYO_MATPOW_DEBUG"))
                std::fprintf(stderr, "[mlir] matpow %s: plan=%d %s\n",
                             blk.id.c_str(), int(mpOpt.has_value()),
                             mpWhy.c_str());
            if (mpOpt && mpOpt->condKind == 0) {
                jit::MatPowPlan& plan = *mpOpt;
                std::string varsAttr;
                for (const xyo::BlockField* vf : plan.vars) {
                    if (!varsAttr.empty()) varsAttr.push_back('\x1e');
                    varsAttr += vf->id;
                    varsAttr.push_back('\x1f');
                    varsAttr += fieldName(*vf);
                }
                const int64_t dim = int64_t(plan.vars.size()) + 1;
                auto hex64 = [](uint64_t v) {
                    char buf[17];
                    std::snprintf(buf, sizeof buf, "%016llx",
                                  (unsigned long long)v);
                    return std::string(buf);
                };
                std::string matAttr;
                matAttr.reserve(plan.mat.size() * 16);
                for (int64_t m : plan.mat) matAttr += hex64(uint64_t(m));
                std::string postAttr;
                postAttr.reserve(plan.postSrc.size() * 8);
                for (int32_t ps : plan.postSrc) {
                    char buf[9];
                    std::snprintf(buf, sizeof buf, "%08x", unsigned(ps));
                    postAttr += buf;
                }
                std::string writAttr;
                writAttr.reserve(plan.written.size());
                for (char wv : plan.written)
                    writAttr.push_back(wv ? '1' : '0');
                Op* tryOp = b.create(
                    "xyo.matpow.try", {count}, {Type::boolean()},
                    {{"vars", Attr::str(std::move(varsAttr))},
                     {"mat", Attr::str(std::move(matAttr))},
                     {"post", Attr::str(std::move(postAttr))},
                     {"writ", Attr::str(std::move(writAttr))},
                     {"flags", Attr::integer(plan.needsNonnegState ? 1 : 0)},
                     {"dim", Attr::integer(dim)},
                     {"bound", Attr::integer(plan.stmtBound)}});
                Op* ifOp = b.create("xyo.if", {tryOp->result(0)}, {});
                Region* tr = ifOp->addRegion();
                {
                    Block* tb = tr->addBlock();
                    Builder hb(m_);
                    hb.setBlock(tb);
                    hb.createTerminator("xyo.yield", {});
                }
                Region* er = ifOp->addRegion();
                {
                    Block* eb = er->addBlock();
                    Builder ebb(m_);
                    ebb.setBlock(eb);
                    const bool savedSuppress = suppressMatPowCond_;
                    suppressMatPowCond_ = true;
                    const bool okEmit = emitStatement(ebb, blk);
                    suppressMatPowCond_ = savedSuppress;
                    if (!okEmit) return false;
                    ebb.createTerminator("xyo.yield", {});
                }
                return true;
            }
        }
        // 並列 append ループ（par-for）。「反復カウンタ + 各リストへ
        // 1 個ずつ append」の warp repeat をワーカー分割へ委譲する。
        // 成功なら xyo.parfor.try が真を返して逐次ループをスキップ、
        // 失敗（小さい反復・非数値・上限越え等）なら else の通常
        // xyo.repeat へ落ちる — 意味論は逐次と完全一致。
        if (repeat && inWarp_) {
            const xyo::BlockInput* pfSub = findInput(blk, "SUBSTACK");
            static const bool parForOn = [] {
                const char* e = std::getenv("XYO_PARFOR");
                return !e || e[0] != '0';
            }();
            std::string why;
            auto planOpt =
                (parForOn && pfSub && !pfSub->block.empty())
                    ? jit::analyzeParFor(blocks_, &blk, &why)
                    : std::optional<jit::ParForPlan>{};
            if (planOpt) {
                jit::ParForPlan& plan = *planOpt;
                // lists = "id\x1fname\x1eid\x1fname..."
                std::string listsAttr;
                for (const xyo::BlockField* lf : plan.lists) {
                    if (!listsAttr.empty()) listsAttr.push_back('\x1e');
                    listsAttr += lf->id;
                    listsAttr.push_back('\x1e');
                    listsAttr += fieldName(*lf);
                }
                Op* tryOp = b.create(
                    "xyo.parfor.try", {count}, {Type::boolean()},
                    {{"counter", Attr::str(plan.counterField->id)},
                     {"cname", Attr::str(fieldName(*plan.counterField))},
                     {"delta", Attr::real(plan.delta)},
                     {"lists", Attr::str(listsAttr)}});
                Region* pfR = tryOp->addRegion();
                Block* pfBody = pfR->addBlock();
                Builder pfb(m_);
                pfb.setBlock(pfBody);
                // ITEM 式だけを emit する（カウンタの change は
                // arg で渡され、変数へは書かない）。
                pfCounterId_ = plan.counterField->id;
                for (const xyo::Block* cb = plan.bodyHead; cb;
                     cb = cb->next.empty() ? nullptr
                                           : blocks_.getBlock(cb->next)) {
                    if (cb->opcode != "data_addtolist") continue;
                    const xyo::BlockField* lf =
                        cb->fields.find("LIST") != cb->fields.end()
                            ? &cb->fields.at("LIST") : nullptr;
                    if (!lf) return fail("parfor append without LIST");
                    int64_t slot = -1;
                    for (size_t k = 0; k < plan.lists.size(); ++k)
                        if (plan.lists[k] == lf) {
                            slot = static_cast<int64_t>(k);
                            break;
                        }
                    if (slot < 0) return fail("parfor list not in plan");
                    Value item = emitInputValue(pfb, *cb, "ITEM");
                    if (failed()) return false;
                    pfb.create("xyo.pfwrite", {item}, {},
                               {{"slot", Attr::integer(slot)}});
                }
                pfCounterId_.clear();
                pfb.createTerminator("xyo.yield", {});
                Op* ifOp = b.create("xyo.if", {tryOp->result(0)}, {});
                Region* tr = ifOp->addRegion();
                {
                    Block* tb = tr->addBlock();
                    Builder hb(m_);
                    hb.setBlock(tb);
                    hb.createTerminator("xyo.yield", {});
                }
                Region* er = ifOp->addRegion();
                {
                    Block* eb = er->addBlock();
                    Builder ebb(m_);
                    ebb.setBlock(eb);
                    // 並列化に失敗したときの逐次 repeat（普通の経路）。
                    Op* seq = ebb.create("xyo.repeat", {count}, {},
                                         std::move(attrs));
                    Region* seqR = seq->addRegion();
                    Block* seqBody = seqR->addBlock();
                    Builder sb2(m_);
                    sb2.setBlock(seqBody);
                    if (!emitSubstack(sb2, blk, "SUBSTACK")) return false;
                    if (inWarp_)
                        sb2.createTerminator(
                            "xyo.loop_yield", {},
                            {{"warp", Attr::integer(1)}});
                    else
                        sb2.createTerminator("xyo.loop_yield", {});
                    sb2.createTerminator("xyo.yield", {});
                    ebb.createTerminator("xyo.yield", {});
                }
                return true;
            }
        }
        // 空本体の warp repeat は既存 JIT と同じ専用ヘルパで回す（呼出しも
        // 代入も無いので、本体を回す代わりに C 側のタイトループで数える）。
        const xyo::BlockInput* sub = findInput(blk, "SUBSTACK");
        const bool emptyBody = !sub || sub->block.empty();
        if (repeat && emptyBody && inWarp_) {
            b.create("xyo.empty_repeat", {count}, {});
            return true;
        }
        // ループ不変 pen setter のホイスト（legacy の XYO_PENHOIST 相当）。
        // forever は本体が必ず 1 回走るので実行ガードが要らない。
        // repeat は `count >= 0.5` の実行ガードで包む（legacy と同じく
        // 本体 0 回反復では setter を走らせない）。while/until は対象外。
        std::string bodyStartId;
        {
            const xyo::BlockInput* subIn = findInput(blk, "SUBSTACK");
            if (subIn) bodyStartId = subIn->block;
        }
        static const bool penHoist = [] {
            const char* e = std::getenv("XYO_PENHOIST");
            return !e || e[0] != '0';
        }();
        if ((forever || repeat) && penHoist && !bodyStartId.empty()) {
            static const std::unordered_set<std::string> kSetter = {
                "pen_setPenSizeTo", "pen_setPenColorToColor",
                "pen_setPenColorParamTo"};
            std::string cur = bodyStartId;
            std::vector<const xyo::Block*> hoisted;
            while (!cur.empty()) {
                const xyo::Block* sb = blocks_.getBlock(cur);
                if (!sb || !kSetter.count(sb->opcode)) break;
                // 入力は定数のみ（ループ不変の十分条件、保守側）。
                bool ok = true;
                for (const auto& [name, in] : sb->inputs.items) {
                    (void)name;
                    if (!in.block.empty()) {
                        const xyo::Block* ib = blocks_.getBlock(in.block);
                        if (ib && ib->opcode != "math_number" &&
                            ib->opcode != "text" &&
                            ib->opcode != "colour_picker" &&
                            ib->opcode != "math_whole_number" &&
                            ib->opcode != "math_positive_number" &&
                            ib->opcode != "math_integer" &&
                            ib->opcode != "math_angle") {
                            ok = false;
                        }
                    }
                    if (!in.shadow.empty() && in.block.empty()) {
                        const xyo::Block* ib2 = blocks_.getBlock(in.shadow);
                        if (ib2 && ib2->opcode != "math_number" &&
                            ib2->opcode != "text" &&
                            ib2->opcode != "colour_picker") {
                            ok = false;
                        }
                    }
                }
                if (!ok) break;
                hoisted.push_back(sb);
                cur = sb->next;
            }
            if (!hoisted.empty()) {
                if (repeat) {
                    // 実行ガード: 本体は TIMES>=0.5 のときだけ走るので、
                    // ホイストした setter も同条件の if の中へ置く
                    // （legacy codegen の "n >= 0.5" ガードと同じ）。
                    Op* num = b.create("xyo.to_num", {count},
                                       {Type::f64()});
                    Op* half = b.create("xyo.const.f64", {}, {Type::f64()},
                                        {{"d", Attr::real(0.5)}});
                    Op* ran = b.create(
                        "xyo.cmp.f",
                        {num->result(0), half->result(0)},
                        {Type::boolean()},
                        {{"pred", Attr::integer(3)}});  // FCMP_OGE
                    Op* ifOp = b.create("xyo.if", {ran->result(0)}, {});
                    Region* tr = ifOp->addRegion();
                    Block* tb = tr->addBlock();
                    Builder hb(m_);
                    hb.setBlock(tb);
                    for (const xyo::Block* sb : hoisted)
                        if (!emitStatement(hb, *sb)) return false;
                    hb.createTerminator("xyo.yield", {});
                    Region* er = ifOp->addRegion();
                    Block* eb = er->addBlock();
                    Builder eb2(m_);
                    eb2.setBlock(eb);
                    eb2.createTerminator("xyo.yield", {});
                } else {
                    for (const xyo::Block* sb : hoisted)
                        if (!emitStatement(b, *sb)) return false;
                }
                // hoist で本体が空になった（cur が鎖の末尾）場合は
                // bodyStartId を残しておき、後段の emitSubstack 経路へ
                // 落とさない（SUBSTACK 全体を再 emit して二重実行になる
                // のを防ぐ）ため終端だけを入れるフラグとして扱う。
                bodyStartId = cur;  // 本体は hoist した分の続きから
            }
        }
        const bool hoistedWholeBody = [&] {
            const xyo::BlockInput* subIn = findInput(blk, "SUBSTACK");
            return subIn && !subIn->block.empty() && bodyStartId.empty();
        }();
        // 通常のループ op を指定ビルダへ emit する（条件付きループの
        // matpow が失敗したときの else 分岐でも同じ形を使う）。
        const std::vector<Value> loopArgs = operands;
        const AttrMap loopAttrs = attrs;
        auto emitLoopOp = [&](Builder& nb0) -> bool {
            Op* loop =
                nb0.create(repeat ? "xyo.repeat"
                                  : forever ? "xyo.forever" : "xyo.while",
                           loopArgs, {}, loopAttrs);
            if (!repeat && !forever) {
                Region* beforeR = loop->addRegion();
                Block* before = beforeR->addBlock();
                Builder nb(m_);
                nb.setBlock(before);
                Value cond = emitCondition(nb, blk, "CONDITION");
                if (failed()) return false;
                if (until)
                    cond = nb.create("xyo.bnot", {cond}, {Type::boolean()})
                               ->result(0);
                nb.createTerminator("xyo.condition", {cond});
            }
            Region* bodyR = loop->addRegion();
            Block* body = bodyR->addBlock();
            Builder nb2(m_);
            nb2.setBlock(body);
            if (hoistedWholeBody) {
                // 全本体をホイストした: 空本体のまま終端へ（二重 emit 防止）。
            } else if (!bodyStartId.empty()) {
                const bool savedTerm2 = terminated_;
                terminated_ = false;
                ++branchDepth_;
                const bool ok = emitChain(nb2, bodyStartId, 0);
                --branchDepth_;
                terminated_ = savedTerm2;
                if (!ok) return false;
            } else if (!emitSubstack(nb2, blk, "SUBSTACK")) {
                return false;
            }
            if (inWarp_)
                nb2.createTerminator("xyo.loop_yield", {},
                                     {{"warp", Attr::integer(1)}});
            else
                nb2.createTerminator("xyo.loop_yield", {});
            nb2.createTerminator("xyo.yield", {});
            return true;
        };

        // 条件付きループ（repeat until / while）が単調な affine 比較なら、
        // 反復回数を二分探索して行列累乗 O(log N) へ置き換える。
        static const bool matPowCondOn = [] {
            const char* e = std::getenv("XYO_MATPOW_MLIR_COND");
            return !e || e[0] != '0';
        }();
        if (!repeat && !forever && inWarp_ && matPowCondOn &&
            !suppressMatPowCond_) {
            static const bool matPowOn = [] {
                const char* e = std::getenv("XYO_MATPOW");
                return !e || e[0] != '0';
            }();
            std::string mpWhy;
            auto mpOpt = matPowOn
                ? jit::analyzeMatPow(blocks_, &blk, &mpWhy)
                : std::optional<jit::MatPowPlan>{};
            if (mpOpt && mpOpt->condKind != 0) {
                jit::MatPowPlan& plan = *mpOpt;
                std::string varsAttr;
                for (const xyo::BlockField* vf : plan.vars) {
                    if (!varsAttr.empty()) varsAttr.push_back('\x1e');
                    varsAttr += vf->id;
                    varsAttr.push_back('\x1f');
                    varsAttr += fieldName(*vf);
                }
                const int64_t dim = int64_t(plan.vars.size()) + 1;
                auto hex64 = [](uint64_t v) {
                    char buf[17];
                    std::snprintf(buf, sizeof buf, "%016llx",
                                  (unsigned long long)v);
                    return std::string(buf);
                };
                std::string matAttr;
                matAttr.reserve(plan.mat.size() * 16);
                for (int64_t m : plan.mat) matAttr += hex64(uint64_t(m));
                std::string postAttr;
                postAttr.reserve(plan.postSrc.size() * 8);
                for (int32_t ps : plan.postSrc) {
                    char buf[9];
                    std::snprintf(buf, sizeof buf, "%08x", unsigned(ps));
                    postAttr += buf;
                }
                std::string writAttr;
                writAttr.reserve(plan.written.size());
                for (char wv : plan.written)
                    writAttr.push_back(wv ? '1' : '0');
                std::string diffAttr;
                diffAttr.reserve(plan.diff.size() * 16);
                for (int64_t dv : plan.diff) diffAttr += hex64(uint64_t(dv));
                Op* tryOp = b.create(
                    "xyo.matpow.try", {}, {Type::boolean()},
                    {{"vars", Attr::str(std::move(varsAttr))},
                     {"mat", Attr::str(std::move(matAttr))},
                     {"post", Attr::str(std::move(postAttr))},
                     {"writ", Attr::str(std::move(writAttr))},
                     {"diff", Attr::str(std::move(diffAttr))},
                     {"cond", Attr::integer(plan.condKind)},
                     {"cmp", Attr::integer(plan.cmpKind)},
                     {"flags", Attr::integer(plan.needsNonnegState ? 1 : 0)},
                     {"dim", Attr::integer(dim)},
                     {"bound", Attr::integer(plan.stmtBound)}});
                Op* ifOp = b.create("xyo.if", {tryOp->result(0)}, {});
                Region* tr = ifOp->addRegion();
                {
                    Block* tb = tr->addBlock();
                    Builder hb(m_);
                    hb.setBlock(tb);
                    hb.createTerminator("xyo.yield", {});
                }
                Region* er = ifOp->addRegion();
                {
                    Block* eb = er->addBlock();
                    Builder ebb(m_);
                    ebb.setBlock(eb);
                    // 元のループを「変換しない」状態で再 emit する。
                    // 通常の文 emission をそのまま通すので、条件評価・本体・
                    // 終端の組み立てが本経路と完全に一致する。
                    const bool savedSuppress = suppressMatPowCond_;
                    suppressMatPowCond_ = true;
                    const bool okEmit = emitStatement(ebb, blk);
                    suppressMatPowCond_ = savedSuppress;
                    if (!okEmit) return false;
                    ebb.createTerminator("xyo.yield", {});
                }
                return true;
            }
        }
        if (!emitLoopOp(b)) return false;
        return true;
    }
    //--- run without screen refresh: 本体だけ isWarp=1 ---
    if (op == "control_all_at_once") {
        Op* all = b.create("xyo.all_at_once", {}, {});
        Region* r = all->addRegion();
        Block* body = r->addBlock();
        Builder nb(m_);
        nb.setBlock(body);
        const bool savedWarp = inWarp_;
        inWarp_ = true;
        if (!emitSubstack(nb, blk, "SUBSTACK")) {
            inWarp_ = savedWarp;
            return false;
        }
        inWarp_ = savedWarp;
        return true;
    }
    //--- for each ---
    if (op == "control_for_each") {
        const xyo::BlockField* f = findField(blk, "VARIABLE", "VARIABLE");
        if (!f) return false;
        Value count = emitInputValue(b, blk, "VALUE");
        if (failed()) return false;
        Op* loop = b.create("xyo.for_each", {count}, {},
                            {{"id", Attr::str(f->id)},
                             {"name", Attr::str(fieldName(*f))}});
        Region* r = loop->addRegion();
        Block* body = r->addBlock();
        Builder nb(m_);
        nb.setBlock(body);
        if (!emitSubstack(nb, blk, "SUBSTACK")) return false;
        if (inWarp_)
            nb.createTerminator("xyo.loop_yield", {},
                                {{"warp", Attr::integer(1)}});
        else
            nb.createTerminator("xyo.loop_yield", {});
        nb.createTerminator("xyo.yield", {});
        return true;
    }
    //--- stop（スクリプトを終わらせるものは return まで持っていく）---
    if (op == "control_stop") {
        const xyo::BlockField* f = findField(blk, "STOP_OPTION", "STOP_OPTION");
        if (!f) return false;
        const std::string opt = fieldName(*f);
        // 手続きの中の stop all / stop this script は、呼び出し元まで
        // unwind する必要がある（レガシー codegen は戻り直後に
        // c->stopped を見る）。MLIR 経路はその契約を実装していないので
        // 安全側に倒してスクリプト全体を従来経路へ回す
        // （`general_stop_all_in_proc` の SIGSEGV 対策）。
        if (current_ && (opt == "all" || opt == "this script"))
            return fail("stop inside procedure");
        if (opt == "all" || opt == "this script") {
            b.create("xyo.stop", {}, {},
                     {{"kind", Attr::str(opt == "all" ? "all" : "script")}});
            terminated_ = true;
            return true;
        }
        if (opt == "other scripts in sprite" ||
            opt == "other scripts in stage") {
            return emitCompatStatement(b, blk);
        }
        return fail("unknown stop option: " + opt);
    }
    if (op == "control_delete_this_clone") {
        // 手続き内の clone 削除も同様に unwind が要る。
        if (current_) return fail("delete clone inside procedure");
        b.create("xyo.stop", {}, {}, {{"kind", Attr::str("clone")}});
        return true;
    }
    //--- wait until（既存 JIT と同じ event 依存待機） ---
    if (op == "procedures_return") {
        // reporter 手続きの戻り値。値は入力 VALUE。
        Value v = emitInputValue(b, blk, "VALUE");
        if (failed()) return false;
        b.create("xyo.procret", {v}, {});
        terminated_ = true;
        return true;
    }
    if (op == "control_wait_until") {        const bool event = blocks_.isPureWaitCondition(&blk);
        Op* w = b.create("xyo.wait_until", {}, {},
                         {{"site", Attr::integer(static_cast<int64_t>(
                              reinterpret_cast<uintptr_t>(&blk)))},
                          {"event", Attr::integer(event ? 1 : 0)}});
        Region* r = w->addRegion();
        Block* condB = r->addBlock();
        Builder nb(m_);
        nb.setBlock(condB);
        Value cond = emitCondition(nb, blk, "CONDITION");
        if (failed()) return false;
        nb.createTerminator("xyo.condition", {cond});
        return true;
    }
    // broadcast and wait: compat 直呼びでは待機プロトコルを合成できず
    // ハングするため、専用 op にして lowering で
    // begin → (step が 0 になるまで yield) を展開する。
    if (op == "event_broadcastandwait") {
        Value v = emitInputValue(b, blk, "BROADCAST_INPUT");
        if (failed()) return false;
        b.create("xyo.bcastwait", {v}, {});
        return true;
    }
    // wait: ランタイムの waitBegin/poll + 通常 yield で組み立てる
    // （compat のプリミティブ実行と同じ状態遷移。legacy codegen と同じ形）。
    if (op == "control_wait") {
        Value secs = emitInputValue(b, blk, "DURATION");
        if (failed()) return false;
        b.create("xyo.wait", {secs}, {});
        return true;
    }
    // 変数/リストモニタの表示切替（legacy の xyo_var_monitor と同じ）。
    if (op == "data_showvariable" || op == "data_hidevariable" ||
        op == "data_showlist" || op == "data_hidelist") {
        const bool isList =
            op == "data_showlist" || op == "data_hidelist";
        const xyo::BlockField* f =
            findField(blk, isList ? "LIST" : "VARIABLE",
                      isList ? "LIST" : "VARIABLE");
        if (!f) return false;
        const bool vis =
            op == "data_showvariable" || op == "data_showlist";
        b.create("xyo.monitor", {}, {},
                 {{"id", Attr::str(f->id)},
                  {"vis", Attr::integer(vis ? 1 : 0)}});
        return true;
    }
    if (emitDirect(b, blk)) return !failed();
    return emitCompatStatement(b, blk);
}

bool ScriptBuilder::emitPendingProcedures() {
    while (!pending_.empty()) {
        const std::string vkey = pending_.front();
        pending_.pop_front();
        auto it = procs_.find(vkey);
        if (it == procs_.end() || it->second.emitted) continue;
        ProcSig& sig = it->second;
        sig.emitted = true;
        current_ = &sig;
        inWarp_ = sig.warp;
        branchDepth_ = 0;
        argIndex_.clear();
        for (size_t i = 0; i < sig.names.size(); ++i)
            argIndex_[sig.names[i]] = i;
        Builder nb(m_);
        nb.setBlock(sig.fn->body);
        terminated_ = false;
        if (!emitChain(nb, sig.def->next)) return false;
        nb.createTerminator("func.return", {});
        current_ = nullptr;
        inWarp_ = false;
        argIndex_.clear();
    }
    return true;
}

bool ScriptBuilder::build(const std::string& topId) {
    const xyo::Block* top = blocks_.getBlock(topId);
    if (!top) return fail("missing top block");
    if (!isStartHat(top->opcode))
        return fail("unsupported hat " + top->opcode);
    // ハット自体は実行しない（入力の評価は spawn 側の仕事）ので、入力を
    // 持つハット（when greater than 等）もそのままコンパイルできる。
    if (top->next.empty()) return fail("empty script");

    auto* fn = m_.createFunction(entryName_, {Type::ctx()});
    Builder b(m_);
    b.setBlock(fn->body);
    terminated_ = false;
    if (!emitChain(b, top->next)) return false;
    b.createTerminator("func.return", {});
    if (!emitPendingProcedures()) return false;
    return true;
}

/// 事前スキャン: 到達可能な全ブロックの opcode を検査し、emit 途中で
/// LLVM モジュールに中途半端な関数を残さないようにする。
bool preScan(const Blocks& blocks, const std::string& firstId,
             std::string* reason, bool lenient) {
    // lenient（XYO_MLIR_NOFALLBACK=1）では未対応 opcode を compat 層で実行
    // するので「compat では扱えないもの」だけを弾く。既定は従来どおり
    // allowlist 方式（1 つでも外れたらスクリプト全体を既存 JIT に任せる）。
    // 全 opcode が compat か native で実行できる（reporter 手続きも native）。
    static const std::unordered_set<std::string> kDenied = {};
    static const std::unordered_set<std::string> kAllowed = {
        "data_setvariableto",  "data_changevariableby", "data_addtolist",
        "data_deleteoflist",   "data_deletealloflist", "data_insertatlist",
        "data_replaceitemoflist", "control_if",        "control_if_else",
        "control_repeat",      "control_forever",      "control_repeat_until",
        "control_while",       "math_number",          "math_integer",
        "math_whole_number",   "math_positive_number", "math_angle",
        "data_variable",       "operator_add",         "operator_subtract",
        "operator_multiply",   "operator_divide",      "operator_mod",
        "operator_lt",         "operator_gt",          "operator_equals",
        "operator_and",        "operator_or",          "operator_not",
        "operator_round",      "operator_mathop",      "data_itemoflist",
        "data_itemnumoflist",  "data_listlength",      "data_lengthoflist",
        "data_listcontainsitem", "text",               "operator_join",
        "operator_letter_of",  "operator_length",      "operator_contains",
        "procedures_call",     "procedures_return",
        "argument_reporter_string_number",
        "argument_reporter_boolean", "argument_reporter",
        // no-fallback 期に native 化したもの（既定でも有効）。
        "control_wait_until",  "control_all_at_once",  "control_for_each",
        "control_stop",        "control_delete_this_clone",
        "sensing_dayssince2000", "sensing_timer",
        "math_boolean",        "colour_picker",        "event_broadcast",
        "event_broadcastandwait", "event_broadcast_menu",
    };
    std::unordered_set<std::string> seen;
    std::vector<std::string> stack{firstId};
    while (!stack.empty()) {
        std::string id = std::move(stack.back());
        stack.pop_back();
        if (id.empty()) continue;
        if (!seen.insert(id).second) continue;  // 合流は許す（cycle は emit 側）
        const xyo::Block* blk = blocks.getBlock(id);
        if (!blk) {
            *reason = "missing block " + id;
            return false;
        }
        static const std::unordered_set<std::string> extra = [] {
            std::unordered_set<std::string> out;
            const char* e = std::getenv("XYO_MLIR_EXTRA_ALLOW");
            if (!e) return out;
            std::string all(e);
            size_t pos = 0;
            while (pos <= all.size()) {
                size_t comma = all.find(',', pos);
                std::string op = all.substr(
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (!op.empty()) out.insert(op);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            return out;
        }();
        const bool ok = lenient ? kDenied.count(blk->opcode) == 0
                                : (kAllowed.count(blk->opcode) != 0 ||
                                   extra.count(blk->opcode) != 0);
        if (!ok) {
            *reason = "unsupported opcode " + blk->opcode;
            return false;
        }
        if (!blk->next.empty()) stack.push_back(blk->next);
        for (const auto& [name, in] : blk->inputs.items) {
            (void)name;
            if (!in.block.empty()) stack.push_back(in.block);
            if (!in.shadow.empty()) stack.push_back(in.shadow);
        }
    }
    return true;
}

} // namespace

BuildOutcome buildScriptFromBlocks(Module& module, Blocks& blocks,
                                   const std::string& topId,
                                   const std::string& entryName,
                                   const StringPinner& pin,
                                   std::map<std::string, std::string>* procNames,
                                   const BuildOptions* options) {
    BuildOutcome out;
    std::string reason;
    const xyo::Block* top = blocks.getBlock(topId);
    // 既定で no-fallback（未対応ブロックは compat 層で実行）。=0 で従来の
    // allowlist 方式（1 つでも外れたらスクリプト全体を既存 JIT に任せる）。
    static const bool lenient = [] {
        const char* e = std::getenv("XYO_MLIR_NOFALLBACK");
        return !e || e[0] != '0';
    }();
    if (top && !top->next.empty() &&
        !preScan(blocks, top->next, &reason, lenient)) {
        out.reason = reason;
        return out;
    }
    ScriptBuilder sb(module, blocks, pin, entryName, lenient);
    if (options && options->coroMode) sb.setCoroMode(true);
    if (!sb.build(topId)) {
        out.reason = sb.error();
        return out;
    }
    out.ok = true;
    out.hasLoop = sb.hasLoop();
    out.internalYield = sb.usedInternalYield();
    if (procNames) *procNames = sb.procNames();
    return out;
}

} // namespace xyo::mlir
