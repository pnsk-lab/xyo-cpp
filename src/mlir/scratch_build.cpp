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
            return b.cst(pin_(std::u16string(str->view())));
        }
        return b.cst(xyo::Value::undefined().bits);
    }

    /// 数値リテラル shadow の値。project.json の NUM は文字列なので、
    /// そのまま const にすると算術のたびに文字列→数値変換が要る
    /// （Scratch もブロック値は Cast.toNumber してから使う）。数値に
    /// ならない文字列はそのまま残す。
    Value numericLiteralValue(Builder& b, const xyo::Value& v) {
        if (v.isNumber())
            return b.cst(xyo::Value::numCanonical(v.asNumberUnchecked()).bits);
        if (v.isString()) {
            const double d = cast::toNumberOrNaN(v);
            if (!std::isnan(d)) return b.cstNum(d);
        }
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
    bool emitSubstack(Builder& b, const xyo::Block& blk, const char* name) {
        const xyo::BlockInput* sub = findInput(blk, name);
        if (sub && !sub->block.empty()) return emitChain(b, sub->block);
        return true;
    }

    /// false: 未定義手続き（Scratch の no-op 呼出し）。fail() 済みなら
    /// failed() が真になる。
    bool resolveProc(const std::string& proccode, ProcSig** out);
    bool emitCall(Builder& b, const xyo::Block& blk, Value* out = nullptr);
    /// 未対応 opcode を compat 層（プリミティブ直呼び）で実行する。
    bool emitCompatStatement(Builder& b, const xyo::Block& blk);
    Value emitCompatValue(Builder& b, const xyo::Block& blk);
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
            fail("unknown procedure argument: " +
                 SStr::toUtf8(f->value.asStrPtr()->view()));
            return {};
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
    if (op == "data_itemoflist" || op == "data_itemnumoflist" ||
        op == "data_listlength" || op == "data_listcontainsitem" ||
        op == "data_lengthoflist") {
        const xyo::BlockField* f = findField(blk, "LIST", "LIST");
        if (!f) return {};
        Value list = listPtr(b, *f);
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
    const bool wantValue = out != nullptr && sig->reporter;
    Op* callOp = wantValue
                     ? b.create("xyo.call", std::move(args), {Type::value()},
                                std::move(callAttrs))
                     : b.create("xyo.call", std::move(args), {},
                                std::move(callAttrs));
    if (wantValue) *out = callOp->result(0);
    return true;
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
        // repeat/while は 0 回反復で setter が走ってしまうため対象外。
        std::string bodyStartId;
        {
            const xyo::BlockInput* subIn = findInput(blk, "SUBSTACK");
            if (subIn) bodyStartId = subIn->block;
        }
        static const bool penHoist = [] {
            const char* e = std::getenv("XYO_PENHOIST");
            return !e || e[0] != '0';
        }();
        if (forever && penHoist && !bodyStartId.empty()) {
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
                for (const xyo::Block* sb : hoisted)
                    if (!emitStatement(b, *sb)) return false;
                bodyStartId = cur;  // 本体は hoist した分の続きから
            }
        }
        Op* loop = b.create(repeat ? "xyo.repeat"
                             : forever ? "xyo.forever" : "xyo.while",
                            std::move(operands), {}, std::move(attrs));
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
        if (!bodyStartId.empty()) {
            if (!emitChain(nb2, bodyStartId, 0)) return false;
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
                                   std::map<std::string, std::string>* procNames) {
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
    if (!sb.build(topId)) {
        out.reason = sb.error();
        return out;
    }
    out.ok = true;
    out.hasLoop = sb.hasLoop();
    if (procNames) *procNames = sb.procNames();
    return out;
}

} // namespace xyo::mlir
