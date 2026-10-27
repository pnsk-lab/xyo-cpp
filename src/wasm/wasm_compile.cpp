// xyo-cpp: スクリプト → wasm コンパイラの実装（Phase C）。
// 仕様は wasm_compile.hpp のコメントを参照。
#include "wasm_compile.hpp"

#include "core/cast.hpp"
#include "core/value.hpp"
#include "model/blocks.hpp"

#include <map>
#include "wasm_emit.hpp"

namespace xyo::wasm {

namespace {

using xyo::val::kNan;

// ローカル配置:
//   0 = slots (i32, 引数)
//   1 = bits  (i64 スクラッチ)
//   2 = num   (f64 スクラッチ)
//   3 = tmp   (f64 スクラッチ)
constexpr uint32_t kBits = 1, kNum = 2, kTmp = 3, kFirstFree = 4;

class Gen {
public:
    Gen(const Program& p, Code& c) : p_(p), c_(c) {}

    void run() {
        emitStmts(p_.main);
        c_.i32Const(0);   // 完走
    }

    uint32_t nextLocal(ValType t) {
        c_.local(t);
        return next_++;
    }

private:
    void bail() {
        c_.i32Const(1);
        c_.return_();
    }

    /// f64 を 1 つ消費し、NaN を 0 に落として 1 つ残す。
    void nanToZero() {
        c_.localSet(kTmp);                                  // x
        c_.localGet(kTmp);                                  // v1 = x
        c_.f64Const(0.0);                                   // v2 = 0
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Eq();                                         // cond = (x==x)
        c_.select();                                        // x か 0
    }

    /// f64 を 1 つ消費して slots[var] へ格納（NaN は kNan へ正規化）。
    void storeF64(uint32_t var) {
        const uint32_t off = var * 8;
        c_.localSet(kTmp);
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Ne();                                         // isnan
        c_.if_();
        c_.i64Const(int64_t(kNan));
        c_.localSet(kBits);
        c_.else_();
        c_.localGet(kTmp);
        c_.i64ReinterpretF64();
        c_.localSet(kBits);
        c_.end();
        c_.localGet(0);
        c_.localGet(kBits);
        c_.i64Store(3, off);
    }

    void emitExpr(uint32_t idx) {
        const Expr& e = p_.exprs[idx];
        switch (e.k) {
        case EK::ConstNum:
            c_.f64Const(e.num);
            return;
        case EK::VarGet: {
            const uint32_t off = e.var * 8;
            c_.localGet(0);
            c_.i64Load(3, off);
            c_.localSet(kBits);
            // isNumber() = ((bits>>48)&0xFFF8) != 0x7FF8 || bits == kNan
            c_.localGet(kBits);
            c_.i64Const(48);
            c_.i64ShrU();
            c_.i64Const(0xFFF8);
            c_.i64And();
            c_.i64Const(0x7FF8);
            c_.i64Eq();                                     // タグ付きか
            c_.if_();
            c_.localGet(kBits);
            c_.i64Const(int64_t(kNan));
            c_.i64Eq();                                     // NaN は数値
            c_.i32Eqz();
            c_.if_();
            bail();                                         // 文字列等 → bail
            c_.end();
            c_.end();
            c_.localGet(kBits);
            c_.f64ReinterpretI64();
            return;
        }
        case EK::Add:
        case EK::Sub:
        case EK::Mul:
        case EK::Div: {
            emitExpr(e.a);
            nanToZero();
            emitExpr(e.b);
            nanToZero();
            if (e.k == EK::Add) c_.f64Add();
            else if (e.k == EK::Sub) c_.f64Sub();
            else if (e.k == EK::Mul) c_.f64Mul();
            else c_.f64Div();
            return;
        }
        case EK::Mod: {
            // Scratch: r = n % m; if (r / m < 0) r += m;（JS の % そのもの
            // ではない。負の剰余を m 側へ寄せる）。
            const uint32_t la = nextLocal(ValType::F64),
                           lb = nextLocal(ValType::F64),
                           lt = nextLocal(ValType::F64);
            emitExpr(e.a);
            nanToZero();
            emitExpr(e.b);
            nanToZero();
            c_.localSet(lb);
            c_.localSet(la);
            c_.localGet(la);
            c_.localGet(lb);
            c_.f64Div();
            c_.f64Trunc();
            c_.localGet(lb);
            c_.f64Mul();
            c_.localSet(lt);
            // lt = a - trunc(a/b)*b  (= JS %)
            c_.localGet(la);
            c_.localGet(lt);
            c_.f64Sub();
            c_.localSet(lt);
            // select(lt/lb < 0, lt+lb, lt)
            c_.localGet(lt);
            c_.localGet(lb);
            c_.f64Add();
            c_.localGet(lt);
            c_.localGet(lt);
            c_.localGet(lb);
            c_.f64Div();
            c_.f64Const(0.0);
            c_.f64Lt();
            c_.select();
            return;
        }
        case EK::Neg:
            c_.f64Const(0.0);
            emitExpr(e.a);
            c_.f64Sub();
            return;
        case EK::Round:
            // JS Math.round（.5 は +Infinity 方向、-0 を保持）。
            emitExpr(e.a);
            emitJsRoundF64();
            return;
        }
    }

    /// スタック上の f64 を JS Math.round し、f64 を 1 つ残す。
    /// floor(x+0.5) だけでは 0.49999999999999994 が 1 になり、-0 も失う。
    void emitJsRoundF64() {
        const uint32_t a = nextLocal(ValType::F64),
                       r = nextLocal(ValType::F64);
        c_.localSet(a);
        c_.localGet(a);
        c_.f64Const(0.5);
        c_.f64Add();
        c_.f64Floor();
        c_.localSet(r);
        // |a| >= 2^52 -> a
        c_.localGet(a);
        c_.localGet(r);
        c_.localGet(a);
        c_.f64Const(4503599627370496.0);
        c_.f64Ge();
        c_.localGet(a);
        c_.f64Const(-4503599627370496.0);
        c_.f64Le();
        c_.i32Or();
        c_.select();
        c_.localSet(r);
        // 0 <= a < 0.5 -> +0
        c_.f64Const(0.0);
        c_.localGet(r);
        c_.localGet(a);
        c_.i64ReinterpretF64();
        c_.i64Const(0);
        c_.i64LtS();
        c_.i32Eqz();
        c_.localGet(a);
        c_.f64Const(0.5);
        c_.f64Lt();
        c_.i32And();
        c_.select();
        c_.localSet(r);
        // -0.5 <= a < 0 -> -0
        c_.f64Const(-0.0);
        c_.localGet(r);
        c_.localGet(a);
        c_.i64ReinterpretF64();
        c_.i64Const(0);
        c_.i64LtS();
        c_.localGet(a);
        c_.f64Const(-0.5);
        c_.f64Ge();
        c_.i32And();
        c_.select();
        c_.localSet(r);
        c_.localGet(r);
    }

    void emitStmt(uint32_t idx) {
        const Stmt& s = p_.stmts[idx];
        switch (s.k) {
        case SK::SetVar:
            emitExpr(s.expr);
            storeF64(s.var);
            return;
        case SK::ChangeVar: {
            c_.localGet(0);
            c_.i64Load(3, s.var * 8);
            c_.localSet(kBits);
            c_.localGet(kBits);
            c_.i64Const(48);
            c_.i64ShrU();
            c_.i64Const(0xFFF8);
            c_.i64And();
            c_.i64Const(0x7FF8);
            c_.i64Eq();
            c_.if_();
            c_.localGet(kBits);
            c_.i64Const(int64_t(kNan));
            c_.i64Eq();
            c_.i32Eqz();
            c_.if_();
            bail();
            c_.end();
            c_.end();
            c_.localGet(kBits);
            c_.f64ReinterpretI64();
            nanToZero();
            emitExpr(s.expr);
            nanToZero();
            c_.f64Add();
            storeF64(s.var);
            return;
        }
        case SK::Repeat: {
            emitExpr(s.expr);
            nanToZero();
            c_.localSet(kTmp);
            c_.localGet(kTmp);
            c_.f64Const(2147483647.0);
            c_.f64Gt();
            c_.if_();
            bail();
            c_.end();
            // d = Math.round(x)
            c_.localGet(kTmp);
            emitJsRoundF64();
            c_.localSet(kTmp);
            const uint32_t ln = nextLocal(ValType::I32),
                           li = nextLocal(ValType::I32);
            c_.localGet(kTmp);
            c_.f64Const(0.0);
            c_.f64Gt();
            c_.if_();
            c_.localGet(kTmp);
            c_.i32TruncF64S();
            c_.localSet(ln);
            c_.else_();
            c_.i32Const(0);
            c_.localSet(ln);
            c_.end();
            c_.i32Const(0);
            c_.localSet(li);
            c_.block();
            c_.loop();
            c_.localGet(li);
            c_.localGet(ln);
            c_.i32GeS();
            c_.brIf(1);
            emitStmts(s.body);
            c_.localGet(li);
            c_.i32Const(1);
            c_.i32Add();
            c_.localSet(li);
            c_.br(0);
            c_.end();
            c_.end();
            return;
        }
        }
    }

    void emitStmts(const std::vector<uint32_t>& list) {
        for (uint32_t id : list) emitStmt(id);
    }

    const Program& p_;
    Code& c_;
    uint32_t next_ = kFirstFree;
};

} // namespace

std::vector<uint8_t> compileProgram(const Program& p) {
    Module m;
    const uint32_t t = m.type({ValType::I32}, {ValType::I32});
    m.importMemory("env", "memory", 256);
    const uint32_t f = m.addFunc(t);
    m.exportFunc("f", f);
    Code& c = m.code(f);
    c.local(ValType::I64);   // 1
    c.local(ValType::F64);   // 2
    c.local(ValType::F64);   // 3

    Gen g(p, c);
    g.run();
    c.end();
    return m.finish();
}

// --- ブロック木 → Program ----------------------------------------------

namespace {

bool isNumShadow(const std::string& op) {
    return op == "math_number" || op == "math_integer" ||
           op == "math_whole_number" || op == "math_positive_number" ||
           op == "math_angle";
}

struct Lower {
    Blocks& b;
    Program& p;
    std::map<std::string, uint32_t> vars;

    uint32_t slotFor(const std::string& id) {
        auto it = vars.find(id);
        if (it != vars.end()) return it->second;
        const uint32_t s = uint32_t(vars.size());
        vars.emplace(id, s);
        if (s + 1 > p.nSlots) p.nSlots = s + 1;
        return s;
    }

    const Block* inputOf(const Block* blk, const char* name) {
        const BlockInput* in = blk->inputs.find(name);
        if (!in) return nullptr;
        const std::string& id = !in->block.empty() ? in->block : in->shadow;
        return id.empty() ? nullptr : b.getBlock(id);
    }

    bool expr(const Block* blk, uint32_t& out) {
        if (!blk) return false;
        const std::string& op = blk->opcode;
        if (isNumShadow(op)) {
            auto it = blk->fields.find("NUM");
            if (it == blk->fields.end()) return false;
            out = p.exprNum(cast::toNumber(it->second.value));
            return true;
        }
        if (op == "data_variable") {
            auto it = blk->fields.find("VARIABLE");
            if (it == blk->fields.end() || it->second.id.empty()) return false;
            out = p.exprVar(slotFor(it->second.id));
            return true;
        }
        if (op == "operator_round") {
            uint32_t a;
            if (!expr(inputOf(blk, "NUM"), a)) return false;
            out = p.exprUn(EK::Round, a);
            return true;
        }
        EK k;
        if (op == "operator_add") k = EK::Add;
        else if (op == "operator_subtract") k = EK::Sub;
        else if (op == "operator_multiply") k = EK::Mul;
        else if (op == "operator_divide") k = EK::Div;
        else if (op == "operator_mod") k = EK::Mod;
        else return false;
        uint32_t a, bb;
        if (!expr(inputOf(blk, "NUM1"), a)) return false;
        if (!expr(inputOf(blk, "NUM2"), bb)) return false;
        out = p.exprBin(k, a, bb);
        return true;
    }

    bool setVar(const Block* blk, SK kind, uint32_t& out) {
        auto it = blk->fields.find("VARIABLE");
        if (it == blk->fields.end() || it->second.id.empty()) return false;
        uint32_t e;
        if (!expr(inputOf(blk, "VALUE"), e)) return false;
        Stmt s;
        s.k = kind;
        s.var = slotFor(it->second.id);
        s.expr = e;
        out = p.stmt(std::move(s));
        return true;
    }

    bool stmt(const Block* blk, uint32_t& out) {
        if (!blk) return false;
        if (blk->opcode == "data_setvariableto")
            return setVar(blk, SK::SetVar, out);
        if (blk->opcode == "data_changevariableby")
            return setVar(blk, SK::ChangeVar, out);
        if (blk->opcode == "control_repeat") {
            uint32_t e;
            if (!expr(inputOf(blk, "TIMES"), e)) return false;
            Stmt s;
            s.k = SK::Repeat;
            s.expr = e;
            const BlockInput* sub = blk->inputs.find("SUBSTACK");
            if (sub && !sub->block.empty() && !chain(sub->block, s.body))
                return false;
            out = p.stmt(std::move(s));
            return true;
        }
        return false;   // 非対応 → スクリプト全体を interp へ
    }

    bool chain(const std::string& id, std::vector<uint32_t>& out) {
        std::string cur = id;
        while (!cur.empty()) {
            const Block* blk = b.getBlock(cur);
            if (!blk) return false;
            uint32_t s;
            if (!stmt(blk, s)) return false;
            out.push_back(s);
            cur = blk->next;
        }
        return true;
    }
};

} // namespace

bool lowerScript(::xyo::Blocks& blocks, const ::xyo::Block* top,
                 Program& out) {
    if (!top) return false;
    Lower l{blocks, out};
    if (!l.chain(top->next, out.main)) return false;
    return true;
}

} // namespace xyo::wasm
