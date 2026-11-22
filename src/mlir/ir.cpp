// xyo-cpp: MLIR 風 IR のコア実装（Module / Builder / verifier / printer）。
#include "ir.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_set>

#include "../core/cast.hpp"
#include "../core/value.hpp"

namespace xyo::mlir {

// ================================================================ 型 ====

std::string_view Type::name() const {
    switch (kind) {
    case TypeKind::Value: return "!xyo.value";
    case TypeKind::F64: return "!xyo.f64";
    case TypeKind::Bool: return "!xyo.bool";
    case TypeKind::I64: return "!xyo.i64";
    case TypeKind::I32: return "!xyo.i32";
    case TypeKind::Ptr: return "!xyo.ptr";
    case TypeKind::Ctx: return "!xyo.ctx";
    default: return "<none>";
    }
}

// ============================================================== 属性 ====

Attr Attr::integer(int64_t v) {
    Attr a;
    a.kind = Kind::Int;
    a.i = v;
    return a;
}
Attr Attr::real(double v) {
    Attr a;
    a.kind = Kind::Double;
    a.d = v;
    return a;
}
Attr Attr::str(std::string v) {
    Attr a;
    a.kind = Kind::Str;
    a.s = std::move(v);
    return a;
}
const std::string& Attr::asStr() const {
    static const std::string kEmpty;
    return kind == Kind::Str ? s : kEmpty;
}

// ============================================================ dialect ====

namespace dialect {

bool isTerminator(std::string_view name) {
    return name == "xyo.yield" || name == "xyo.condition" ||
           name == "func.return";
}

bool isPure(std::string_view name) {
    // メモリにも観測可能な状態にも触れない op。DCE/畳み込みの対象。
    static const std::unordered_set<std::string_view> kPure = {
        "xyo.const",    "xyo.const.f64", "xyo.add",   "xyo.sub",
        "xyo.mul",      "xyo.div",       "xyo.mod",   "xyo.add.f",
        "xyo.sub.f",    "xyo.mul.f",     "xyo.div.f", "xyo.mod.f",
        "xyo.round",    "xyo.round.f",   "xyo.mathop", "xyo.mathop.f",
        "xyo.lt",       "xyo.gt",        "xyo.eq",    "xyo.truthy",
        "xyo.arg",      "xyo.and",       "xyo.or",    "xyo.not",
        "xyo.select",   "xyo.cmp.f",     "xyo.to_num",
        "xyo.from_num", "xyo.join",      "xyo.letterof", "xyo.strlen",
        "xyo.strcontains", "xyo.band",   "xyo.bor",   "xyo.bnot",
        "xyo.boolval",  "xyo.num_ok",     "xyo.box.f",  "xyo.cmp.guarded",
        // 整数 SSA（xyo.const.i / *.i / 変換）。副作用なし。
        "xyo.const.i",  "xyo.add.i",     "xyo.sub.i", "xyo.mul.i",
        "xyo.mod.i",    "xyo.div.i",     "xyo.cmp.i",     "xyo.from_int",
        "xyo.to_int",
        "xyo.int_exact_ok",
    };
    return kPure.count(name) != 0;
}

bool isReadOnly(std::string_view name) {
    // メモリを読むが書かない op。CSE は不可（yield で値が変わる）だが、
    // 観測可能な副作用は無い。
    return name == "xyo.getvar" || name == "xyo.list.ptr";
}

bool isRegionOp(std::string_view name) {
    return name == "xyo.if" || name == "xyo.while" || name == "xyo.repeat" ||
           name == "xyo.forever" || name == "func.func";
}

size_t regionCount(std::string_view name) {
    if (name == "xyo.if" || name == "xyo.while") return 2;
    if (name == "xyo.repeat" || name == "xyo.forever" ||
        name == "xyo.all_at_once" || name == "xyo.for_each" ||
        name == "xyo.wait_until" ||
        name == "func.func")
        return 1;
    return 0;
}

} // namespace dialect

bool intLiteralBits(uint64_t bits, int64_t* out) {
    const xyo::Value v{bits};
    if (v.isString()) {
        // 空文字列は asStrPtr() が null を返す。数値 0 だが文字列読みが
        // 変わる (""/"0") ため対象外。
        const SStr* s = v.asStrPtr();
        if (!s) return false;
        double d = xyo::cast::toNumberOrNaN(v);
        if (d != d || d < -2147483648.0 || d > 2147483647.0) return false;
        if (std::trunc(d) != d) return false;
        // 正準形のみ（印字して戻るもの）。"1" は可、"1.0" は不可。
        if (xyo::jsnum::toString16(d) != std::u16string(s->view()))
            return false;
        *out = static_cast<int64_t>(d);
        return true;
    }
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    // 素の double（タグ域 < kNan）。NaN・非有限・非整数・範囲外を弾く。
    if (bits >= static_cast<uint64_t>(xyo::val::kNan)) {
        // kNan そのもの（NaN）は不可。kNan より大きいタグ値は非数値。
        return false;
    }
    if (d != d || d < -2147483648.0 || d > 2147483647.0) return false;
    if (std::trunc(d) != d) return false;
    *out = static_cast<int64_t>(d);
    return true;
}

// ============================================================== Module ====

Module::Module() = default;
Module::~Module() = default;

Op* Module::alloc() {
    ops_.push_back(std::make_unique<Op>());
    Op* op = ops_.back().get();
    op->debugId = nextDebugId_++;
    return op;
}

Op* Module::create(std::string name, std::vector<Value> operands,
                   std::vector<Type> resultTypes, AttrMap attrs) {
    Op* op = alloc();
    op->name = std::move(name);
    op->operands = std::move(operands);
    op->resultTypes = std::move(resultTypes);
    op->attrs = std::move(attrs);
    return op;
}

Region* Op::addRegion() {
    regions.push_back(std::make_unique<Region>());
    Region* r = regions.back().get();
    r->owner = this;
    return r;
}

Module::Function* Module::createFunction(std::string name,
                                         std::vector<Type> argTypes,
                                         AttrMap attrs) {
    AttrMap all = std::move(attrs);
    all["sym_name"] = Attr::str(name);
    Op* fn = create("func.func", {}, {}, std::move(all));
    Region* body = fn->addRegion();
    Block* entry = body->addBlock();
    for (size_t i = 0; i < argTypes.size(); ++i) {
        Value v;
        v.block = entry;
        v.argIndex = static_cast<uint32_t>(i);
        v.type = argTypes[i];
        entry->args.push_back(v);
    }
    Function f;
    f.op = fn;
    f.name = std::move(name);
    f.body = entry;
    f.argTypes = std::move(argTypes);
    functions_.push_back(std::move(f));
    return &functions_.back();
}

Module::Function* Module::function(std::string_view name) {
    for (auto& f : functions_)
        if (f.name == name) return &f;
    return nullptr;
}

void Module::forEachOp(const std::function<void(Op&)>& fn) {
    std::function<void(Block&)> walkBlock = [&](Block& b) {
        for (Op* op : b.ops) {
            fn(*op);
            for (auto& r : op->regions)
                for (auto& child : r->blocks) walkBlock(*child);
        }
    };
    for (auto& f : functions_) walkBlock(*f.body);
}

void Module::forEachOp(const std::function<void(Op&)>& fn) const {
    const_cast<Module*>(this)->forEachOp(fn);
}

size_t Module::opCount() const {
    size_t n = 0;
    const_cast<Module*>(this)->forEachOp([&](Op&) { ++n; });
    return n;
}

// ============================================================= Builder ====

Op* Builder::create(std::string name, std::vector<Value> operands,
                    std::vector<Type> resultTypes, AttrMap attrs) {
    Op* op = m_.create(std::move(name), std::move(operands),
                       std::move(resultTypes), std::move(attrs));
    op->parent = b_;
    if (b_) b_->ops.push_back(op);
    return op;
}

Op* Builder::createTerminator(std::string name, std::vector<Value> operands,
                              AttrMap attrs) {
    return create(std::move(name), std::move(operands), {}, std::move(attrs));
}

Value Builder::unary(std::string name, Value a, Type result, AttrMap attrs) {
    Op* op = create(std::move(name), {a}, {result}, std::move(attrs));
    return op->result(0);
}

Value Builder::binary(std::string name, Value a, Value b, Type result,
                      AttrMap attrs) {
    Op* op = create(std::move(name), {a, b}, {result}, std::move(attrs));
    return op->result(0);
}

Value Builder::cst(uint64_t bits) {
    Op* op = create("xyo.const", {}, {Type::value()},
                    {{"bits", Attr::integer(static_cast<int64_t>(bits))}});
    return op->result(0);
}

Value Builder::cstNum(double d) {
    return cst(xyo::Value::numCanonical(d).bits);
}

Value Builder::cstF64(double d) {
    Op* op = create("xyo.const.f64", {}, {Type::f64()}, {{"d", Attr::real(d)}});
    return op->result(0);
}

Value Builder::getVar(const std::string& id, const std::string& name) {
    Op* op = create("xyo.getvar", {}, {Type::value()},
                    {{"id", Attr::str(id)}, {"name", Attr::str(name)}});
    return op->result(0);
}

void Builder::setVar(const std::string& id, const std::string& name, Value v) {
    create("xyo.setvar", {v}, {},
           {{"id", Attr::str(id)}, {"name", Attr::str(name)}});
}

// =========================================================== verifier ====

namespace {

struct Verifier {
    std::vector<Diagnostic>* errors;
    uint64_t count = 0;

    void fail(Op* op, std::string msg) {
        ++count;
        if (errors) errors->push_back({std::move(msg), op});
    }

    static bool contains(const Block* ancestor, const Block* block) {
        const Block* b = block;
        while (b) {
            if (b == ancestor) return true;
            if (!b->parent) return false;
            Op* owner = b->parent->owner;
            b = owner ? owner->parent : nullptr;
        }
        return false;
    }

    static std::optional<size_t> opIndex(const Block* b, const Op* op) {
        for (size_t i = 0; i < b->ops.size(); ++i)
            if (b->ops[i] == op) return i;
        return std::nullopt;
    }

    bool visible(const Value& v, const Block* cur, size_t curIndex,
                 Op* user) {
        if (!v.valid()) {
            fail(user, "operand is empty for " + user->name);
            return false;
        }
        if (!v.type.valid()) {
            fail(user, "operand has no type");
            return false;
        }
        if (v.isBlockArg()) {
            if (!contains(v.block, cur)) {
                fail(user, "block argument used outside its region");
                return false;
            }
            return true;
        }
        if (!v.op->parent) {
            fail(user, "operand result has no parent block");
            return false;
        }
        if (v.index >= v.op->resultTypes.size()) {
            fail(user, "operand result index out of range");
            return false;
        }
        if (v.op->parent == cur) {
            auto idx = opIndex(cur, v.op);
            if (!idx || *idx >= curIndex) {
                if (std::getenv("XYO_MLIR_VDBG"))
                    std::fprintf(stderr, "[verify] %s uses %s (%d) at %zu\n",
                                 user ? user->name.c_str() : "?",
                                 v.op ? v.op->name.c_str() : "?",
                                 idx ? (int)*idx : -1, curIndex);
                fail(user, "use before definition in block");
                return false;
            }
            return true;
        }
        if (!contains(v.op->parent, cur)) {
            if (std::getenv("XYO_MLIR_VDBG"))
                std::fprintf(stderr, "[verify] %s uses %s in sibling\n",
                             user ? user->name.c_str() : "?",
                             v.op ? v.op->name.c_str() : "?");
            fail(user, "value defined in a sibling/dead region");
            return false;
        }
        return true;
    }

    void block(Block& b, bool isFunctionEntry) {
        for (size_t i = 0; i < b.ops.size(); ++i) {
            Op& op = *b.ops[i];
            if (op.parent != &b) fail(&op, "op parent block mismatch");
            if (dialect::isTerminator(op.name) && i + 1 != b.ops.size())
                fail(&op, "terminator is not the last op");
            if (!dialect::isTerminator(op.name) && i + 1 == b.ops.size() &&
                !isFunctionEntry && !b.ops.empty())
                fail(&op, "region block has no terminator");
            const size_t expected = dialect::regionCount(op.name);
            if (expected != op.regions.size())
                fail(&op, "region count mismatch for " + op.name);
            for (size_t a = 0; a < op.operands.size(); ++a) {
                const Value& v = op.operands[a];
                visible(v, &b, i, &op);
                if (v.type != op.operands[a].type) {
                    fail(&op, "operand type changed while verifying");
                }
            }
            for (auto& r : op.regions) {
                if (r->blocks.empty()) {
                    fail(&op, "region has no block");
                    continue;
                }
                for (size_t bi = 0; bi < r->blocks.size(); ++bi) {
                    Block& child = *r->blocks[bi];
                    if (bi == 0 && op.name != "func.func" &&
                        !child.args.empty()) {
                        // 入口 block の引数は op の iter_args に対応する。
                    }
                    block(child, op.name == "func.func");
                }
            }
        }
    }
};

} // namespace

bool verify(Module& m, std::vector<Diagnostic>* errors) {
    Verifier v{errors};
    for (auto& f : m.functions()) {
        if (!f.op || f.op->regions.size() != 1) {
            v.fail(f.op, "function has no body region");
            continue;
        }
        if (!f.body) {
            v.fail(f.op, "function has no entry block");
            continue;
        }
        v.block(*f.body, /*isFunctionEntry=*/true);
        // 関数シグネチャと entry block 引数の一致。
        if (f.body->args.size() != f.argTypes.size())
            v.fail(f.op, "function signature/entry args mismatch");
    }
    return v.count == 0;
}

// ============================================================= printer ====

namespace {

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

std::string attrsToString(const AttrMap& attrs) {
    if (attrs.empty()) return {};
    std::string out = " {";
    bool first = true;
    for (const auto& [k, a] : attrs) {
        if (!first) out += ", ";
        first = false;
        out += k;
        out += " = ";
        switch (a.kind) {
        case Attr::Kind::Int:
            out += std::to_string(a.i) + " : i64";
            break;
        case Attr::Kind::Double: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", a.d);
            out += buf;
            out += " : f64";
            break;
        }
        case Attr::Kind::Str: {
            out += '"';
            for (char c : a.s) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
            break;
        }
        default: out += "none";
        }
    }
    out += "}";
    return out;
}

struct Printer {
    std::map<Key, std::string> names;
    uint64_t next = 0;
    std::string out;

    Key keyOf(const Value& v) {
        if (v.isBlockArg())
            return Key{v.block, v.argIndex, true};
        return Key{v.op, v.index, false};
    }

    std::string nameOf(const Value& v) {
        auto it = names.find(keyOf(v));
        if (it != names.end()) return it->second;
        return "%?";
    }

    void assignBlock(Block& b) {
        for (auto& arg : b.args) {
            names[keyOf(arg)] = "%" + std::to_string(next++);
        }
        for (Op* op : b.ops) assignOp(*op);
    }

    void assignOp(Op& op) {
        for (size_t i = 0; i < op.resultTypes.size(); ++i) {
            names[Key{&op, static_cast<uint32_t>(i), false}] =
                "%" + std::to_string(next++);
        }
        for (auto& r : op.regions)
            for (auto& b : r->blocks) assignBlock(*b);
    }

    void printOp(Op& op, int indent) {
        std::string pad(static_cast<size_t>(indent) * 2, ' ');
        out += pad;
        if (!op.resultTypes.empty()) {
            bool first = true;
            for (size_t i = 0; i < op.resultTypes.size(); ++i) {
                if (!first) out += ", ";
                first = false;
                Value v;
                v.op = &op;
                v.index = static_cast<uint32_t>(i);
                out += nameOf(v);
            }
            out += " = ";
        }
        out += '"' + op.name + '"';
        out += "(";
        for (size_t i = 0; i < op.operands.size(); ++i) {
            if (i) out += ", ";
            out += nameOf(op.operands[i]);
        }
        out += ")";
        out += attrsToString(op.attrs);
        if (!op.regions.empty()) {
            out += " (";
            for (size_t ri = 0; ri < op.regions.size(); ++ri) {
                if (ri) out += ", ";
                out += "{\n";
                for (auto& b : op.regions[ri]->blocks)
                    printBlock(*b, indent + 1);
                out += pad + "  }";
            }
            out += ")";
        }
        out += " : (";
        for (size_t i = 0; i < op.operands.size(); ++i) {
            if (i) out += ", ";
            out += std::string(op.operands[i].type.name());
        }
        out += ") -> (";
        for (size_t i = 0; i < op.resultTypes.size(); ++i) {
            if (i) out += ", ";
            out += std::string(op.resultTypes[i].name());
        }
        out += ")\n";
    }

    void printBlock(Block& b, int indent) {
        std::string pad(static_cast<size_t>(indent) * 2, ' ');
        out += pad + "^bb" + std::to_string(b.index) + "(";
        for (size_t i = 0; i < b.args.size(); ++i) {
            if (i) out += ", ";
            out += nameOf(b.args[i]) + ": " +
                   std::string(b.args[i].type.name());
        }
        out += "):\n";
        for (Op* op : b.ops) printOp(*op, indent + 1);
    }
};

} // namespace

std::string Module::print() const {
    Printer p;
    for (auto& f : functions_)
        if (f.op) p.assignOp(*f.op);
    for (auto& f : functions_) {
        p.out += "func.func @" + f.name + "(";
        for (size_t i = 0; i < f.body->args.size(); ++i) {
            if (i) p.out += ", ";
            p.out += p.nameOf(f.body->args[i]) + ": " +
                     std::string(f.body->args[i].type.name());
        }
        p.out += ") {\n";
        for (Op* op : f.body->ops) p.printOp(*op, 1);
        p.out += "}\n";
    }
    return p.out;
}

} // namespace xyo::mlir
