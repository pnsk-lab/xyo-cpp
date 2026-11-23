// xyo-cpp: MLIR 風 IR の pass（canonicalize / CSE / DCE / 数値 SSA 昇格）。
//
// 定数畳み込みは jit/rt.cpp の実行時ヘルパをそのまま呼ぶ（fold 結果と
// 実行結果を構造的に一致させる）ため、JIT を有効にしたネイティブビルド
// でのみコンパイルする。
#include "ir.hpp"

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../core/sstr.hpp"
#include "../core/value.hpp"

namespace xyo::mlir {
namespace {

// 実行時ヘルパ（src/jit/rt.cpp）。定数畳み込みを「実行時と同じ関数」で
// 行うことで、fold 結果と実行結果が構造的に一致する。
extern "C" {
uint64_t xyo_add(uint64_t, uint64_t);
uint64_t xyo_sub(uint64_t, uint64_t);
uint64_t xyo_mul(uint64_t, uint64_t);
uint64_t xyo_div(uint64_t, uint64_t);
uint64_t xyo_mod(uint64_t, uint64_t);
int32_t xyo_lt(uint64_t, uint64_t);
int32_t xyo_gt(uint64_t, uint64_t);
int32_t xyo_eq(uint64_t, uint64_t);
int32_t xyo_truthy(uint64_t);
double xyo_to_num(uint64_t);
uint64_t xyo_num(double);
double xyo_round(double);
uint64_t xyo_join(uint64_t, uint64_t);
}

int64_t repeatTripCount(double c);

bool blockTerminated(const Block& b) {
    return !b.ops.empty() && dialect::isTerminator(b.ops.back()->name);
}

/// Module 内の全 block（関数本体 + region 内）を列挙する。
void walkBlocks(Module& m, const std::function<void(Block&)>& fn) {
    std::function<void(Block&)> rec = [&](Block& b) {
        fn(b);
        for (Op* op : b.ops)
            for (auto& r : op->regions)
                for (auto& child : r->blocks) rec(*child);
    };
    for (auto& f : m.functions()) rec(*f.body);
}

Op* insertBefore(Module& m, Block& b, size_t index, Op* op) {
    op->parent = &b;
    b.ops.insert(b.ops.begin() + static_cast<ptrdiff_t>(index), op);
    return op;
}

Op* insertAfter(Module& m, Block& b, size_t index, Op* op) {
    return insertBefore(m, b, index + 1, op);
}

void replaceAllUses(Module& m, Op* from, uint32_t fromIndex,
                    const Value& to) {
    m.forEachOp([&](Op& op) {
        for (auto& operand : op.operands)
            if (operand.op == from && operand.index == fromIndex)
                operand = to;
    });
}

void eraseFromBlock(Block& b, Op* op) {
    b.ops.erase(std::remove(b.ops.begin(), b.ops.end(), op), b.ops.end());
}

Value makeConstant(Module& m, Type type, const char* name, AttrMap attrs) {
    return m.create(name, {}, {type}, std::move(attrs))->result(0);
}

Value makeBoxedConst(Module& m, uint64_t bits) {
    return makeConstant(m, Type::value(), "xyo.const",
                        {{"bits", Attr::integer(static_cast<int64_t>(bits))}});
}

bool constF64(Op* op, double* out) {
    const Attr* a = op->attr("d");
    if (!a || !a->isDouble()) return false;
    *out = a->d;
    return true;
}

bool constBits(Op* op, uint64_t* out) {
    const Attr* a = op->attr("bits");
    if (!a || !a->isInt()) return false;
    *out = static_cast<uint64_t>(a->i);
    return true;
}

bool isNumericBits(uint64_t bits) { return xyo::Value{bits}.isNumber(); }

double scratchMod(double n, double m) {
    double r = std::fmod(n, m);
    if (r / m < 0) r += m;
    return r;
}

// ======================================================= canonicalize ====

struct Folder {
    Module& m;
    PassStats* stats;
    uint64_t folded = 0;

    // 現在の op の直前に定数 op を挿し、結果値で置換して op を消す。
    bool replaceWithBoxed(Block& b, size_t index, Op* op, uint64_t bits) {
        Value c = makeBoxedConst(m, bits);
        insertBefore(m, b, index, c.op);
        replaceAllUses(m, op, 0, c);
        eraseFromBlock(b, op);
        ++folded;
        return true;
    }
    bool replaceWithF64(Block& b, size_t index, Op* op, double d) {
        Value c = makeConstant(m, Type::f64(), "xyo.const.f64",
                               {{"d", Attr::real(d)}});
        insertBefore(m, b, index, c.op);
        replaceAllUses(m, op, 0, c);
        eraseFromBlock(b, op);
        ++folded;
        return true;
    }
    bool replaceWithBool(Block& b, size_t index, Op* op, bool v) {
        Value c = makeBoxedConst(m, v ? kTrueBits : kFalseBits);
        insertBefore(m, b, index, c.op);
        replaceAllUses(m, op, 0, c);
        eraseFromBlock(b, op);
        ++folded;
        return true;
    }
    static constexpr uint64_t kTrueBits = xyo::val::kTrue;
    static constexpr uint64_t kFalseBits = xyo::val::kFalse;

    void run(Block& b) {
        for (size_t i = 0; i < b.ops.size();) {
            Op* op = b.ops[i];
            if (foldOp(b, i, op)) continue;  // op が消えた
            ++i;
        }
    }

    bool foldOp(Block& b, size_t i, Op* op) {
        const std::string& n = op->name;
        // xyo.int_exact_ok(xyo.const) / (xyo.const.f64) はコンパイル時に
        // 確定できる。lower_llvm と同じ条件（素の非 -0.0 double・整数・
        // nonneg 時は [0,2^20]、それ以外は |v|<=2^20）で畳む。
        // 特にトリップ数の文字列リテラル（"10000" 等）は false と畳まれ、
        // int 昇格ループ全体が dead になるのを防ぐ。
        if (n == "xyo.int_exact_ok" && op->operands.size() == 1 &&
            op->operands[0].op) {
            Op* src = op->operands[0].op;
            uint64_t bits = 0;
            bool have = false;
            if (src->name == "xyo.const" && constBits(src, &bits))
                have = true;
            else if (src->name == "xyo.const.f64") {
                double d = 0;
                if (constF64(src, &d)) {
                    std::memcpy(&bits, &d, sizeof(bits));
                    have = true;
                }
            }
            if (have) {
                // lowerer と同じく to_num 基準: 数値化できる文字列
                // ("0" 等) も正確な整数なら昇格対象になる。
                const double d =
                    xyo::Value{bits}.isNumber()
                        ? xyo::Value{bits}.asNumberUnchecked()
                        : xyo_to_num(bits);
                uint64_t dbits = 0;
                std::memcpy(&dbits, &d, sizeof(dbits));
                static constexpr double kBound = 1048576.0;  // 2^20
                const bool nonNeg = op->attrInt("nonneg", 0) != 0;
                const bool notNegZero =
                    dbits != 0x8000000000000000ULL;
                const bool inRange =
                    nonNeg ? (d >= 0.0 && d <= kBound)
                           : (std::fabs(d) <= kBound);
                const bool isInt = std::trunc(d) == d;
                const bool ok = d == d && notNegZero && inRange && isInt;
                // boolean 定数（raw 0/1）。int_exact_ok の結果は常に
                // and/if の i1 経路に載るので !xyo.value ボックスには
                // しない（lowerer の bool 正規化が i1 化する）。
                Value c = makeConstant(
                    m, Type::boolean(), "xyo.const",
                    {{"bits", Attr::integer(ok ? 1 : 0)}});
                insertBefore(m, b, i, c.op);
                replaceAllUses(m, op, 0, c);
                eraseFromBlock(b, op);
                ++folded;
                return true;
            }
        }
        // 数値定数だけを対象にする（文字列/GC パスには触れない）。
        auto boxedNum = [&](size_t k, double* out) {
            if (k >= op->operands.size()) return false;
            Value v = op->operands[k];
            if (!v.op) return false;
            uint64_t bits = 0;
            if (!constBits(v.op, &bits) || !isNumericBits(bits)) return false;
            *out = xyo_to_num(bits);
            return true;
        };
        auto f64Const = [&](size_t k, double* out) {
            return k < op->operands.size() && op->operands[k].op &&
                   constF64(op->operands[k].op, out);
        };

        if (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
            n == "xyo.div" || n == "xyo.mod") {
            double a = 0, c = 0;
            if (!boxedNum(0, &a) || !boxedNum(1, &c)) return false;
            uint64_t bits = 0;
            // 実行時ヘルパをそのまま呼ぶ（-0/NaN/Inf も同一経路）。
            xyo::Value av = xyo::Value::numCanonical(a);
            xyo::Value cv = xyo::Value::numCanonical(c);
            uint64_t ab = av.bits, cb = cv.bits;
            if (n == "xyo.add") bits = xyo_add(ab, cb);
            else if (n == "xyo.sub") bits = xyo_sub(ab, cb);
            else if (n == "xyo.mul") bits = xyo_mul(ab, cb);
            else if (n == "xyo.div") bits = xyo_div(ab, cb);
            else bits = xyo_mod(ab, cb);
            return replaceWithBoxed(b, i, op, bits);
        }
        if (n == "xyo.add.f" || n == "xyo.sub.f" || n == "xyo.mul.f" ||
            n == "xyo.div.f" || n == "xyo.mod.f") {
            double a = 0, c = 0;
            if (!f64Const(0, &a) || !f64Const(1, &c)) return false;
            double r = 0;
            if (n == "xyo.add.f") r = a + c;
            else if (n == "xyo.sub.f") r = a - c;
            else if (n == "xyo.mul.f") r = a * c;
            else if (n == "xyo.div.f") r = a / c;
            else r = scratchMod(a, c);
            return replaceWithF64(b, i, op, r);
        }
        if (n == "xyo.lt" || n == "xyo.gt" || n == "xyo.eq") {
            double a = 0, c = 0;
            if (!boxedNum(0, &a) || !boxedNum(1, &c)) return false;
            uint64_t ab = xyo::Value::numCanonical(a).bits;
            uint64_t cb = xyo::Value::numCanonical(c).bits;
            int32_t r = n == "xyo.lt" ? xyo_lt(ab, cb)
                                      : n == "xyo.gt" ? xyo_gt(ab, cb)
                                                      : xyo_eq(ab, cb);
            return replaceWithBool(b, i, op, r != 0);
        }
        if (n == "xyo.truthy") {
            double a = 0;
            if (!boxedNum(0, &a)) return false;
            uint64_t ab = xyo::Value::numCanonical(a).bits;
            return replaceWithBool(b, i, op, xyo_truthy(ab) != 0);
        }
        if (n == "xyo.to_num") {
            double a = 0;
            if (!boxedNum(0, &a)) {
                // 数値でない定数（文字列等）も実行時と同じヘルパで畳み込む。
                // SStr は pinner 保持で実行時まで生きる。
                if (op->operands.size() == 1 && op->operands[0].op &&
                    op->operands[0].op->name == "xyo.const") {
                    uint64_t bits = 0;
                    if (constBits(op->operands[0].op, &bits))
                        return replaceWithF64(b, i, op, xyo_to_num(bits));
                }
                return false;
            }
            return replaceWithF64(
                b, i, op,
                xyo_to_num(xyo::Value::numCanonical(a).bits));
        }
        if (n == "xyo.from_num") {
            double a = 0;
            if (!f64Const(0, &a)) return false;
            return replaceWithBoxed(b, i, op, xyo_num(a));
        }
        if (n == "xyo.round") {
            double a = 0;
            if (!boxedNum(0, &a)) return false;
            return replaceWithBoxed(b, i, op, xyo_num(xyo_round(a)));
        }
        if (n == "xyo.round.f") {
            double a = 0;
            if (!f64Const(0, &a)) return false;
            return replaceWithF64(b, i, op, xyo_round(a));
        }
        return false;
    }
};

// ==================================================================== CSE ====

std::string cseKey(Op& op) {
    std::string key = op.name;
    key += '(';
    for (const Value& v : op.operands) {
        if (v.op)
            key += "o" + std::to_string(v.op->debugId) + ":" +
                   std::to_string(v.index);
        else
            key += "b" + std::to_string(
                             reinterpret_cast<uintptr_t>(v.block)) + ":" +
                   std::to_string(v.argIndex);
        key += ',';
    }
    key += ')';
    for (const auto& [k, a] : op.attrs) {
        key += k;
        key += '=';
        switch (a.kind) {
            case Attr::Kind::Double: {
                // double 属性（xyo.const.f64 の d など）を鍵に含める。
                // 以前は i/s しか連結しておらず、**同ブロックの別値の
                // const.f64 が全部 1 個に畳まれて 0 になっていた**
                // （mandel で 1957 → 556 の誤答として発覚）。
                uint64_t bits = 0;
                std::memcpy(&bits, &a.d, sizeof(bits));
                key += "d" + std::to_string(bits);
                break;
            }
            case Attr::Kind::Int:
                key += "i" + std::to_string(a.i);
                break;
            case Attr::Kind::Str:
                key += "s" + a.s;
                break;
            case Attr::Kind::None:
                break;
        }
        key += ';';
    }
    return key;
}

bool cseEligible(Op& op) {
    static const std::unordered_set<std::string> kOk = {
        "xyo.const",  "xyo.const.f64", "xyo.add",     "xyo.sub",
        "xyo.mul",    "xyo.div",       "xyo.mod",     "xyo.add.f",
        "xyo.sub.f",  "xyo.mul.f",     "xyo.div.f",   "xyo.mod.f",
        "xyo.round",  "xyo.round.f",   "xyo.mathop",  "xyo.mathop.f",
        "xyo.to_num", "xyo.from_num",
        "xyo.const.i", "xyo.add.i",    "xyo.sub.i",   "xyo.mul.i",
        "xyo.mod.i",  "xyo.cmp.i",     "xyo.from_int", "xyo.to_int",
    };
    return kOk.count(op.name) != 0;
}

// ==================================================================== DCE ====

void runDce(Module& m, uint64_t* removed) {
    bool changed = true;
    while (changed) {
        changed = false;
        std::unordered_set<Op*> used;
        m.forEachOp([&](Op& op) {
            for (const Value& v : op.operands)
                if (v.op) used.insert(v.op);
        });
        std::vector<std::pair<Block*, Op*>> dead;
        walkBlocks(m, [&](Block& b) {
            for (Op* op : b.ops)
                if (dialect::isPure(op->name) && !used.count(op) &&
                    !op->resultTypes.empty())
                    dead.push_back({&b, op});
        });
        for (auto& [b, op] : dead) {
            eraseFromBlock(*b, op);
            ++*removed;
            changed = true;
        }
    }
}

// ====================================================== 数値 SSA 昇格 ====


struct LoopCandidate {
    std::string id;
    std::string name;
    // 現在の SSA 値。昇格変数は (Value ビット, f64 影) の対で持つ。
    Value bits;
    Value d;
    // ループ前の実メモリ値（0 回反復のときに書き戻す値）。
    Value initBits;
    // 整数プロモーション: i64 で持ち回る。entry は定数。
    bool isInt = false;
    Value iv;
    int64_t intEntry = 0;
    int64_t intLo = 0;
    int64_t intHi = 0;
    // ガード版: entry は定数でなく、入口ガード（int_exact_ok）が
    // 整数性を保証する。区間の起点はガード範囲。
    bool intGuarded = false;
};

/// 昇格できる「葉」の op（入れ子 region は xyo.if だけ別扱い）。
bool promotableLeaf(const std::string& n) {
    static const std::unordered_set<std::string> kOk = {
        "xyo.const",   "xyo.const.f64", "xyo.getvar",  "xyo.setvar",
        "xyo.add",     "xyo.sub",       "xyo.mul",     "xyo.div",
        "xyo.mod",     "xyo.round",     "xyo.mathop",  "xyo.to_num",
        "xyo.from_num", "xyo.boolval",  "xyo.lt",      "xyo.gt",
        "xyo.eq",      "xyo.truthy",    "xyo.not",     "xyo.and",
        "xyo.or",      "xyo.band",      "xyo.bor",     "xyo.bnot",
        "xyo.cmp.f",   "xyo.select",    "xyo.to_str",
        "xyo.strlen",  "xyo.loop_yield", "xyo.condition",
        // リスト操作は変数を書かない（実メモリの shadow を汚さない）ので
        // 昇格ループ内に置いてよい。sensing_of や compat は書く可能性が
        // あるため引き続き不許可。
        "xyo.list.ptr",    "xyo.list.item",    "xyo.list.itemnum",
        "xyo.list.length", "xyo.list.add",     "xyo.list.deleteall",
        "xyo.list.replace", "xyo.list.insert", "xyo.list.delete",
        "xyo.list.contains", "xyo.list.contents",
        // 文字列 reporter（変数を書かない）。join は可変長の
        // オペランド列を持つため昇格ループ内では未対応。
        "xyo.letterof", "xyo.strcontains", "xyo.join",
    };
    return kOk.count(n) != 0;
}

/// 内側ループを持つループの昇格（`XYO_MLIR_NONESTPROMOTE=1` で無効化）。
///
/// Round 234 で入れたときは `math_mandel` の結果が legacy と一致しなかった
/// が、原因はこの機能ではなく **CSE の鍵が double 属性を含んでいなかった**
/// こと（同ブロックの別値の `xyo.const.f64` が全部 1 個に畳まれていた）。
/// 鍵を直した Round 236 以降、47 ベンチのデータ系チェックサムは
/// すべて legacy と一致する。
bool nestedLoopEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("XYO_MLIR_NONESTPROMOTE");
        return !(e && e[0] == '1');
    }();
    return on;
}

/// 本体に置いてよい「昇格前の入れ子ループ」か。
///
/// 外側を昇格すると、昇格値はレジスタ（SSA）に載る。入れ子ループは
/// 実メモリの変数を読み書きするので、入る前に spill・抜けたら reload
/// すれば、入れ子の中断中もメモリの値は常に正しい（＝他のスクリプトから
/// 見える値が変わらない）。その条件は:
///   - 入れ子の中に中断点（call / compat / 待機 / さらに深いループ）が無い
///     （あれば、その時点でメモリと SSA が食い違う可能性を排除できない）
///   - 入れ子が書く変数が分かる（分からないものは整数化の対象から外す）
bool plainNestedLoop(Op* op, std::unordered_set<std::string>* writes) {
    if (op->name != "xyo.while" && op->name != "xyo.repeat" &&
        op->name != "xyo.forever")
        return false;
    if (op->regions.empty()) return false;
    bool ok = true;
    std::function<void(Block&)> walk = [&](Block& b) {
        for (Op* o : b.ops) {
            if (!ok) return;
            if (o->name == "xyo.setvar" && !o->operands.empty())
                writes->insert(std::string(o->attrStr("id")));
            if (o->name == "xyo.if" || o->name == "xyo.yield" ||
                o->name == "xyo.condition" || o->name == "xyo.loop_yield" ||
                o->name == "xyo.warp_yield" || o->name == "xyo.getvar" ||
                o->name == "xyo.setvar") {
                for (auto& r : o->regions)
                    if (r)
                        for (auto& bp : r->blocks) walk(*bp);
                continue;
            }
            if (dialect::isRegionOp(o->name) || !promotableLeaf(o->name)) {
                ok = false;
                return;
            }
        }
    };
    for (auto& r : op->regions)
        if (r)
            for (auto& bp : r->blocks) walk(*bp);
    return ok;
}

/// 本体（if の奥も含む）に「昇格前の入れ子ループ」があるか。
bool bodyHasPlainNestedLoop(Block& body) {
    bool found = false;
    std::function<void(Block&)> walk = [&](Block& b) {
        for (Op* op : b.ops) {
            if (found) return;
            if (op->name == "xyo.if") {
                for (auto& r : op->regions)
                    if (r)
                        for (auto& bp : r->blocks) walk(*bp);
                continue;
            }
            std::unordered_set<std::string> w;
            if (plainNestedLoop(op, &w)) found = true;
        }
    };
    walk(body);
    return found;
}

struct Scan {
    std::map<std::string, LoopCandidate> cands;
    std::vector<Op*> nestedOps;
    std::unordered_set<std::string> nestedWrites;
};

/// op の深い複製（region/block 引数を含む）。複製範囲内の参照
/// （複製した op の結果・複製した block 引数）は付け替え、範囲外への
/// 参照はそのまま保つ（可視性は呼び出し側が保証する）。
/// 入口ガード付きループ複製（変数単位整数化＋途中脱出なし）で使う。
struct Cloner {
    Module& m;
    std::unordered_map<Op*, Op*> opMap;
    struct ArgKey {
        const Block* b = nullptr;
        uint32_t i = 0;
        bool operator==(const ArgKey& o) const = default;
    };
    struct ArgHash {
        size_t operator()(const ArgKey& k) const noexcept {
            return std::hash<const void*>{}(k.b) * 31u + k.i;
        }
    };
    std::unordered_map<ArgKey, Value, ArgHash> argMap;

    Value remap(const Value& v) {
        if (!v.valid()) return v;
        if (v.isBlockArg()) {
            auto it = argMap.find({v.block, v.argIndex});
            if (it != argMap.end()) return it->second;
            return v;  // 複製範囲外（count 等の親値）は共有する。
        }
        if (v.op) {
            auto it = opMap.find(v.op);
            if (it != opMap.end()) return it->second->result(v.index);
            return v;
        }
        return v;
    }

    Op* cloneInto(Op* src, Block& dest) {
        Op* c = m.create(src->name, {}, src->resultTypes, src->attrs);
        for (const Value& v : src->operands)
            c->operands.push_back(remap(v));
        c->parent = &dest;
        dest.ops.push_back(c);
        opMap[src] = c;
        for (auto& r : src->regions) {
            Region* nr = c->addRegion();
            for (auto& sb : r->blocks) {
                Block* nb = nr->addBlock();
                for (size_t i = 0; i < sb->args.size(); ++i) {
                    Value a;
                    a.block = nb;
                    a.argIndex = static_cast<uint32_t>(i);
                    a.type = sb->args[i].type;
                    nb->args.push_back(a);
                    argMap[{sb.get(), static_cast<uint32_t>(i)}] = a;
                }
            }
            for (size_t bi = 0; bi < r->blocks.size(); ++bi)
                for (Op* o : r->blocks[bi]->ops)
                    cloneInto(o, *nr->blocks[bi]);
        }
        return c;
    }
};

/// 本体（と入れ子の xyo.if）を走査して昇格可否と変数一覧を作る。
bool scanBlock(Block& b, Scan* s, int depth) {
    if (depth > 8) return false;
    for (Op* op : b.ops) {
        if (op->name == "xyo.if") {
            if (op->regions.size() != 2) return false;
            for (auto& r : op->regions) {
                if (!r || r->blocks.size() != 1) return false;
                if (!scanBlock(*r->entry(), s, depth + 1)) return false;
            }
            continue;
        }
        if (dialect::isRegionOp(op->name)) {
            // 昇格前の入れ子ループはブラックボックスとして置ける。
            // 値の受け渡しは rewriteBlock が spill/reload で行う。
            std::unordered_set<std::string> w;
            if (nestedLoopEnabled() && plainNestedLoop(op, &w)) {
                s->nestedOps.push_back(op);
                for (auto& id : w) s->nestedWrites.insert(id);
                continue;
            }
            return false;
        }
        if (op->name == "xyo.yield") continue;
        if (!promotableLeaf(op->name)) return false;
        if (op->name == "xyo.getvar" || op->name == "xyo.setvar") {
            LoopCandidate c;
            c.id = std::string(op->attrStr("id"));
            c.name = std::string(op->attrStr("name"));
            s->cands.emplace(c.id, c);
        }
    }
    return true;
}

bool constBitsOf(Op* op, uint64_t* out) {
    if (!op || op->name != "xyo.const") return false;
    return constBits(op, out);
}

/// 整数リテラルの値。xyo.const（素の double ビット）または xyo.const.f64
/// で、有限・整数・i32 範囲のときだけ true。
bool intConstValue(const Value& v, int64_t* out) {
    if (!v.op) return false;
    if (v.op->name == "xyo.const") {
        uint64_t bits = 0;
        if (!constBits(v.op, &bits)) return false;
        return intLiteralBits(bits, out);
    }
    if (v.op->name == "xyo.const.f64") {
        // Attr の double は綴りを持たない素の数値として扱う。
        const Attr* a = v.op->attr("d");
        if (!a || !a->isDouble()) return false;
        double d = a->d;
        if (d != d || d < -2147483648.0 || d > 2147483647.0) return false;
        if (std::trunc(d) != d) return false;
        *out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

/// 定数を Scratch の数値として評価する（数値リテラルは文字列 Value で
/// 来ることがある — shadow フィールドは文字列のまま保持される）。
bool constNumValue(const Value& v, double* out) {
    if (!v.op) return false;
    if (v.op->name == "xyo.const.f64") {
        const Attr* a = v.op->attr("d");
        if (!a || !a->isDouble()) return false;
        *out = a->d;
        return true;
    }
    if (v.op->name != "xyo.const") return false;
    uint64_t bits = 0;
    if (!constBits(v.op, &bits)) return false;
    *out = xyo_to_num(bits);
    return true;
}

struct Promoter {
    Module& m;
    Block& parent;
    size_t loopIndex;
    Op* loop;
    Block& body;
    std::map<std::string, LoopCandidate> cands;
    Scan scanTmp;
    std::unordered_map<Op*, std::string> getvarOf;  // 除去した getvar → id
    PassStats* stats;
    bool warpLoop = false;
    bool condTerm = false;  // while の cond 領域を書き換え中か
    std::vector<std::string> order;
    // 変数ごとの運搬表現: i=i32, d=f64 のみ, b=(bits,f64)。
    // d は while ループ + 数値専用変数（m.numericVars）だけに使い、
    // lowering の xyo.warp_yield kinds="d"（実際に譲る 1 回だけ box.f を
    // 作る）と対にする。既定は b（従来動作）。
    std::unordered_map<std::string, char> kinds_;
    char kindOf(const std::string& id) const {
        auto it = kinds_.find(id);
        return it == kinds_.end() ? 'b' : it->second;
    }
    static bool dOnlyEnabled() {
        static const bool on = [] {
            const char* e = std::getenv("XYO_MLIR_DONLY");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    void planKinds(bool allowD) {
        kinds_.clear();
        for (const std::string& id : order) {
            if (intLoop && cands[id].isInt) {
                kinds_[id] = 'i';
            } else if (allowD && warpLoop && dOnlyEnabled() &&
                       m.numericVars.count(id)) {
                kinds_[id] = 'd';
            } else {
                kinds_[id] = 'b';
            }
        }
    }
    // 整数プロモーション: 全昇格変数が i64 で回る（all-or-nothing）。
    // entry は定数・本体は整数閉・区間は i32・書き込みは全てループ内か
    // 整数リテラルであることが静的に証明できたときだけ true。
    bool intLoop = false;
    // preheader 走査で集めた定数（entry 候補 + 本体が読む不変量）。
    std::unordered_map<std::string, int64_t> intConsts;
    // 最終区間で「i32 で閉じた」と証明できた op（書き換え時の判定用）。
    // 書き換えで op 名が .i に変わっても同じポインタで引ける。
    std::unordered_set<Op*> intProvenOps;
    // if の substack を書き換えている間、その if が代入する変数だけを
    // yield に載せる（全昇格変数を載せると phi が爆発する）。
    const std::unordered_set<std::string>* ifAssigned_ = nullptr;
    // 最終区間での mod 被除数の非負性（書き換え時の urem 選択用）。
    // mod op -> 整数化の方針。-1: srem+補正、-2: urem、
    // 0 以上: sub+select をその回数だけ繰り返す（被除数 < (k+1)*m 証明済み）。
    std::unordered_map<Op*, int> modSubK;
    // 分岐で証明された偶数性（`if (mod(V,2)==0)` の真側）。V が偶数
    // なら `div(V,2)` は正確で整数化できる。分岐内だけ有効なスコープ
    // 付き集合で、setvar で解除する。div op 自体の精密区間は
    // divRanges_（永続）に記録し、書換え時の intOkOf からも引く。
    // op が分岐内にしか存在しないため、分岐依存の区間でも健全。
    std::unordered_set<std::string> evenVars_;
    // IntVal は後方定義のため pair で持つ（{lo,hi}）。
    std::unordered_map<Op*, std::pair<int64_t, int64_t>> divRanges_;
    // 入口ガード版プロモーション（変数単位整数化＋入口検査）。
    // entry 非定数の変数にガード範囲を与えた解析が成功したときだけ使う。
    // ガード範囲: |v| ≤ 2^20 の正確な整数（int_exact_ok が検査）。
    static constexpr int64_t kGuardBound = 1048576;
    // ガード版の解析中に参照する entry 範囲（nullptr なら通常解析）。
    const std::map<std::string, std::pair<int64_t, int64_t>>*
        guardEntries_ = nullptr;
    /// true なら head 区間を毎ラウンド guardEntries_ のボックスへ交差
    /// させる（ループ条件がボックスを強制する版だけが使う）。
    bool clampToBox_ = false;
    /// true なら「ループ内の書き込みが全て整数式」だけを要求し、
    /// プロジェクト全体の非整数書き込みは問わない（ボックス版のみ）。
    /// 健全性は入口ガード＋head の inrange＋yield reload の検証で担保する。
    bool relaxClean_ = false;
    /// この promoter が担当する int ループがボックス版か（reload 検証用）。
    bool boxLoop_ = false;
    static bool boxEnabled() {
        static const bool on = [] {
            const char* e = std::getenv("XYO_MLIR_BOX");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    // ガード版の検査極性。true なら全ガード変数に [0, 2^20] の
    // 非負ボックスを仮定し、int_exact_ok に nonneg 属性を付ける。
    bool guardNonNeg_ = false;
    /// 強制反復回数（box-repeat 解析用）。0 以上なら tripBound() が
    /// 実際の被演算子の代わりにこの値を返す。動的 trip の repeat を
    /// 解析可能にする（実行時は trip ガードで上限を保証する）。
    /// 解析の前後で呼び出し側が設定・復帰する。
    int64_t forcedTrips_ = -1;
    /// 本体にある昇格前の入れ子ループ（spill/reload の対象）。
    std::vector<Op*> nestedOps_;
    std::unordered_set<std::string> nestedWrites_;
    bool isNestedOp(Op* op) const {
        for (Op* o : nestedOps_)
            if (o == op) return true;
        return false;
    }

    Promoter(Module& mm, Block& pp, size_t li, Op* lp, Block& bb,
             std::map<std::string, LoopCandidate> cc = {},
             Scan ss = {}, std::unordered_map<Op*, std::string> gv = {},
             PassStats* st = nullptr)
        : m(mm),
          parent(pp),
          loopIndex(li),
          loop(lp),
          body(bb),
          cands(std::move(cc)),
          scanTmp(std::move(ss)),
          getvarOf(std::move(gv)),
          stats(st) {}
    static bool guardEnabled() {
        static const bool on = [] {
            const char* e = std::getenv("XYO_MLIR_GUARD");
            return !(e && e[0] == '0');
        }();
        return on;
    }

    Op* make(std::string name, std::vector<Value> operands,
             std::vector<Type> results, AttrMap attrs = {}) {
        Op* op = m.create(std::move(name), std::move(operands),
                          std::move(results), std::move(attrs));
        return op;
    }
    void push(Block& b, std::vector<Op*>& out, Op* op) {
        op->parent = &b;
        out.push_back(op);
    }

    /// 整数プロモーションの i64 値。getvar(候補)→運搬値、定数→const.i、
    /// 整数 op 結果→そのまま。失敗時は空 Value（呼び出し側で proof との
    /// 不整合として扱う。proof が通った本体では起きない）。
    /// 整数プロモーションの i64 値。in-place 変換済みの .i op・
    /// const.i・getvar（運搬値/定数）を返す。to_num/round は恒等なので
    /// 辿る。失敗時は空 Value（呼び出し側で verifier に落とさせる）。
    Value iOf(const Value& v, Block& b, std::vector<Op*>& out) {
        if (v.op) {
            auto it = getvarOf.find(v.op);
            if (it != getvarOf.end()) {
                LoopCandidate& c = cands[it->second];
                if (c.isInt) {
                    if (!c.iv.op && !c.iv.isBlockArg() && intDebug())
                        std::fprintf(stderr,
                                     "[mlir-int] iOf empty iv for var %s\n",
                                     it->second.c_str());
                    return c.iv;
                }
                return Value{};
            }
            const std::string& n = v.op->name;
            if (n == "xyo.from_int" && !v.op->operands.empty())
                return v.op->operands[0];
            if (n == "xyo.add.i" || n == "xyo.sub.i" || n == "xyo.mul.i" ||
                n == "xyo.mod.i" || n == "xyo.div.i" ||
                n == "xyo.const.i" || n == "xyo.to_int") {
                // in-place 変換前の古い型が残っていることがあるため付け直す。
                Value r = v;
                r.type = Type::i32();
                return r;
            }
            if (n == "xyo.getvar") {
                std::string id(v.op->attrStr("id"));
                int64_t c = 0;
                // in-body 非候補は preheader 定数（ループ内で書かれない）。
                if (inLoopBody(v.op)) {
                    auto cit = intConsts.find(id);
                    if (cit == intConsts.end()) return Value{};
                    c = cit->second;
                } else if (!outerIntConst(v, &c)) {
                    return Value{};
                }
                Op* k = make("xyo.const.i", {}, {Type::i32()},
                             {{"v", Attr::integer(c)}});
                push(b, out, k);
                return k->result(0);
            }
            if (n == "xyo.list.item" || n == "xyo.list.item.i") {
                // 要素整数リストの読みは to_int で i32 にする（内容証明に
                // より正確。範囲外は空文字列→ 0 になる）。
                Op* t = make("xyo.to_int", {v}, {Type::i32()});
                push(b, out, t);
                return t->result(0);
            }
            if ((n == "xyo.div" || n == "xyo.div.i") &&
                v.op->operands.size() == 2 &&
                divRanges_.count(v.op) != 0) {
                // 偶数証明済みの div。arith 書換えが先に .i 化していれば
                // 上（.i 集合）で拾う。ここに来るのは書換えが飛ばした
                // 場合で、i32 で運ぶ。証明なしの div は通さない
                // （不健全な i32 化になる）。
                int64_t dd = 0;
                if (!intConstValue(v.op->operands[1], &dd) || dd <= 0)
                    return Value{};
                Value av = iOf(v.op->operands[0], b, out);
                Value cv = iOf(v.op->operands[1], b, out);
                if (!av || !cv) return Value{};
                Op* d = make("xyo.div.i", {av, cv}, {Type::i32()});
                push(b, out, d);
                return d->result(0);
            }
            int64_t c = 0;
            if (intConstValue(v, &c)) {
                Op* k = make("xyo.const.i", {}, {Type::i32()},
                             {{"v", Attr::integer(c)}});
                push(b, out, k);
                return k->result(0);
            }
        }
        if (v.type == Type::i64() || v.type == Type::i32()) return v;
        if (intDebug())
            std::fprintf(stderr, "[mlir-int] iOf miss: %s\n",
                         v.op ? v.op->name.c_str() : "(blockarg)");
        return Value{};
    }

    /// 昇格変数の Value ビット（必要なら作る）。
    /// 整数変数は from_int で作る（i32 範囲の整数なので正確）。
    Value intBitsOf(const Value& iv, Block& b, std::vector<Op*>& out) {
        Op* f = make("xyo.from_int", {iv}, {Type::value()});
        push(b, out, f);
        return f->result(0);
    }
    /// オペランド Value に記録された型は書き換え前のことがある（op の
    /// resultTypes は in-place で更新されるため）。現在の型を引く。
    static Type curType(const Value& v) {
        if (v.op && v.index < v.op->resultTypes.size())
            return v.op->resultTypes[v.index];
        return v.type;
    }

    Value bitsOf(const Value& v, Block& b, std::vector<Op*>& out) {
        if (v.op) {
            auto it = getvarOf.find(v.op);
            if (it != getvarOf.end()) {
                LoopCandidate& c = cands[it->second];
                if (c.isInt) return intBitsOf(c.iv, b, out);
                if (c.bits) return c.bits;
                // d-only 運搬: 必要になった時点で box.f を作る。
                if (c.d) {
                    Op* bx = make("xyo.box.f", {c.d}, {Type::value()});
                    push(b, out, bx);
                    c.bits = bx->result(0);
                }
                return c.bits;
            }
        }
        const Type ct = curType(v);
        if (ct == Type::i32() || ct == Type::i64()) {
            // 書き換え済みの整数 SSA（mod.i 等）を値として使う箇所。
            Op* fx = make("xyo.from_int", {v}, {Type::value()});
            push(b, out, fx);
            return fx->result(0);
        }
        if (ct == Type::boolean()) {
            Op* bv = make("xyo.boolval", {v}, {Type::value()});
            push(b, out, bv);
            return bv->result(0);
        }
        if (ct == Type::f64()) {
            Op* bx = make("xyo.box.f", {v}, {Type::value()});
            push(b, out, bx);
            return bx->result(0);
        }
        return v;
    }
    /// 昇格変数の f64 影。整数変数は to_num(from_int(iv))（正確）。
    Value dOf(const Value& v, Block& b, std::vector<Op*>& out) {
        if (v.op) {
            // box.f(f) の数値影は f 自身（to_num を挟まない）。
            if (v.op->name == "xyo.box.f" && !v.op->operands.empty())
                return v.op->operands[0];
            auto it = getvarOf.find(v.op);
            if (it != getvarOf.end()) {
                LoopCandidate& c = cands[it->second];
                if (c.isInt) {
                    Value bits = intBitsOf(c.iv, b, out);
                    Op* t = make("xyo.to_num", {bits}, {Type::f64()});
                    push(b, out, t);
                    return t->result(0);
                }
                return c.d;
            }
        }
        if (curType(v) == Type::f64()) return v;
        uint64_t bits = 0;
        if (v.op && constBitsOf(v.op, &bits)) {
            Op* c = make("xyo.const.f64", {}, {Type::f64()},
                         {{"d", Attr::real(xyo_to_num(bits))}});
            push(b, out, c);
            return c->result(0);
        }
        Value bv = bitsOf(v, b, out);
        Op* t = make("xyo.to_num", {bv}, {Type::f64()});
        push(b, out, t);
        return t->result(0);
    }
    /// 「素の非 NaN double か」の i1。定数はコンパイル時に畳む。
    Value okOf(const Value& v, Block& b, std::vector<Op*>& out) {
        if (v.op) {
            auto it = getvarOf.find(v.op);
            if (it != getvarOf.end()) {
                Value cb = bitsOf(v, b, out);
                Op* ok = make("xyo.num_ok", {cb}, {Type::boolean()});
                push(b, out, ok);
                return ok->result(0);
            }
            uint64_t bits = 0;
            if (constBitsOf(v.op, &bits)) {
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                const bool ok = bits < static_cast<uint64_t>(val::kNan) &&
                                d == d;
                Op* c = make("xyo.const", {}, {Type::boolean()},
                             {{"bits", Attr::integer(ok ? 1 : 0)}});
                push(b, out, c);
                return c->result(0);
            }
            if (v.op->name == "xyo.cmp.f" || v.op->name == "xyo.lt" ||
                v.op->name == "xyo.gt" || v.op->name == "xyo.eq" ||
                v.op->name == "xyo.truthy" || v.op->name == "xyo.not" ||
                v.op->name == "xyo.and" || v.op->name == "xyo.or") {
                Op* c = make("xyo.const", {}, {Type::boolean()},
                             {{"bits", Attr::integer(0)}});
                push(b, out, c);
                return c->result(0);
            }
        }
        Value bv = bitsOf(v, b, out);
        Op* ok = make("xyo.num_ok", {bv}, {Type::boolean()});
        push(b, out, ok);
        return ok->result(0);
    }
    /// 定数の i1 を作る（コンパイル時定数）。
    Value constBool(bool v, Block& b, std::vector<Op*>& out) {
        Op* c = make("xyo.const", {}, {Type::boolean()},
                     {{"bits", Attr::integer(v ? 1 : 0)}});
        push(b, out, c);
        return c->result(0);
    }
    Value constF64(double d, Block& b, std::vector<Op*>& out) {
        Op* c = make("xyo.const.f64", {}, {Type::f64()}, {{"d", Attr::real(d)}});
        push(b, out, c);
        return c->result(0);
    }

    /// --- 整数プロモーションの解析 ---
    /// 整数値の抽象値。ok=false は「整数と証明できない」。
    /// tainted=true は昇格候補の getvar を含む（= ループ運搬値に依存）。
    struct IntVal {
        bool ok = false;
        bool tainted = false;
        int64_t lo = 0;
        int64_t hi = 0;
    };

    static IntVal intUnknown(bool tainted) {
        IntVal r;
        r.ok = false;
        r.tainted = tainted;
        return r;
    }

    static IntVal intRange(int64_t lo, int64_t hi, bool tainted) {
        IntVal r;
        r.ok = true;
        r.tainted = tainted;
        r.lo = lo;
        r.hi = hi;
        return r;
    }

    /// preheader（parent の fromIndex より前）を逆走し、定数代入を集める。
    /// 最も近い同一 var への setvar が整数リテラルなら entry。
    /// region・呼び出し・compat・停止・待機などに当たったら打ち切り。
    /// 本体内の getvar はループ運搬値、preheader の getvar はこの写像で
    /// 定数化できる（位置に依存するため呼び出し側で使い分ける）。
    void scanIntConstsFrom(size_t fromIndex,
                           std::unordered_map<std::string, int64_t>& out) const {
        out.clear();
        if (intDebug()) {
            std::fprintf(stderr, "[mlir-int] scan from=%zu size=%zu\n",
                         fromIndex, parent.ops.size());
        }
        std::unordered_set<std::string> killed;
        for (size_t k = fromIndex; k > 0;) {
            --k;
            Op* op = parent.ops[k];
            if (intDebug() && k < 5) {
                std::string extra;
                if (op->name == "xyo.setvar")
                    extra = " id=" + std::string(op->attrStr("id"));
                std::fprintf(stderr, "[mlir-int]   [%zu] %s%s\n", k,
                             op->name.c_str(), extra.c_str());
            }
            const std::string& n = op->name;
            if (n == "xyo.setvar") {
                if (op->operands.empty()) continue;
                std::string id(op->attrStr("id"));
                if (killed.count(id)) continue;
                killed.insert(id);
                int64_t c = 0;
                if (intConstValue(op->operands[0], &c)) out[id] = c;
                continue;
            }
            if (dialect::isPure(n) || n == "xyo.getvar") continue;
            // リスト葉 op はスカラー変数を書かないので飛ばす。fill と
            // 初期化の順序（add→set／set→add）に依らず entry を拾う。
            // region を持つものは従来どおり打ち切り（保守側）。
            if (n.size() > 9 && n.compare(0, 9, "xyo.list.") == 0 &&
                op->regions.empty())
                continue;
            return;  // 打ち切り（これより前は見ない）
        }
    }

    void scanIntConsts() { scanIntConstsFrom(loopIndex, intConsts); }

    /// 入れ子 if の内側で昇格候補へ書き込むか（区間合流が要るため
    /// 整数プロモーションの対象外にする）。
    bool hasIntIfWrite(Block& b, int depth = 0) const {
        if (depth > 8) return true;
        for (Op* op : b.ops) {
            if (op->name == "xyo.if") {
                for (auto& r : op->regions) {
                    if (!r || r->blocks.size() != 1) continue;
                    if (hasIntIfWrite(*r->entry(), depth + 1)) return true;
                }
                continue;
            }
            if (op->name == "xyo.setvar" && !op->operands.empty()) {
                std::string id(op->attrStr("id"));
                if (cands.count(id) && depth > 0) return true;
            }
        }
        return false;
    }

    /// 本体中の if がすべて「候補変数への単純代入のみ」で構成された
    /// 2 領域か。そうなら両分岐を評価して select にできる（分岐なし）。
    bool intIfsSupported(Block& b, int depth = 0) const {
        if (depth > 4) return false;
        for (Op* op : b.ops) {
            if (op->name == "xyo.if") {
                if (op->regions.size() != 2) return false;
                for (auto& r : op->regions) {
                    if (!r || r->blocks.size() != 1) return false;
                    Block* rb = r->entry();
                    std::unordered_set<std::string> seen;
                    for (Op* o : rb->ops) {
                        if (ifRegionOpOk(o, &seen)) continue;
                        return false;
                    }
                }
                continue;
            }
            if (op->name == "xyo.while" || op->name == "xyo.repeat" ||
                op->name == "xyo.for_each" || op->name == "xyo.forever")
                return false;
        }
        return true;
    }

    /// if 領域内で許す op: terminator / 純粋 op / 候補の getvar /
    /// 候補への setvar（同一領域で 1 回まで）。
    bool ifRegionOpOk(Op* o, std::unordered_set<std::string>* seen) const {
        if (!o) return false;
        if (o->name == "xyo.yield") return true;
        if (o->name == "xyo.setvar") {
            if (o->operands.empty()) return false;
            const std::string id(o->attrStr("id"));
            if (!cands.count(id)) return false;
            return seen->insert(id).second;
        }
        if (o->name == "xyo.getvar" || dialect::isPure(o->name))
            return o->regions.empty();
        return false;
    }

    /// 1 個の if が「候補への単純代入のみ」の 2 領域か（倍精度 select 用）。
    bool floatIfSelectable(const Op* op) const {
        if (!op || op->regions.size() != 2 || op->operands.size() != 1)
            return false;
        for (auto& r : op->regions) {
            if (!r || r->blocks.size() != 1) return false;
            std::unordered_set<std::string> seen;
            for (Op* o : r->entry()->ops) {
                if (ifRegionOpOk(o, &seen)) continue;
                return false;
            }
        }
        return true;
    }

    /// 1 個の if が「候補への単純代入のみ」の 2 領域か（select 化の前提）。
    bool intIfSelectable(const Op* op) const {
        if (!op || op->name != "xyo.if" || op->regions.size() != 2 ||
            op->operands.size() != 1)
            return false;
        for (auto& r : op->regions) {
            if (!r || r->blocks.size() != 1) return false;
            std::unordered_set<std::string> seen;
            for (Op* o : r->entry()->ops) {
                if (ifRegionOpOk(o, &seen)) continue;
                return false;
            }
        }
        return true;
    }

    /// op がループ本体（+入れ子 if）の内側にあるか。
    bool inLoopBody(const Op* op) const {
        const Block* b = op ? op->parent : nullptr;
        while (b) {
            if (b == &body) return true;
            if (!b->parent) return false;
            Op* owner = b->parent->owner;
            b = owner ? owner->parent : nullptr;
        }
        return false;
    }

    /// 本体内 getvar の整数定数（preheader 由来）。本体外は位置で判定。
    bool outerIntConst(const Value& v, int64_t* out) const {
        if (!v.op || v.op->name != "xyo.getvar") return false;
        // 親ブロック内の位置を探す（preheader の直線コードのみ対象）。
        const Block* pb = v.op->parent;
        if (pb != &parent) return false;
        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < parent.ops.size(); ++i) {
            if (parent.ops[i] == v.op) {
                idx = i;
                found = true;
                break;
            }
        }
        if (!found) return false;
        std::unordered_map<std::string, int64_t> m;
        scanIntConstsFrom(idx, m);
        auto it = m.find(std::string(v.op->attrStr("id")));
        if (it == m.end()) return false;
        *out = it->second;
        return true;
    }

    /// 値の整数評価（DAG 再帰。ループ運搬は iv 写像経由で読む）。
    IntVal evalIntValue(
        const Value& v,
        const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& iv,
        std::unordered_map<Op*, IntVal>& memo) {
        auto mit = memo.find(v.op);
        if (v.op && mit != memo.end()) return mit->second;
        // 分岐依存の精密区間（div の偶数証明）。op が分岐内にしかない
        // ため、記録時の区間がそのまま有効。
        if (v.op) {
            auto dit = divRanges_.find(v.op);
            if (dit != divRanges_.end())
                return intRange(dit->second.first, dit->second.second,
                                true);
        }
        IntVal r = evalIntUnmemo(v, iv, memo);
        if (v.op && r.ok) intProvenOps.insert(v.op);
        return r;
    }

    IntVal evalIntUnmemo(
        const Value& v,
        const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& iv,
        std::unordered_map<Op*, IntVal>& memo) {
        if (!v.op) {
            // block 引数（書き換え前の本体には無い）は未知・非汚染。
            return intUnknown(false);
        }
        Op* op = v.op;
        const std::string& n = op->name;
        if (n == "xyo.getvar") {
            std::string id(op->attrStr("id"));
            if (inLoopBody(op)) {
                // 本体内: 候補は運搬値、それ以外は loop entry 時の
                // preheader 定数（ループ内で書かれないため不変）。
                auto it = iv.find(id);
                if (it != iv.end())
                    return intRange(it->second.first, it->second.second,
                                    true);
                auto cit = intConsts.find(id);
                if (cit != intConsts.end())
                    return intRange(cit->second, cit->second, false);
                return intUnknown(false);
            }
            // 本体外（preheader 等）: 位置依存の定数のみ信用する。
            int64_t c = 0;
            if (outerIntConst(v, &c)) return intRange(c, c, false);
            return intUnknown(false);
        }
        if (n == "xyo.const" || n == "xyo.const.f64") {
            int64_t c = 0;
            if (intConstValue(v, &c)) return intRange(c, c, false);
            return intUnknown(false);
        }
        if (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul") {
            if (op->operands.size() != 2)
                return intUnknown(false);
            IntVal a = evalIntValue(op->operands[0], iv, memo);
            IntVal b = evalIntValue(op->operands[1], iv, memo);
            if (!a.ok || !b.ok)
                return intUnknown(a.tainted || b.tainted);
            if (n == "xyo.mul") {
                // -0.0 の生成を避ける（i32 輸送で +0 になり 1/x の符号が
                // 変わる）。+0 と負数の積だけが -0 になり得る。
                const bool aHasZero = a.lo <= 0 && 0 <= a.hi;
                const bool bHasZero = b.lo <= 0 && 0 <= b.hi;
                if ((aHasZero && b.lo < 0) || (bHasZero && a.lo < 0))
                    return intUnknown(a.tainted || b.tainted);
            }
            __int128 lo, hi;
            if (n == "xyo.add") {
                lo = (__int128)a.lo + b.lo;
                hi = (__int128)a.hi + b.hi;
            } else if (n == "xyo.sub") {
                lo = (__int128)a.lo - b.hi;
                hi = (__int128)a.hi - b.lo;
            } else {
                __int128 p[4] = {(__int128)a.lo * b.lo, (__int128)a.lo * b.hi,
                                (__int128)a.hi * b.lo, (__int128)a.hi * b.hi};
                lo = hi = p[0];
                for (int k = 1; k < 4; ++k) {
                    if (p[k] < lo) lo = p[k];
                    if (p[k] > hi) hi = p[k];
                }
            }
            if (lo < INT32_MIN || hi > INT32_MAX)
                return intUnknown(a.tainted || b.tainted);
            return intRange((int64_t)lo, (int64_t)hi,
                            a.tainted || b.tainted);
        }
        if (n == "xyo.div") {
            // 除算は原則として整数値ではない（5/2 = 2.5）。
            // ただし除数が定数 2 で被除数が偶数証明済みなら正確
            // （collatz の n/2）。偶数性は `if (mod(V,2)==0)` の真側で
            // evenVars_ に記録される。trig の atan(i/100) のように
            // 証明が無いものは従来どおり対象外。
            if (op->operands.size() != 2) return intUnknown(false);
            int64_t dd = 0;
            if (!intConstValue(op->operands[1], &dd) || dd != 2)
                return intUnknown(false);
            const Value& num = op->operands[0];
            int64_t lo = 0, hi = 0;
            bool tainted = false;
            if (num.op && num.op->name == "xyo.getvar") {
                std::string id(num.op->attrStr("id"));
                auto it = iv.find(id);
                if (it == iv.end() || !evenVars_.count(id))
                    return intUnknown(false);
                lo = it->second.first;
                hi = it->second.second;
                tainted = true;
            } else {
                int64_t c = 0;
                if (!intConstValue(num, &c) || (c & 1) != 0)
                    return intUnknown(false);
                lo = hi = c;
            }
            // 偶数 ÷ 2 は正確。ceil/floor で区間を狭める
            // （境界が奇数でも外側へ丸めるので健全）。
            auto ceilDiv2 = [](int64_t x) -> int64_t {
                return x >= 0 ? (x + 1) / 2 : x / 2;
            };
            auto floorDiv2 = [](int64_t x) -> int64_t {
                return x >= 0 ? x / 2 : (x - 1) / 2;
            };
            int64_t nlo = ceilDiv2(lo), nhi = floorDiv2(hi);
            if (nlo > nhi) return intUnknown(false);
            if (nlo < INT32_MIN || nhi > INT32_MAX)
                return intUnknown(false);
            IntVal r = intRange(nlo, nhi, tainted);
            divRanges_[op] = {nlo, nhi};
            return r;
        }
        if (n == "xyo.mod") {
            if (op->operands.size() != 2)
                return intUnknown(false);
            IntVal a = evalIntValue(op->operands[0], iv, memo);
            int64_t m = 0;
            if (!a.ok || !intConstValue(op->operands[1], &m) || m <= 0 ||
                m > INT32_MAX)
                return intUnknown(a.tainted);
            // 正の除数: 非負なら [0, min(hi,m-1)]、負を含めば [-(m-1), m-1]。
            if (a.lo >= 0)
                return intRange(0, std::min(a.hi, m - 1), a.tainted);
            return intRange(-(m - 1), m - 1, a.tainted);
        }
        if (n == "xyo.list.item" || n == "xyo.list.item.i") {
            // exact range のあるリストの読みは整数。範囲外は空文字列→
            // 数値 0 になるので、区間は 0 で広げたものを返す。
            if (op->operands.empty() || !op->operands[0].op)
                return intUnknown(false);
            std::string lid(op->operands[0].op->attrStr("id"));
            auto lr = m.intListRanges.find(lid);
            if (lr == m.intListRanges.end()) {
                bool t = false;
                for (const Value& u : op->operands)
                    t = t || evalIntValue(u, iv, memo).tainted;
                return intUnknown(t);
            }
            bool t = false;
            if (op->operands.size() > 1)
                t = evalIntValue(op->operands[1], iv, memo).tainted;
            return intRange(lr->second.first, lr->second.second, t);
        }
        // 未知 op（select を含む）: 汚染だけ伝播（呼び出し側で
        // tainted&&!ok を弾く）。select は書き換えで扱わないため、
        // 候補に触れるとループ全体が倍精度になる。
        // 未知 op: 汚染だけ伝播（呼び出し側で tainted&&!ok を弾く）。
        bool t = false;
        for (const Value& u : op->operands)
            t = t || evalIntValue(u, iv, memo).tainted;
        return intUnknown(t);
    }

    /// `setvar` のオペランドが「必ず数値 Value を生成する」か。int 輸送は
    /// store を num(int) にするので、整数として読めるが数値でない値
    /// （正準形の整数文字列、bool、文字を返し得る op）を書き込むと表現が
    /// interp とずれる。ここで保守的に拒否した変数は 'b' 運搬（生 bits
    /// 保持）に落ちるため意味は壊れない。
    bool exactIntOperand(
        const Value& v,
        const std::unordered_map<std::string, std::pair<int64_t, int64_t>>&
            iv,
        int depth = 0) const {
        // block 引数（'b' 運搬の生 bits）は文字列を持ち得るので不可。
        if (!v.op || depth > 8) return false;
        Op* op = v.op;
        const std::string& n = op->name;
        if (n == "xyo.const") {
            uint64_t bits = 0;
            if (!constBits(op, &bits)) return false;
            if (isNumericBits(bits)) return true;
            // 正準形の整数文字列（s:"1" 等）も可。Scratch の数値
            // リテラル shadow は生文字列のまま const になるため、
            // `change i by 1` の典型形がここで落ちていた。
            // intLiteralBits が toString での往復を保証しているため、
            // i32 経由で数値化して格納しても観測可能な表現は変わらない
            // （格納側は const なら rawBits、読みは to_int で同一）。
            // 非正準形（" 1"・"1.0"・""）は従来どおり不可。
            int64_t dummy = 0;
            return intLiteralBits(bits, &dummy);
        }
        if (n == "xyo.const.f64") return true;
        if (n == "xyo.getvar") {
            // 整数候補（i32 運搬 = 常に数値）だけ。他の変数は文字列を
            // 持ち得る。entry の const は値ではなく型を見るので除外。
            return iv.count(std::string(op->attrStr("id"))) != 0;
        }
        // .i 系・from_int・to_int は常に数値 Value。
        if (n == "xyo.from_int" ||
            (n.size() > 2 && n.compare(n.size() - 2, 2, ".i") == 0 &&
             n != "xyo.list.item.i" && n != "xyo.list.itemnum.i"))
            return true;
        // 四則・mod・round・from_num・from_bool は常に数値を返す。
        if (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
            n == "xyo.div" || n == "xyo.mod" || n == "xyo.round" ||
            n == "xyo.from_num" || n == "xyo.boolval" ||
            n == "xyo.mathop" || n == "xyo.list.len" ||
            n == "xyo.list.itemnum" || n == "xyo.strlen" ||
            n == "xyo.num_ok") {
            for (const Value& u : op->operands)
                if (u.type == Type::value() &&
                    !exactIntOperand(u, iv, depth + 1))
                    return false;
            return true;
        }
        // select/if の両分岐が数値確定なら数値。
        if (n == "xyo.select" && op->operands.size() == 3)
            return exactIntOperand(op->operands[1], iv, depth + 1) &&
                   exactIntOperand(op->operands[2], iv, depth + 1);
        return false;
    }

    /// 本体（+ 入れ子 if）を走査する共有ヘルパー。
    template <typename F> void walkIntBody(Block& b, F&& fn, int depth = 0) {
        if (depth > 8) return;
        for (Op* op : b.ops) {
            if (op->name == "xyo.if" && op->regions.size() == 2) {
                bool ok = true;
                for (auto& r : op->regions) {
                    if (!r || r->blocks.size() != 1) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    for (auto& r : op->regions)
                        walkIntBody(*r->entry(), fn, depth + 1);
                    fn(op);
                    continue;
                }
            }
            fn(op);
        }
    }

    /// ループ全体の整数証明。成功時は cands の isInt/intEntry/intLo/intHi

    /// frontend の clean fixpoint と同じ基準での整数式判定（IR 版）。
    /// sameFuncOutside の計数に使い、数え方（ext）と一致させる。
    /// getvar は clean 集合（全書き込みが整数生成式）で判定する。
    bool cleanProvable(const Value& v, int depth = 0) const {
        if (!v.op || depth > 8) return false;
        int64_t c = 0;
        if (intConstValue(v, &c)) return true;
        Op* op = v.op;
        if (op->name == "xyo.getvar")
            return m.intCleanVars.count(std::string(op->attrStr("id"))) != 0;
        if (op->name == "xyo.list.item" || op->name == "xyo.list.item.i") {
            if (op->operands.empty() || !op->operands[0].op) return false;
            std::string lid(op->operands[0].op->attrStr("id"));
            return m.intListRanges.count(lid) != 0;
        }
        // xyo.div は整数値ではない（実数値と切り捨て表現が一致しない）。
        if (op->name == "xyo.add" || op->name == "xyo.sub" ||
            op->name == "xyo.mul" || op->name == "xyo.mod" ||
            op->name == "xyo.round" || op->name == "xyo.to_num") {
            for (const Value& u : op->operands)
                if (!cleanProvable(u, depth + 1)) return false;
            return true;
        }
        return false;
    }

    /// 整数値を作る式か（IR）。const 整数・整数化対象の getvar・
    /// add/sub/mul/mod/round/to_num（整数オペランド）・exact range リストの
    /// 要素読みを認める。
    bool intExprProvable(const Value& v,
                         const std::unordered_set<std::string>& intVars,
                         int depth = 0) const {
        if (!v.op || depth > 8) return false;
        int64_t c = 0;
        if (intConstValue(v, &c)) return true;
        Op* op = v.op;
        if (op->name == "xyo.getvar")
            return intVars.count(std::string(op->attrStr("id"))) != 0;
        if (op->name == "xyo.list.item" || op->name == "xyo.list.item.i") {
            if (op->operands.empty() || !op->operands[0].op) return false;
            std::string lid(op->operands[0].op->attrStr("id"));
            return m.intListRanges.count(lid) != 0;
        }
        // xyo.div は整数値ではない（実数値と切り捨て表現が一致しない）。
        if (op->name == "xyo.add" || op->name == "xyo.sub" ||
            op->name == "xyo.mul" || op->name == "xyo.mod" ||
            op->name == "xyo.round" || op->name == "xyo.to_num") {
            for (const Value& u : op->operands)
                if (!intExprProvable(u, intVars, depth + 1)) return false;
            return true;
        }
        return false;
    }

    /// ループ条件から head 時点の変数の範囲を求める（1 変数 vs 定数の
    /// 比較だけ）。until の条件は否定されているので bnot の個数で極性を
    /// 決める。条件が真である間しか body は走らないため、body 内の演算は
    /// この範囲を前提にしてよい。
    bool condBound(const std::string& id, int64_t* lo, int64_t* hi) const {
        if (loop->name != "xyo.while" || !loop->region(0)) return false;
        Block* cond = loop->region(0)->entry();
        if (!cond) return false;
        Op* term = cond->lastOp();
        if (!term || term->name != "xyo.condition" || term->operands.empty())
            return false;
        // and/or の木を極性付きで降りる（条件が真のとき）。
        //   not(x)      : 極性反転
        //   and(a,b) 真 : 両方真   / 偽 : 不明
        //   or(a,b)  偽 : 両方偽   / 真 : 不明
        std::function<bool(const Value&, bool)> walk =
            [&](const Value& v, bool wantTrue) -> bool {
            if (!v.op) return false;
            const std::string& n = v.op->name;
            if (n == "xyo.bnot" || n == "xyo.not") {
                return !v.op->operands.empty() &&
                       walk(v.op->operands[0], !wantTrue);
            }
            const bool isAnd = n == "xyo.and" || n == "xyo.band";
            const bool isOr = n == "xyo.or" || n == "xyo.bor";
            if ((isAnd && wantTrue) || (isOr && !wantTrue)) {
                bool any = false;
                for (const Value& u : v.op->operands)
                    any = walk(u, wantTrue) || any;
                return any;
            }
            if (isAnd || isOr) return false;  // 片方だけの制約は不明
            if (v.op->operands.size() != 2) return false;
            const std::string& cmp = n;
            if (cmp != "xyo.gt" && cmp != "xyo.lt") return false;
            Value a = v.op->operands[0], b = v.op->operands[1];
            bool flipped = false;
            auto isSelf = [&](const Value& x) {
                return x.op && x.op->name == "xyo.getvar" &&
                       std::string(x.op->attrStr("id")) == id;
            };
            if (!isSelf(a)) {
                if (!isSelf(b)) return false;
                std::swap(a, b);
                flipped = true;
            }
            int64_t c = 0;
            if (!intConstValue(b, &c)) return false;
            std::string op = cmp;
            if (flipped) op = (cmp == "xyo.gt") ? "xyo.lt" : "xyo.gt";
            if (op == "xyo.gt") {
                if (wantTrue) {
                    if (*lo < c + 1) *lo = c + 1;
                } else {
                    if (*hi > c) *hi = c;
                }
            } else {  // lt
                if (wantTrue) {
                    if (*hi > c - 1) *hi = c - 1;
                } else {
                    if (*lo < c) *lo = c;
                }
            }
            return true;
        };
        return walk(term->operands[0], true);
    }

    /// repeat の定数回数 C から、その変数の head 区間の上界を求める。
    /// 本体の書き込みが「定数代入」または「自分自身 + 定数」だけで、
    /// 入れ子ループ内に書き込みが無いときだけ計算する。
    bool repeatBoundOf(const std::string& id, int64_t trips,
                        const std::unordered_map<std::string,
                                                 std::pair<int64_t, int64_t>>&
                            iv,
                        int64_t* lo, int64_t* hi) {
        bool sawWrite = false;
        bool ok = true;
        int64_t cmin = INT64_MAX, cmax = INT64_MIN;
        __int128 pos = 0, neg = 0;
        std::unordered_map<Op*, IntVal> memo;
        std::function<void(Block&, int)> walk = [&](Block& blk, int depth) {
            if (!ok || depth > 8) return;
            for (Op* op : blk.ops) {
                if (!ok) return;
                if (op->name == "xyo.if") {
                    if (op->regions.size() != 2) { ok = false; return; }
                    for (auto& r : op->regions) {
                        if (!r || r->blocks.size() != 1) { ok = false; return; }
                        walk(*r->entry(), depth + 1);
                    }
                    continue;
                }
                if (op->name == "xyo.while" || op->name == "xyo.repeat" ||
                    op->name == "xyo.for_each" || op->name == "xyo.forever") {
                    // 入れ子ループ内の書き込みは反復回数が不明 → 上界なし。
                    std::function<void(Block&, int)> scan =
                        [&](Block& nb, int d) {
                            if (d > 8) return;
                            for (Op* o : nb.ops) {
                                if (o->name == "xyo.setvar" &&
                                    !o->operands.empty() &&
                                    std::string(o->attrStr("id")) == id)
                                    ok = false;
                                if (o->regions.empty()) continue;
                                for (auto& r : o->regions)
                                    if (r && r->blocks.size() == 1)
                                        scan(*r->entry(), d + 1);
                            }
                        };
                    for (auto& r : op->regions)
                        if (r && r->blocks.size() == 1) scan(*r->entry(), 0);
                    continue;
                }
                if (op->name != "xyo.setvar" || op->operands.empty()) continue;
                if (std::string(op->attrStr("id")) != id) continue;
                sawWrite = true;
                const Value& v = op->operands[0];
                int64_t c = 0;
                if (intConstValue(v, &c)) {
                    cmin = std::min(cmin, c);
                    cmax = std::max(cmax, c);
                    continue;
                }
                if (!v.op || v.op->operands.size() != 2) { ok = false; return; }
                Op* a = v.op;
                if (a->name != "xyo.add" && a->name != "xyo.sub") {
                    ok = false;
                    return;
                }
                const Value& x = a->operands[0];
                const Value& y = a->operands[1];
                auto self = [&](const Value& q) {
                    return q.op && q.op->name == "xyo.getvar" &&
                           std::string(q.op->attrStr("id")) == id;
                };
                int64_t d = 0;
                bool found = false;
                const Value* delta = nullptr;
                if (self(x) && intConstValue(y, &d)) {
                    found = true;
                } else if (a->name == "xyo.add" && self(y) &&
                           intConstValue(x, &d)) {
                    found = true;
                } else if (self(x)) {
                    delta = &y;
                } else if (a->name == "xyo.add" && self(y)) {
                    delta = &x;
                }
                if (delta) {
                    // 非定数増分: 現在の区間で評価し、正側・負側に分けて
                    // 加算する（定数増分と同じ式。区間は音な上界）。
                    IntVal r = evalIntValue(*delta, iv, memo);
                    if (!r.ok) {
                        ok = false;
                        return;
                    }
                    if (a->name == "xyo.sub") {
                        int64_t t = r.lo;
                        r.lo = -r.hi;
                        r.hi = -t;
                    }
                    if (r.hi > 0) pos += r.hi;
                    if (r.lo < 0) neg += r.lo;
                    found = true;
                }
                if (!found) { ok = false; return; }
                if (a->name == "xyo.sub") d = -d;
                if (d >= 0) pos += d;
                else neg += d;
            }
        };
        walk(body, 0);
        if (!ok || !sawWrite) return false;
        const LoopCandidate& lc = cands.at(id);
        int64_t baseLo = lc.intEntry;
        int64_t baseHi = lc.intEntry;
        if (lc.intGuarded && guardEntries_) {
            const auto& gr = guardEntries_->at(id);
            baseLo = gr.first;
            baseHi = gr.second;
        }
        if (cmin != INT64_MAX) {
            baseLo = std::min(baseLo, cmin);
            baseHi = std::max(baseHi, cmax);
        }
        __int128 l = (__int128)baseLo + (__int128)trips * neg;
        __int128 h = (__int128)baseHi + (__int128)trips * pos;
        if (l < INT32_MIN || h > INT32_MAX || l > h) return false;
        *lo = (int64_t)l;
        *hi = (int64_t)h;
        return true;
    }

    /// 反復回数とループ条件から、その変数の head 区間の音な上界を返す。
    bool headBoundOf(const std::string& id, int64_t trips,
                     const std::unordered_map<std::string,
                                              std::pair<int64_t, int64_t>>& iv,
                     int64_t* lo, int64_t* hi) {
        int64_t l = INT64_MIN, h = INT64_MAX;
        bool have = false;
        if (loop->name == "xyo.while" && condBound(id, &l, &h)) have = true;
        if (trips >= 0) {
            int64_t tl = 0, th = 0;
            if (repeatBoundOf(id, trips, iv, &tl, &th)) {
                l = std::max(l, tl);
                h = std::min(h, th);
                have = true;
            }
        }
        if (!have) return false;
        *lo = l;
        *hi = h;
        return true;
    }

    /// ループ種別から反復回数（定数の repeat / for_each のみ）を求める。
    int64_t tripBound() const {
        if (forcedTrips_ >= 0) return forcedTrips_;
        if (!loop->operands.empty()) {
            double c = 0;
            if (loop->name == "xyo.repeat" && constNumValue(loop->operands[0], &c))
                return repeatTripCount(c);
            if (loop->name == "xyo.for_each" &&
                constNumValue(loop->operands[0], &c) && c >= 0.0 && c < 1e9 &&
                c == std::floor(c))
                return (int64_t)c;
        }
        return -1;
    }

    /// head 区間を反復回数・条件で締める（音な上界なので交差してよい）。
    void clampHeadIntervals(
            std::unordered_map<std::string, std::pair<int64_t, int64_t>>&
                headIv,
            const std::unordered_set<std::string>& active) {
        const int64_t trips = tripBound();
        for (const std::string& id : active) {
            auto& h = headIv[id];
            int64_t lo = 0, hi = 0;
            if (clampToBox_ && guardEntries_) {
                auto g = guardEntries_->find(id);
                if (g != guardEntries_->end()) {
                    h.first = std::max(h.first, g->second.first);
                    h.second = std::min(h.second, g->second.second);
                    if (h.first > h.second) h.second = h.first;
                }
            }
            if (!headBoundOf(id, trips, headIv, &lo, &hi)) continue;
            int64_t nl = std::max(h.first, lo);
            int64_t nh = std::min(h.second, hi);
            // 音な上界どうしの交差は空にならない（entry を含む）が、
            // 万一空になったら締めない（誤った narrowing を避ける）。
            if (nl > nh) continue;
            h.first = nl;
            h.second = nh;
        }
    }

    /// 最終区間で本体の各式を評価し、i32 で閉じた op を記録する。
    /// 書き換えは前から順に op 名を .i へ変えるため、後続の op から見ると
    /// オペランドが「未知の op」になる。ポインタで覚えておけば同じ DAG を
    /// 再評価できる。
    void recordIntOps() {
        intProvenOps.clear();
        std::unordered_map<std::string, std::pair<int64_t, int64_t>> iv;
        for (auto& [id, c] : cands)
            if (c.isInt) iv[id] = {c.intLo, c.intHi};
        std::unordered_map<Op*, IntVal> memo;
        std::function<void(Block&)> eval = [&](Block& blk) {
            for (Op* op : blk.ops) {
                // 任意の op のオペランドと結果を評価して、整数で閉じた
                // 部分式を記録する（setvar 式の外にある添字計算も含む）。
                if (op->name != "xyo.if" && !dialect::isRegionOp(op->name)) {
                    for (const Value& u : op->operands)
                        evalIntValue(u, iv, memo);
                    if (!op->resultTypes.empty())
                        evalIntValue(op->result(0), iv, memo);
                }
                if (op->name == "xyo.setvar") {
                    if (op->operands.empty()) continue;
                    std::string id(op->attrStr("id"));
                    auto it = cands.find(id);
                    if (it == cands.end() || !it->second.isInt) continue;
                    IntVal r = evalIntValue(op->operands[0], iv, memo);
                    if (r.ok) iv[id] = {r.lo, r.hi};
                    continue;
                }
                if (op->name == "xyo.if") {
                    if (op->regions.size() != 2) continue;
                    auto pre = iv;
                    if (op->region(0)) eval(*op->region(0)->entry());
                    auto thenIv = iv;
                    iv = pre;
                    if (op->region(1)) eval(*op->region(1)->entry());
                    for (auto& [k, v] : iv) {
                        auto t = thenIv.find(k);
                        if (t == thenIv.end()) continue;
                        v.first = std::min(v.first, t->second.first);
                        v.second = std::max(v.second, t->second.second);
                    }
                }
            }
        };
        eval(body);
    }

    /// ループ全体の整数証明。成功時は「整数で持ち回れる変数」だけに
    /// isInt/intEntry/intLo/intHi を立てる（部分整数化）。整数化できない
    /// 変数は従来どおり (bits, f64) の対で持ち回る。
    bool analyzeIntLoop(const std::map<std::string,
                         std::pair<int64_t, int64_t>>* guardEntries = nullptr,
                         bool clampToBox = false,
                         bool relaxClean = false) {
        guardEntries_ = guardEntries;
        clampToBox_ = clampToBox;
        relaxClean_ = relaxClean;
        // 試行ごとに証明セットを清める（失敗時の残留が rewrite の
        // 判定に混ざらないようにする）。
        intProvenOps.clear();
        modSubK.clear();
        evenVars_.clear();
        divRanges_.clear();
        for (auto& [id, c] : cands) c.intGuarded = false;
        if (m.intEligibleVars.empty()) {
            intTrace("no eligible vars (%s)", std::string());
            return false;
        }
        std::unordered_set<std::string> active;
        for (auto& [id, c] : cands)
            if (m.intEligibleVars.count(id)) active.insert(id);
        if (active.empty()) {
            intTrace("no eligible cands (%s)", std::string());
            return false;
        }
        scanIntConsts();
        // if 領域が「候補への単純代入だけ」でないなら、if 内で書かれる
        // 変数は制御依存で区間を狭められないため整数化しない。
        if (!intIfsSupported(body)) {
            std::unordered_set<std::string> ifWrites;
            std::function<void(Block&)> collect = [&](Block& blk) {
                for (Op* op : blk.ops) {
                    if (op->name != "xyo.if") continue;
                    for (auto& r : op->regions) {
                        if (!r || r->blocks.size() != 1) continue;
                        for (Op* o : r->entry()->ops)
                            if (o->name == "xyo.setvar" &&
                                !o->operands.empty())
                                ifWrites.insert(std::string(o->attrStr("id")));
                        collect(*r->entry());
                    }
                }
            };
            collect(body);
            for (const std::string& id : ifWrites) active.erase(id);
            if (active.empty()) {
                intTrace("if regions not selectable: no int vars",
                         std::string());
                return false;
            }
        }
        // entry が定数の変数だけを残す。ガード版では entry 非定数の
        // 変数にガード範囲を与えて続行する（入口検査が整数性を保証）。
        for (auto& [id, c] : cands) {
            if (!active.count(id)) continue;
            auto it = intConsts.find(id);
            if (it == intConsts.end()) {
                if (guardEntries) {
                    auto g = guardEntries->find(id);
                    if (g == guardEntries->end()) {
                        active.erase(id);
                        continue;
                    }
                    c.intGuarded = true;
                    continue;
                }
                active.erase(id);
                continue;
            }
            c.intEntry = it->second;
        }
        if (active.empty()) {
            if (intDebug()) {
                std::string cs, ks;
                for (auto& [id, c] : cands) cs += id + " ";
                for (auto& [id, v] : intConsts)
                    ks += id + "=" + std::to_string(v) + " ";
                std::fprintf(stderr, "[mlir-int] no const entry loop=%s cands=[%s] consts=[%s]\n",
                             loop->name.c_str(), cs.c_str(), ks.c_str());
                for (size_t zi = 0; zi <= loopIndex && zi < parent.ops.size(); ++zi)
                    std::fprintf(stderr, "[mlir-int]   p[%zu] %s\n", zi,
                                 parent.ops[zi]->name.c_str());
            }
            intTrace("no const entry (%s)", std::string());
            return false;
        }
        std::unordered_map<std::string, std::pair<int64_t, int64_t>> headIv;
        for (const std::string& id : active) {
            const LoopCandidate& lc = cands[id];
            if (lc.intGuarded && guardEntries) {
                headIv[id] = guardEntries->at(id);
            } else {
                headIv[id] = {cands[id].intEntry, cands[id].intEntry};
            }
        }
        // fixpoint: ループ頭の区間を backedge 値の和集合で更新する。
        // 本体内の途中書き込みは和集合に入れない（mod で抑えられる前の
        // 中間値が上限を押し上げ続ける発散を避けるため）。
        // 上限が 3 回連続で伸びたら program 由来の閾値（mod の除数と
        // INT32_MAX）へワイドニングする。証明できない変数は落とし、
        // 残りだけで続行する（部分整数化）。
        std::unordered_set<int64_t> thresholds;
        walkIntBody(body, [&](Op* op) {
            if (op->name != "xyo.mod" || op->operands.size() != 2) return;
            int64_t mm = 0;
            if (intConstValue(op->operands[1], &mm) && mm > 0 &&
                mm <= INT32_MAX)
                thresholds.insert(mm - 1);
        });
        thresholds.insert(INT32_MAX);
        std::unordered_map<std::string, int> growth;
        // if 条件から得た変数ごとの上界候補（wrap カウンタ用）。
        // widening の発火時に閾値の代わりにここへジャンプする。
        // 不正な候補は union で戻り、収束判定が健全性を保証する。
        std::unordered_map<std::string, int64_t> narrowCapHi;
        const int64_t trips = tripBound();
        bool converged = false;
        for (int round = 0; round < 64; ++round) {
            bool changed = false;
            using IvMap =
                std::unordered_map<std::string, std::pair<int64_t, int64_t>>;
            IvMap iv;
            // 反復回数・ループ条件が与える上界を先に適用する（適用しないと
            // `i = i + 1` の評価が i32 を溢れて変数ごと落ちる）。
            clampHeadIntervals(headIv, active);
            for (const std::string& id : active) iv[id] = headIv[id];
            std::unordered_map<Op*, IntVal> memo;
            // Gauss-Seidel: iv をその場で更新しながら走査する。
            // setvar は上書き（REPLACE）。中間値は head に入れない。
            // if は両分岐を評価して区間を union する（条件付き書き込みを
            // 片側の値で狭めると不健全になる）。
            // if 条件による区間の絞り込み（wrap カウンタ用）。
            // `if (i > 1000) i = 1` の形は反復回数によらず i を有界に
            // 保つが、単なる union では else 側が際限なく広がる。条件が
            // 「整数候補 V と定数 C の比較」であれば、各分岐の評価前に
            // V の区間を条件の含意で交差させる（到達不能な分岐の絞り込み
            // は union で吸収されるため安全側に倒れる）。
            // condTrue: 条件が真の分岐を評価中か。V が active の整数候補
            // であること（実行時は必ず整数＝NaN なし）が前提。
            std::function<bool(const Value&, int, int64_t, bool, IvMap&,
                               int)>
                assumeConstBound = [&](const Value& x, int pred, int64_t c,
                                      bool condTrue, IvMap& m,
                                      int depth) -> bool {
                if (depth > 8) return false;
                if (x.op) {
                    const std::string& xn = x.op->name;
                    // to_num / box.f は値の恒等写像として辿る。
                    if ((xn == "xyo.to_num" || xn == "xyo.box.f") &&
                        !x.op->operands.empty())
                        return assumeConstBound(x.op->operands[0], pred, c,
                                                condTrue, m, depth + 1);
                    if (xn == "xyo.getvar") {
                        std::string id(x.op->attrStr("id"));
                        if (!active.count(id)) return false;
                        auto it = m.find(id);
                        if (it == m.end()) return false;
                        int64_t lo = it->second.first;
                        int64_t hi = it->second.second;
                        // 仮定の上界（head 非依存）。widening のジャンプ先
                        // 候補にする。有効性は不動点の収束判定が保証する
                        // （外れ値は union で戻り、収束しなければ従来の
                        // 閾値ジャンプへフォールバックする）。
                        const int64_t INF = INT64_MAX;
                        int64_t capHi = INF;
                        // pred: 0=LT 1=GT 2=LE 3=GE 4=EQ（NE は非凸で不可）。
                        if (pred == 0) {
                            if (condTrue) {
                                hi = std::min(hi, c - 1);
                                capHi = c - 1;
                            } else
                                lo = std::max(lo, c);
                        } else if (pred == 1) {
                            if (condTrue)
                                lo = std::max(lo, c + 1);
                            else {
                                hi = std::min(hi, c);
                                capHi = c;
                            }
                        } else if (pred == 2) {
                            if (condTrue) {
                                hi = std::min(hi, c);
                                capHi = c;
                            } else
                                lo = std::max(lo, c + 1);
                        } else if (pred == 3) {
                            if (condTrue)
                                lo = std::max(lo, c);
                            else {
                                hi = std::min(hi, c - 1);
                                capHi = c - 1;
                            }
                        } else if (pred == 4) {
                            if (!condTrue) return false;
                            lo = hi = c;
                            capHi = c;
                        } else {
                            return false;
                        }
                        // 空にすると到達不能分岐。union 側の過近似を保つ
                        // ため絞らない（精度は落ちるが健全）。
                        if (lo > hi) return false;
                        // i32 を溢れる絞り込みは落下判定に任せる（ここでは
                        // 現状維持＝過近似）。
                        if (lo < INT32_MIN || hi > INT32_MAX) return false;
                        it->second = {lo, hi};
                        if (capHi != INF) {
                            auto nc = narrowCapHi.find(id);
                            if (nc == narrowCapHi.end())
                                narrowCapHi[id] = capHi;
                            else if (capHi < nc->second)
                                nc->second = capHi;
                        }
                        return true;
                    }
                    // add/sub(定数) は逆向きに辿る（(i+1)>1000 → i>999）。
                    if ((xn == "xyo.add" || xn == "xyo.sub" ||
                         xn == "xyo.add.i" || xn == "xyo.sub.i") &&
                        x.op->operands.size() == 2) {
                        int64_t k = 0;
                        const Value* base = nullptr;
                        if (intConstValue(x.op->operands[1], &k)) {
                            base = &x.op->operands[0];
                        } else if ((xn == "xyo.add" || xn == "xyo.add.i") &&
                                   intConstValue(x.op->operands[0], &k)) {
                            base = &x.op->operands[1];
                        } else {
                            return false;
                        }
                        const bool isSub =
                            xn == "xyo.sub" || xn == "xyo.sub.i";
                        __int128 nc = isSub ? (__int128)c + k
                                            : (__int128)c - k;
                        if (nc < INT32_MIN || nc > INT32_MAX) return false;
                        return assumeConstBound(*base, pred, (int64_t)nc,
                                                condTrue, m, depth + 1);
                    }
                }
                return false;
            };
            // if の条件式から (pred, var側, 定数側, 極性) を取り出す。
            // bnot/not は極性反転して辿る。成功時は assumeConstBound で
            // 分岐側の区間を絞る。
            // `eq(mod(V,K),0)` 形から V の K 倍数性を取り出す
            // （K==2 なら偶数）。box.f/to_num を透過し、両順序に対応。
            // 成功時は真側の評価用に evenVars_ へ登録する。
            std::function<bool(Op*, bool)> assumeEven =
                [&](Op* condOp, bool condTrue) -> bool {
                if (!condOp) return false;
                const std::string& n = condOp->name;
                int pred = -1;
                std::vector<Value> ops;
                if (n == "xyo.eq") {
                    pred = 4;
                    ops = condOp->operands;
                } else if ((n == "xyo.cmp.i" || n == "xyo.cmp.f" ||
                            n == "xyo.cmp.guarded") &&
                           condOp->attrInt("pred", -1) == 4) {
                    pred = 4;
                    if (n == "xyo.cmp.guarded" &&
                        condOp->operands.size() == 4)
                        ops = {condOp->operands[2],
                               condOp->operands[3]};
                    else if (condOp->operands.size() >= 2)
                        ops = {condOp->operands[0],
                               condOp->operands[1]};
                    else
                        return false;
                } else {
                    return false;
                }
                if (ops.size() != 2 || !condTrue) return false;
                // mod 側と 0 側を特定する。
                const Value* modV = nullptr;
                const Value* zeroV = nullptr;
                auto isZeroConst = [&](const Value& v) -> bool {
                    int64_t z = 0;
                    if (intConstValue(v, &z) && z == 0) return true;
                    if (v.op && v.op->name == "xyo.const.f64") {
                        const Attr* a = v.op->attr("d");
                        return a && a->isDouble() && a->d == 0.0;
                    }
                    return false;
                };
                auto asMod = [&](const Value& v) -> Op* {
                    Op* o = v.op;
                    if (!o) return nullptr;
                    if ((o->name == "xyo.box.f" ||
                         o->name == "xyo.to_num") &&
                        !o->operands.empty())
                        o = o->operands[0].op;
                    if (!o) return nullptr;
                    if (o->name != "xyo.mod" && o->name != "xyo.mod.i" &&
                        o->name != "xyo.mod.f")
                        return nullptr;
                    if (o->operands.size() != 2) return nullptr;
                    int64_t kk = 0;
                    const Value& dv = o->operands[1];
                    if (dv.op && dv.op->name == "xyo.const.i") {
                        const Attr* a = dv.op->attr("v");
                        if (!a || !a->isInt()) return nullptr;
                        kk = a->i;
                        if (kk == 0) return nullptr;
                    } else if (!intConstValue(dv, &kk) || kk == 0) {
                        return nullptr;
                    }
                    // 偶数性だけを追う（K==2）。一般の K は div 側が
                    // 対応していない。
                    if (kk != 2 && kk != -2) return nullptr;
                    return o;
                };
                Op* mo = nullptr;
                if (isZeroConst(ops[1])) {
                    mo = asMod(ops[0]);
                } else if (isZeroConst(ops[0])) {
                    mo = asMod(ops[1]);
                }
                if (!mo || mo->operands.empty()) return false;
                // 被除数が整数候補の getvar なら偶数とみなす（非 NaN は
                // 整数証明の帰結として不動点の健全性で担保）。
                Op* base = mo->operands[0].op;
                if (!base || base->name != "xyo.getvar") return false;
                std::string id(base->attrStr("id"));
                if (!active.count(id)) return false;
                evenVars_.insert(id);
                if (intDebug()) {
                    std::fprintf(stderr, "[mlir-int] even %s\n",
                                 id.c_str());
                }
                return true;
            };
            std::function<void(Op*, bool, IvMap&)> assumeBranch =
                [&](Op* condOp, bool condTrue, IvMap& m) {
                    if (!condOp) return;
                    // 偶数性の記録（真側のみ）。区間の絞り込みとは独立。
                    if (condOp->name != "xyo.bnot" &&
                        condOp->name != "xyo.not")
                        assumeEven(condOp, condTrue);
                    const std::string& n = condOp->name;
                    if (n == "xyo.bnot" || n == "xyo.not") {
                        if (!condOp->operands.empty() &&
                            condOp->operands[0].op)
                            assumeBranch(condOp->operands[0].op, !condTrue,
                                         m);
                        return;
                    }
                    int pred = -1;
                    std::vector<Value> ops;
                    if (n == "xyo.cmp.i" || n == "xyo.cmp.f" ||
                        n == "xyo.cmp.guarded") {
                        pred = (int)condOp->attrInt("pred", -1);
                        // cmp.guarded は (bits, bits, f64, f64)。f64 側は
                        // const.f64 との比較、bits 側は const との比較で
                        // 取り出す（どちらか片方で足りる）。
                        if (n == "xyo.cmp.guarded" &&
                            condOp->operands.size() == 4) {
                            const Value& ad = condOp->operands[2];
                            const Value& cd = condOp->operands[3];
                            int64_t cf = 0;
                            auto asInt = [&](const Value& v,
                                             int64_t* o) -> bool {
                                if (!v.op) return false;
                                if (v.op->name == "xyo.const.f64") {
                                    const Attr* a = v.op->attr("d");
                                    if (!a || !a->isDouble()) return false;
                                    double d = a->d;
                                    if (!(d >= -2147483648.0 &&
                                          d <= 2147483647.0))
                                        return false;
                                    // 整数値でなければ絞れない。
                                    if (std::trunc(d) != d) return false;
                                    *o = (int64_t)d;
                                    return true;
                                }
                                return intConstValue(v, o);
                            };
                            if (asInt(cd, &cf)) {
                                assumeConstBound(ad, pred, cf, condTrue, m,
                                                0);
                                return;
                            }
                            if (asInt(ad, &cf)) {
                                // 左右反転: pred を反対向きに読み替える。
                                int rp = pred;
                                if (pred == 0) rp = 2;
                                else if (pred == 1) rp = 3;
                                else if (pred == 2) rp = 0;
                                else if (pred == 3) rp = 1;
                                assumeConstBound(cd, rp, cf, condTrue, m,
                                                0);
                                return;
                            }
                        }
                        if (condOp->operands.size() >= 2)
                            ops = {condOp->operands[0],
                                   condOp->operands[1]};
                        else
                            return;
                    } else if (n == "xyo.lt" || n == "xyo.gt" ||
                               n == "xyo.eq") {
                        pred = n == "xyo.lt" ? 0 : n == "xyo.gt" ? 1 : 4;
                        ops = condOp->operands;
                    } else {
                        return;
                    }
                    if (ops.size() != 2 || pred < 0 || pred > 4) return;
                    int64_t c = 0;
                    // f64 定数は整数値のときだけ使う（cmp.f の定数側）。
                    auto constInt = [&](const Value& v,
                                        int64_t* o) -> bool {
                        if (!v.op) return false;
                        if (v.op->name == "xyo.const.f64") {
                            const Attr* a = v.op->attr("d");
                            if (!a || !a->isDouble()) return false;
                            double d = a->d;
                            if (!(d >= -2147483648.0 && d <= 2147483647.0))
                                return false;
                            // cmp.f の定数は [<|>|<=|>=] をそのまま整数に
                            // 読める必要がある。非整数は絞らない。
                            // LT/GT: X<C ⟺ X<=ceil(C)-1、X>C ⟺ X>=floor(C)+1。
                            // LE/GE: X<=C ⟺ X<=floor(C)、X>=C ⟺ X>=ceil(C)。
                            double bnd = d;
                            if (n == "xyo.cmp.f" || n == "xyo.cmp.guarded") {
                                if (pred == 0 || pred == 1) {
                                    bnd = pred == 0 ? std::ceil(d) - 1
                                                    : std::floor(d) + 1;
                                    // 変換後は LE/GE として扱う。
                                    pred = pred == 0 ? 2 : 3;
                                } else {
                                    bnd = pred == 2 ? std::floor(d)
                                                    : std::ceil(d);
                                }
                                if (!(bnd >= -2147483648.0 &&
                                      bnd <= 2147483647.0))
                                    return false;
                            } else if (std::trunc(d) != d) {
                                return false;
                            }
                            *o = (int64_t)bnd;
                            return true;
                        }
                        return intConstValue(v, o);
                    };
                    if (constInt(ops[1], &c)) {
                        assumeConstBound(ops[0], pred, c, condTrue, m, 0);
                        return;
                    }
                    if (constInt(ops[0], &c)) {
                        int rp = pred;
                        if (pred == 0) rp = 2;
                        else if (pred == 1) rp = 3;
                        else if (pred == 2) rp = 0;
                        else if (pred == 3) rp = 1;
                        assumeConstBound(ops[1], rp, c, condTrue, m, 0);
                        return;
                    }
                };
            std::function<bool(Block&)> evalBlock = [&](Block& blk) -> bool {
                for (Op* op : blk.ops) {
                    if (op->name == "xyo.setvar") {
                        std::string id(op->attrStr("id"));
                        if (!active.count(id) || op->operands.empty())
                            continue;
                        IntVal r = evalIntValue(op->operands[0], iv, memo);
                        // 右辺の評価後に偶数性は失われる（3n+1 は反転）。
                        // 右辺自体の評価（div の被除数など）には
                        // 分岐の仮定が有効でなければならない。
                        evenVars_.erase(id);
                        // 証明できない書き込み = その変数だけ整数化を諦める。
                        // 加えて、オペランドが「必ず数値 Value を返す」証明
                        // が無い書き込みも落とす — int 輸送は store を
                        // from_int（num）にするので、整数として読める文字列
                        // （item of list が s:"0" を返す等）を書き込むと
                        // 表現が変わり interp とずれる（found: truthRun）。
                        if (!r.ok || r.lo < INT32_MIN || r.hi > INT32_MAX ||
                            !exactIntOperand(op->operands[0], iv)) {
                            active.erase(id);
                            iv.erase(id);
                            changed = true;
                            continue;
                        }
                        iv[id] = {r.lo, r.hi};
                        continue;
                    }
                    if (op->name == "xyo.if") {
                        if (op->regions.size() != 2) return false;
                        const IvMap pre = iv;
                        const auto preEven = evenVars_;
                        memo.clear();
                        if (!op->operands.empty() && op->operands[0].op)
                            assumeBranch(op->operands[0].op,
                                         /*condTrue=*/true, iv);
                        if (!evalBlock(*op->region(0)->entry())) return false;
                        const IvMap thenIv = iv;
                        iv = pre;
                        evenVars_ = preEven;
                        memo.clear();
                        if (!op->operands.empty() && op->operands[0].op)
                            assumeBranch(op->operands[0].op,
                                         /*condTrue=*/false, iv);
                        if (!evalBlock(*op->region(1)->entry())) return false;
                        evenVars_ = preEven;
                        for (auto& [id, v] : iv) {
                            auto it = thenIv.find(id);
                            if (it == thenIv.end()) continue;
                            v.first = std::min(v.first, it->second.first);
                            v.second = std::max(v.second, it->second.second);
                        }
                        memo.clear();
                        continue;
                    }
                    bool isNested = false;
                    for (Op* no : nestedOps_)
                        if (no == op) isNested = true;
                    if (isNested) {
                        // 入れ子ループが書く変数は、抜けた後の値が不明。
                        // 整数化の対象から落とす（読むだけの変数は残る）。
                        for (const std::string& vid : nestedWrites_) {
                            if (!active.count(vid)) continue;
                            active.erase(vid);
                            iv.erase(vid);
                            changed = true;
                        }
                        continue;
                    }
                    if (dialect::isRegionOp(op->name)) return false;
                }
                return true;
            };
            if (!evalBlock(body)) {
                intTrace("body proof failed round %s", std::to_string(round));
                return false;
            }
            // head の更新は backedge 値（最終書き込み）との和集合。
            // 中間値は含めない。
            for (auto& [id, c] : cands) {
                if (!active.count(id)) continue;
                auto& head = headIv[id];
                auto iit = iv.find(id);
                if (iit == iv.end()) continue;
                int64_t lo = std::min(head.first, iit->second.first);
                int64_t hi = std::max(head.second, iit->second.second);
                // 上界を先に交差させる（これをしないと、締めた区間へ
                // 戻すたびに「変化した」と判定されて収束しない）。
                int64_t bl = 0, bh = 0;
                if (headBoundOf(id, trips, iv, &bl, &bh)) {
                    lo = std::max(lo, bl);
                    hi = std::min(hi, bh);
                    if (lo > hi) {
                        lo = std::min(head.first, iit->second.first);
                        hi = std::max(head.second, iit->second.second);
                    }
                }
                // ボックス強制版: head もボックスへ交差させる（これを
                // しないと本体側の union で毎ラウンド「変化した」になり
                // 収束しない）。
                if (clampToBox_ && guardEntries_) {
                    auto g = guardEntries_->find(id);
                    if (g != guardEntries_->end()) {
                        lo = std::max(lo, g->second.first);
                        hi = std::min(hi, g->second.second);
                        if (lo > hi) lo = hi;
                    }
                }
                if (hi > head.second) {
                    if (++growth[id] >= 3) {
                        int64_t t = INT32_MAX;
                        for (int64_t th : thresholds) {
                            if (th >= hi && th < t) t = th;
                        }
                        // 条件キャップが現在値を覆うなら閾値より手前へ
                        // ジャンプする（wrap カウンタの早期収束）。
                        // 覆わない場合は消費して従来の閾値へ。
                        auto nc = narrowCapHi.find(id);
                        if (nc != narrowCapHi.end()) {
                            if (nc->second >= hi &&
                                nc->second < t)
                                t = nc->second;
                            narrowCapHi.erase(nc);
                        }
                        hi = t;
                        growth[id] = 0;
                    }
                } else {
                    growth[id] = 0;
                }
                if (lo < INT32_MIN || hi > INT32_MAX) {
                    active.erase(id);
                    changed = true;
                    continue;
                }
                if (lo != head.first || hi != head.second) {
                    head = {lo, hi};
                    changed = true;
                }
            }
            clampHeadIntervals(headIv, active);
            if (active.empty()) {
                intTrace("all int vars dropped (%s)", std::string());
                return false;
            }
            if (intDebug() && (round < 6 || round % 10 == 0)) {
                std::string s;
                for (auto& [id, r] : headIv)
                    s += id + "=[" + std::to_string(r.first) + "," +
                         std::to_string(r.second) + "] ";
                std::fprintf(stderr, "[mlir-int] round %d %s\n", round,
                             s.c_str());
            }
            if (!changed) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            intTrace("int intervals did not converge", std::string());
            return false;
        }
        // ループ内の「非整数になり得る書き込み」を数え、プロジェクト全体の
        // 同数（frontend が数える）と一致する変数だけを採用する。
        // 整数式（リテラル・四則・mod・round・整数変数読み）は数えない。
        // ガード変数は例外: 同一関数（同一スレッド・逐次実行）の外書きは
        // 入口ガードが値を検証するので許す。他関数（他スレッドが yield を
        // 跨いで書ける）は従来どおり一致を要求する。
        std::unordered_map<std::string, int> insideNonLiteral;
        std::unordered_set<Op*> insideOps;
        walkIntBody(body, [&](Op* op) {
            insideOps.insert(op);
            if (op->name != "xyo.setvar" || op->operands.empty()) return;
            std::string id(op->attrStr("id"));
            if (!active.count(id)) return;
            if (!intExprProvable(op->operands[0], active))
                insideNonLiteral[id]++;
        });
        // 同一関数の本体外書き込み（ガード変数の入口設定など）。
        // singleSpawn のスクリプト関数に限り有効。手続き本体や多重起動
        // し得るハットでは他スレッドが yield を跨いで書けるため、従来の
        // 厳密な一致を要求する（sameFuncOutside は空のまま）。
        // 計数は frontend の ext と同じ基準（clean 集合）で行う。解析基準
        // （active）で数えると、clean な入口設定（例: y = getvar_x）が
        // ext 側で数えられず二重に効いて不一致になる。
        std::unordered_map<std::string, int> sameFuncOutside;
        if (guardEntries && m.singleSpawn) {
            Block* fb = &parent;
            while (fb) {
                Region* r = fb->parent;
                if (!r || !r->owner) break;
                Op* o = r->owner;
                if (o->name == "func.func") {
                    if (o->hasAttr("proc")) break;  // 手続き本体は除外。
                    std::function<void(Block&)> walkFn = [&](Block& blk) {
                        for (Op* w : blk.ops) {
                            if (w->name == "xyo.setvar" &&
                                !w->operands.empty() &&
                                !insideOps.count(w)) {
                                std::string id(w->attrStr("id"));
                                if (active.count(id) &&
                                    !cleanProvable(w->operands[0]))
                                    sameFuncOutside[id]++;
                            }
                            for (auto& rr : w->regions) {
                                if (!rr) continue;
                                for (auto& bb : rr->blocks) walkFn(*bb);
                            }
                        }
                    };
                    for (auto& fr : o->regions)
                        if (fr)
                            for (auto& fb2 : fr->blocks) walkFn(*fb2);
                    break;
                }
                fb = o->parent;
            }
        }
        for (auto& [id, c] : cands) {
            if (!active.count(id)) continue;
            auto total = m.intNonLiteralWrites.find(id);
            int n = total == m.intNonLiteralWrites.end() ? 0 : total->second;
            if (intDebug()) {
                std::fprintf(stderr,
                             "[mlir-int] cand %s entry=%lld range=[%lld,%lld] ext=%d inbody=%d samefn=%d\n",
                             id.c_str(), (long long)c.intEntry,
                             (long long)headIv[id].first,
                             (long long)headIv[id].second,
                             n, insideNonLiteral[id], sameFuncOutside[id]);
            }
            if (relaxClean_) {
                // ボックス版: ループ内の書き込みだけ整数式であればよい。
                // 入口は int_exact_ok、各反復の頭は inrange、yield 後は
                // reload 検証が守る。div は intExprProvable に入らないが、
                // 偶数証明で整数化できたもの（divRanges_ 記録済み）は許す。
                bool badWrite = false;
                if (insideNonLiteral[id] != 0) {
                    walkIntBody(body, [&](Op* op) {
                        if (badWrite || op->name != "xyo.setvar" ||
                            op->operands.empty())
                            return;
                        if (std::string(op->attrStr("id")) != id) return;
                        if (intExprProvable(op->operands[0], active))
                            return;
                        Op* wop = op->operands[0].op;
                        if (wop && wop->name == "xyo.div" &&
                            divRanges_.count(wop))
                            return;
                        badWrite = true;
                    });
                }
                if (badWrite) {
                    intTrace("box: non-literal write in body: %s", id);
                    active.erase(id);
                }
                continue;
            }
            int own = insideNonLiteral[id];
            if (c.intGuarded) own += sameFuncOutside[id];
            if (n != own) {
                intTrace("count mismatch: %s", id);
                active.erase(id);
            }
        }
        if (active.empty()) return false;
        // 最終区間で各 mod の被除数の区間を確定する。
        // 本体内の書き込み順に辿る Gauss-Seidel 評価（中間値込み）。
        // subk: -1 srem+補正、-2 urem、0 以上なら sub+select をその回数。
        // if を含む本体はこの走査が条件付き書き込みを無条件として扱うため、
        // 被除数区間が狭く出る可能性がある。sub-chain は使わず srem に落とす
        // （srem は常に正しい）。
        bool bodyHasIf = false;
        walkIntBody(body, [&](Op* op) {
            if (op->name == "xyo.if") bodyHasIf = true;
        });
        if (!bodyHasIf) {
            std::unordered_map<std::string, std::pair<int64_t, int64_t>> iv;
            for (const std::string& id : active)
                iv[id] = {headIv[id].first, headIv[id].second};
            std::unordered_map<Op*, IntVal> memo;
            bool finalBad = false;
            walkIntBody(body, [&](Op* op) {
                if (finalBad) return;
                if (op->name == "xyo.setvar") {
                    std::string id(op->attrStr("id"));
                    if (!active.count(id) || op->operands.empty()) return;
                    IntVal r = evalIntValue(op->operands[0], iv, memo);
                    if (!r.ok) {
                        finalBad = true;
                        return;
                    }
                    iv[id] = {r.lo, r.hi};
                    return;
                }
                if (op->name != "xyo.mod" || op->operands.size() != 2)
                    return;
                int64_t mm = 0;
                if (!intConstValue(op->operands[1], &mm) || mm <= 0 ||
                    mm > INT32_MAX) {
                    modSubK[op] = -1;
                    return;
                }
                IntVal ra = evalIntValue(op->operands[0], iv, memo);
                if (!ra.ok || ra.lo < 0) {
                    modSubK[op] = -1;
                    return;
                }
                // [0, hi] が [(k)*m, (k+1)*m) に収まる最小の k（最大3）。
                int k = -2;
                __int128 bound = (__int128)mm;
                for (int t = 0; t <= 3; ++t, bound += mm) {
                    if ((__int128)ra.hi < bound) {
                        k = t;
                        break;
                    }
                }
                modSubK[op] = k;
            });
            if (finalBad) modSubK.clear();
        }
        // 採用した変数に区間を確定する。
        for (auto& [id, c] : cands) {
            if (!active.count(id)) continue;
            c.isInt = true;
            c.intLo = headIv[id].first;
            c.intHi = headIv[id].second;
        }
        recordIntOps();
        intTrace("int loop: %s", std::to_string(active.size()) + " vars");
        return true;
    }

    /// 書き換え時の整数判定（解析と同じ evalInt を最終区間で再評価）。
    IntVal intOkOf(const Value& v) {
        // 証明セットは「書き換えで .i に改名済みの op」を引くためのもの。
        // 別の op（box.f 等）へ改名された後にヒットすると型が合わないので、
        // 現在の名前が整数 op のときだけ信用する。
        auto intNamed = [](const std::string& n) {
            if (n == "xyo.const.i" || n == "xyo.to_int" ||
                n == "xyo.from_int")
                return true;
            if (n.size() > 2 && n.compare(n.size() - 2, 2, ".i") == 0)
                return true;
            // まだ .i へ改名していない整数 op。集合には「最終区間で整数と
            // 評価できた op」しか入らないので信用してよい。改名済みの
            // box.f / *.f はここに来ない（stale 対策）。
            return n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
                   n == "xyo.mod" || n == "xyo.div";
        };
        // div の精密区間を優先（intProvenOps は全域を返すため、
        // 下流のオーバーフロー判定が落ちることがある）。
        if (v.op) {
            auto dit = divRanges_.find(v.op);
            if (dit != divRanges_.end())
                return intRange(dit->second.first, dit->second.second,
                                true);
        }
        if (v.op && intProvenOps.count(v.op) && intNamed(v.op->name) &&
            v.op->name != "xyo.list.item" &&
            v.op->name != "xyo.list.item.i")
            return intRange(INT32_MIN, INT32_MAX, true);
        // list.item は exact range を返すため常に再評価する（集合ヒットは
        // 全域を返すため、積算等の下流が発散して落ちる）。
        std::unordered_map<std::string, std::pair<int64_t, int64_t>> iv;
        for (auto& [id, c] : cands)
            if (c.isInt) iv[id] = {c.intLo, c.intHi};
        std::unordered_map<Op*, IntVal> memo;
        return evalIntValue(v, iv, memo);
    }

    static bool intDebug() {
        static const bool on = [] {
            const char* e = std::getenv("XYO_MLIR_INTDBG");
            return e && e[0] == '1';
        }();
        return on;
    }
    static void intTrace(const char* fmt, const std::string& a,
                         const std::string& b = {}) {
        if (!intDebug()) return;
        std::fprintf(stderr, "[mlir-int] ");
        std::fprintf(stderr, fmt, a.c_str(), b.c_str());
        std::fprintf(stderr, "\n");
    }

    /// 1 つのブロック（ループ本体または if の substack）を書き換える。
    void rewriteBlock(Block& b) {
        std::vector<Op*> out;
        for (Op* op : b.ops) {
            const std::string& n = op->name;
            if (n == "xyo.getvar") {
                auto it = cands.find(std::string(op->attrStr("id")));
                if (it != cands.end()) {
                    getvarOf[op] = it->first;  // 以降の use で bits/d に置換
                    continue;
                }
                push(b, out, op);
                continue;
            }
            if (n == "xyo.setvar") {
                auto it = cands.find(std::string(op->attrStr("id")));
                if (it == cands.end() || op->operands.empty()) {
                    push(b, out, op);
                    continue;
                }
                LoopCandidate& c = it->second;
                if (intLoop && c.isInt) {
                    // 整数ループ: SSA 値を運ぶだけで実メモリに触れない。
                    // iOf が空を返したら空のまま伝播させ、verifier が
                    // 落として既存 JIT へフォールバックする（黙った誤りなし）。
                    c.iv = iOf(op->operands[0], b, out);
                    continue;
                }
                Value x = op->operands[0];
                if (kindOf(c.id) == 'd') {
                    // f64 だけを持ち回る。ビットは必要になった時点で
                    // bitsOf が box.f で作る。
                    c.d = dOf(x, b, out);
                    c.bits = {};
                    continue;
                }
                Value newBits = bitsOf(x, b, out);
                Value newD = dOf(x, b, out);
                c.bits = newBits;
                c.d = newD;
                continue;
            }
            if (n == "xyo.loop_yield") {
                // warp ループは 64 回に 1 回だけ実際に譲るので、spill も
                // その分岐の中だけに置く（xyo.warp_yield）。
                if (warpLoop) {
                    // すべての書き込みが数値と分かるループでは、ビット影を
                    // 持ち回らず d だけを spill/reload する（本体が
                    // add.f/mod.f だけになり LLVM が最適化しやすい）。
                    // 整数ループは i64 だけを持ち回る。
                    std::string vars, kinds;
                    std::vector<Value> operands;
                    std::vector<Type> results;
                    for (size_t i = 0; i < order.size(); ++i) {
                        LoopCandidate& c = cands[order[i]];
                        if (i) vars += '\x1e';
                        vars += c.id;
                        vars += '\x1f';
                        vars += c.name;
                        // 変数ごとの運搬表現: i=i32, d=f64 のみ, b=(bits,f64)。
                        const char kd = kindOf(c.id);
                        if (kd == 'i') {
                            operands.push_back(c.iv);
                            results.push_back(Type::i32());
                            kinds += 'i';
                        } else if (kd == 'd') {
                            operands.push_back(c.d);
                            results.push_back(Type::f64());
                            kinds += 'd';
                        } else {
                            operands.push_back(c.bits);
                            operands.push_back(c.d);
                            results.push_back(Type::value());
                            results.push_back(Type::f64());
                            kinds += 'b';
                        }
                    }
                    AttrMap wyAttrs{{"vars", Attr::str(std::move(vars))},
                                    {"kinds", Attr::str(std::move(kinds))}};
                    if (boxLoop_) wyAttrs["box"] = Attr::integer(1);
                    Op* wy = make("xyo.warp_yield", std::move(operands),
                                  std::move(results), std::move(wyAttrs));
                    push(b, out, wy);
                    size_t roff = 0;
                    for (size_t i = 0; i < order.size(); ++i) {
                        LoopCandidate& c = cands[order[i]];
                        if (kindOf(c.id) == 'i') {
                            c.iv = wy->result(roff++);
                        } else if (kindOf(c.id) == 'd') {
                            c.d = wy->result(roff++);
                            c.bits = {};
                        } else {
                            c.bits = wy->result(roff);
                            c.d = wy->result(roff + 1);
                            roff += 2;
                        }
                    }
                    continue;
                }
                // 非 warp: 毎反復譲るので、その都度 spill して読み直す。
                // 整数ループは from_int で spill し to_int で戻す。
                for (const std::string& id : order) {
                    LoopCandidate& c = cands[id];
                    Value sv = c.bits;
                    if (intLoop && c.isInt) sv = intBitsOf(c.iv, b, out);
                    Op* st = make("xyo.setvar", {sv}, {},
                                  {{"id", Attr::str(c.id)},
                                   {"name", Attr::str(c.name)}});
                    push(b, out, st);
                }
                push(b, out, op);
                for (const std::string& id : order) {
                    LoopCandidate& c = cands[id];
                    Op* g = make("xyo.getvar", {}, {Type::value()},
                                 {{"id", Attr::str(c.id)},
                                  {"name", Attr::str(c.name)}});
                    push(b, out, g);
                    if (intLoop && c.isInt) {
                        Op* t = make("xyo.to_int", {g->result(0)},
                                     {Type::i32()});
                        push(b, out, t);
                        c.iv = t->result(0);
                        continue;
                    }
                    Op* t = make("xyo.to_num", {g->result(0)}, {Type::f64()});
                    push(b, out, t);
                    c.bits = g->result(0);
                    c.d = t->result(0);
                }
                continue;
            }
            if (!nestedOps_.empty() && isNestedOp(op)) {
                // 昇格前の入れ子ループ: 昇格値を実メモリへ spill してから
                // 入り、抜けたら読み直す。入れ子の中の中断点では、メモリが
                // 「入る直前の値（または入れ子自身が書いた値）」になって
                // いるので、他のスクリプトから見える値は変わらない。
                // repeat の回数など、Value を取るオペランドが整数 SSA に
                // 書き換わっている場合は box し直す（型不一致は verifier
                // ではなく ISel で落ちる）。
                {
                    std::vector<Value> ops = op->operands;
                    bool touched = false;
                    for (size_t i = 0; i < ops.size(); ++i) {
                        if (ops[i].op && getvarOf.count(ops[i].op)) {
                            ops[i] = bitsOf(ops[i], b, out);
                            touched = true;
                        } else if (curType(ops[i]) == Type::i32() ||
                                   curType(ops[i]) == Type::i64()) {
                            ops[i] = bitsOf(ops[i], b, out);
                            touched = true;
                        }
                    }
                    if (touched) op->operands = std::move(ops);
                }
                for (const std::string& id : order) {
                    LoopCandidate& c = cands[id];
                    Value sv = c.bits;
                    if (intLoop && c.isInt) sv = intBitsOf(c.iv, b, out);
                    Op* st = make("xyo.setvar", {sv}, {},
                                  {{"id", Attr::str(c.id)},
                                   {"name", Attr::str(c.name)}});
                    push(b, out, st);
                }
                push(b, out, op);
                for (const std::string& id : order) {
                    LoopCandidate& c = cands[id];
                    Op* g = make("xyo.getvar", {}, {Type::value()},
                                 {{"id", Attr::str(c.id)},
                                  {"name", Attr::str(c.name)}});
                    push(b, out, g);
                    if (intLoop && c.isInt) {
                        Op* t = make("xyo.to_int", {g->result(0)},
                                     {Type::i32()});
                        push(b, out, t);
                        c.iv = t->result(0);
                        continue;
                    }
                    Op* t = make("xyo.to_num", {g->result(0)}, {Type::f64()});
                    push(b, out, t);
                    c.bits = g->result(0);
                    c.d = t->result(0);
                }
                continue;
            }
            if (n == "xyo.condition" && condTerm) {
                // while の cond 領域: 条件に続けて現在値を forward する。
                for (const std::string& id : order) {
                    if (kindOf(id) == 'i') {
                        op->operands.push_back(cands[id].iv);
                        continue;
                    }
                    if (kindOf(id) == 'd') {
                        op->operands.push_back(cands[id].d);
                        continue;
                    }
                    op->operands.push_back(cands[id].bits);
                    op->operands.push_back(cands[id].d);
                }
                push(b, out, op);
                continue;
            }
            if (n == "xyo.yield") {
                // ループ/if の結果として昇格変数の現在値を返す。
                // if の substack では、その if が代入する変数だけを載せる。
                for (const std::string& id : order) {
                    if (ifAssigned_ && !ifAssigned_->count(id)) continue;
                    if (kindOf(id) == 'i') {
                        op->operands.push_back(cands[id].iv);
                        continue;
                    }
                    if (kindOf(id) == 'd') {
                        op->operands.push_back(cands[id].d);
                        continue;
                    }
                    op->operands.push_back(cands[id].bits);
                    op->operands.push_back(cands[id].d);
                }
                push(b, out, op);
                continue;
            }
            if (n == "xyo.if" && !intLoop && floatIfSelectable(op)) {
                // 倍精度影のループでも、両分岐が「候補への単純代入のみ」
                // なら select にできる（分岐を消して LLVM に最適化させる）。
                Value cond = op->operands.empty() ? Value{} : op->operands[0];
                bool okSel = static_cast<bool>(cond);
                // 途中で失敗しても IR を汚さないよう、生成した op は
                // 一時配列に溜めて成功時だけ確定する（失敗＝従来の
                // region 形式の if にフォールバック）。
                std::vector<Op*> tmp;
                struct Shadow2 {
                    Value bits, d;
                };
                std::map<std::string, Shadow2> pre, thenV, elseV;
                for (auto& [id, c] : cands) pre[id] = {c.bits, c.d};
                auto evalRegion = [&](Region* r) -> bool {
                    for (Op* o : r->entry()->ops) {
                        if (o->name == "xyo.yield") continue;
                        if (o->name != "xyo.setvar" || o->operands.empty())
                            return false;
                        const std::string id(o->attrStr("id"));
                        if (!cands.count(id)) return false;
                        LoopCandidate& c = cands[id];
                        const bool dOnly = kindOf(id) == 'd';
                        Value nb = dOnly ? Value{}
                                         : bitsOf(o->operands[0], b, tmp);
                        Value nd = dOf(o->operands[0], b, tmp);
                        if ((!dOnly && !nb) || !nd) return false;
                        c.bits = nb;
                        c.d = nd;
                    }
                    return true;
                };
                if (okSel) {
                    okSel = evalRegion(op->region(0));
                    for (auto& [id, c] : cands) thenV[id] = {c.bits, c.d};
                    for (auto& [id, c] : cands) {
                        c.bits = pre[id].bits;
                        c.d = pre[id].d;
                    }
                    if (okSel) {
                        okSel = evalRegion(op->region(1));
                        for (auto& [id, c] : cands) elseV[id] = {c.bits, c.d};
                    }
                }
                if (!okSel) {
                    // 失敗: 影を入口へ戻して従来経路（region 形式）へ。
                    for (auto& [id, c] : cands) {
                        c.bits = pre[id].bits;
                        c.d = pre[id].d;
                    }
                } else {
                for (Op* o : tmp) out.push_back(o);
                for (auto& [id, c] : cands) {
                    auto& t = thenV[id];
                    auto& e = elseV[id];
                    auto same = [](const Value& x, const Value& y) {
                        return x.op == y.op && x.block == y.block &&
                               x.argIndex == y.argIndex;
                    };
                    if (kindOf(id) == 'd') {
                        c.bits = {};
                    } else if (!same(t.bits, e.bits)) {
                        Op* sb = make("xyo.select", {cond, t.bits, e.bits},
                                      {Type::value()});
                        push(b, out, sb);
                        c.bits = sb->result(0);
                    } else {
                        c.bits = t.bits;
                    }
                    if (!same(t.d, e.d)) {
                        Op* sd = make("xyo.select", {cond, t.d, e.d},
                                      {Type::f64()});
                        push(b, out, sd);
                        c.d = sd->result(0);
                    } else {
                        c.d = t.d;
                    }
                }
                continue;
                }
            }
            if (n == "xyo.if" && intLoop && intIfSelectable(op) &&
                [&] {
                    for (size_t r = 0; r < op->regions.size(); ++r) {
                        if (!op->region(r)) continue;
                        for (Op* o : op->region(r)->entry()->ops) {
                            if (o->name != "xyo.setvar" ||
                                o->operands.empty())
                                continue;
                            auto it2 = cands.find(
                                std::string(o->attrStr("id")));
                            if (it2 == cands.end() || !it2->second.isInt)
                                return false;
                        }
                    }
                    return true;
                }()) {
                // 整数ループ: 両分岐の値を SSA で作り、select で統合する。
                // 分岐が消えるので LLVM がループを最適化しやすい
                // （legacy の rep.region と同じ形）。
                Value cond = op->operands.empty() ? Value{} : op->operands[0];
                bool okSel = static_cast<bool>(cond);
                const size_t outMark = out.size();
                std::map<std::string, Value> pre, thenVals, elseVals;
                for (auto& [id, c] : cands) pre[id] = c.iv;
                auto evalRegion = [&](Region* r) -> bool {
                    for (Op* o : r->entry()->ops) {
                        if (o->name == "xyo.yield") continue;
                        if (o->name != "xyo.setvar" || o->operands.empty())
                            return false;
                        const std::string id(o->attrStr("id"));
                        if (!cands.count(id)) return false;
                        Value v = iOf(o->operands[0], b, out);
                        if (!v) return false;
                        cands[id].iv = v;
                    }
                    return true;
                };
                if (okSel) {
                    okSel = evalRegion(op->region(0));
                    for (auto& [id, c] : cands) thenVals[id] = c.iv;
                    for (auto& [id, c] : cands) c.iv = pre[id];
                    if (okSel) {
                        okSel = evalRegion(op->region(1));
                        for (auto& [id, c] : cands) elseVals[id] = c.iv;
                    }
                }
                if (!okSel) {
                    // select 化できない（値が整数でない等）。生成した途中の
                    // op を捨て、状態を入口へ戻して汎用の region 形式へ
                    // フォールバックする（空 Value を伝播させない）。
                    out.resize(outMark);
                    for (auto& [id, c] : cands) c.iv = pre[id];
                }
                if (okSel) {
                for (auto& [id, c] : cands) {
                    const Value& t = thenVals[id];
                    const Value& e = elseVals[id];
                    if (t.op == e.op && t.block == e.block &&
                        t.argIndex == e.argIndex) {
                        c.iv = t;
                        continue;
                    }
                    Op* sel = make("xyo.select", {cond, t, e}, {Type::i32()});
                    push(b, out, sel);
                    c.iv = sel->result(0);
                }
                continue;
                }
                // ここから下は汎用 if 経路（select 化に失敗した場合）。
            }
            if (n == "xyo.if") {
                // 両方の substack を書き換え、昇格変数の更新を結果として
                // 受け取る（if の後で phi を新しい現在値にする）。
                // **代入する変数だけ**を結果にする — 全昇格変数を結果に
                // すると、if 1 個あたり 2N 個の phi が増え、ループ内の
                // 分岐でレジスタ圧と命令数が跳ね上がる。
                struct Shadow {
                    Value bits, d, iv;
                };
                std::unordered_set<std::string> assigned;
                std::function<void(Block&)> collectAssigned = [&](Block& blk) {
                    for (Op* o : blk.ops) {
                        if (o->name == "xyo.setvar" && !o->operands.empty()) {
                            std::string id(o->attrStr("id"));
                            if (cands.count(id)) assigned.insert(id);
                        }
                        for (auto& r : o->regions)
                            if (r)
                                for (auto& cb : r->blocks)
                                    collectAssigned(*cb);
                    }
                };
                std::vector<Shadow> entryState;
                entryState.reserve(order.size());
                for (const std::string& id : order)
                    entryState.push_back(
                        {cands[id].bits, cands[id].d, cands[id].iv});
                // 両方の領域の代入先を先に集める（領域ごとに集めると、
                // 先に書き換える領域の yield 個数が後から増えて phi と
                // 不一致になる）。
                for (size_t ri = 0; ri < op->regions.size(); ++ri) {
                    Region* r = op->region(ri);
                    if (r && r->entry()) collectAssigned(*r->entry());
                }
                for (size_t ri = 0; ri < op->regions.size(); ++ri) {
                    Region* r = op->region(ri);
                    if (!r) continue;
                    for (size_t i = 0; i < order.size(); ++i) {
                        cands[order[i]].bits = entryState[i].bits;
                        cands[order[i]].d = entryState[i].d;
                        cands[order[i]].iv = entryState[i].iv;
                    }
                    const std::unordered_set<std::string>* saved = ifAssigned_;
                    ifAssigned_ = &assigned;
                    rewriteBlock(*r->entry());
                    ifAssigned_ = saved;
                }
                for (const std::string& id : order) {
                    if (!assigned.count(id)) continue;
                    if (kindOf(id) == 'i') {
                        op->resultTypes.push_back(Type::i32());
                        continue;
                    }
                    if (kindOf(id) == 'd') {
                        op->resultTypes.push_back(Type::f64());
                        continue;
                    }
                    op->resultTypes.push_back(Type::value());
                    op->resultTypes.push_back(Type::f64());
                }
                push(b, out, op);
                size_t roff2 = 0;
                for (const std::string& id : order) {
                    if (!assigned.count(id)) continue;
                    if (kindOf(id) == 'i') {
                        cands[id].iv = op->result(roff2++);
                        continue;
                    }
                    if (kindOf(id) == 'd') {
                        cands[id].d = op->result(roff2++);
                        cands[id].bits = {};
                        continue;
                    }
                    cands[id].bits = op->result(roff2++);
                    cands[id].d = op->result(roff2++);
                }
                continue;
            }
            if (n == "xyo.lt" || n == "xyo.gt" || n == "xyo.eq") {
                const int64_t pred = n == "xyo.lt" ? 0 : n == "xyo.gt" ? 1 : 4;
                Value a = op->operands[0], c = op->operands[1];
                // intOkOf は entry 定数でも ok を返すことがあるが、iOf は
                // 非 int 運搬の変数に空を返す。その場合は float へ
                // フォールスルー（空のまま置くと verifier 落ち）。
                bool useInt = intLoop && intOkOf(a).ok && intOkOf(c).ok;
                Value ai, ci;
                if (useInt) {
                    ai = iOf(a, b, out);
                    ci = iOf(c, b, out);
                    useInt = static_cast<bool>(ai) && static_cast<bool>(ci);
                }
                if (useInt) {
                    // 両辺とも整数証明済み → 整数比較（ガード不要）。
                    op->name = "xyo.cmp.i";
                    op->operands = {ai, ci};
                    op->attrs["pred"] = Attr::integer(pred);
                    push(b, out, op);
                    continue;
                }
                Value ab = bitsOf(a, b, out), cb = bitsOf(c, b, out);
                Value ad = dOf(a, b, out), cd = dOf(c, b, out);
                uint64_t xb = 0, yb = 0;
                auto plain = [](uint64_t bits) {
                    double d;
                    std::memcpy(&d, &bits, sizeof(d));
                    return bits < static_cast<uint64_t>(val::kNan) && d == d;
                };
                const bool ca = a.op && constBitsOf(a.op, &xb) && plain(xb);
                const bool cc = c.op && constBitsOf(c.op, &yb) && plain(yb);
                if (ca && cc) {
                    // 両方が素の数値定数 → コンパイル時に f64 比較へ。
                    op->name = "xyo.cmp.f";
                    op->operands = {ad, cd};
                    op->attrs["pred"] = Attr::integer(pred);
                    push(b, out, op);
                    continue;
                }
                op->name = "xyo.cmp.guarded";
                op->operands = {ab, cb, ad, cd};
                op->attrs["pred"] = Attr::integer(pred);
                push(b, out, op);
                continue;
            }
            if (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
                n == "xyo.div" || n == "xyo.mod" || n == "xyo.round" ||
                n == "xyo.mathop") {
                if (intLoop &&
                    (n == "xyo.add" || n == "xyo.sub" || n == "xyo.mul" ||
                     n == "xyo.mod" || n == "xyo.div") &&
                    op->operands.size() == 2 && intOkOf(op->operands[0]).ok &&
                    intOkOf(op->operands[1]).ok &&
                    !op->resultTypes.empty() && intOkOf(op->result(0)).ok &&
                    (n != "xyo.mod" || [&] {
                        int64_t mm = 0;
                        return intConstValue(op->operands[1], &mm) && mm > 0;
                    }()) &&
                    (n != "xyo.div" || [&] {
                        int64_t dd = 0;
                        return intConstValue(op->operands[1], &dd) && dd > 0;
                    }())) {
                    // 整数ループの純整数演算は in-place で i64 化する。
                    // 後続の iOf がそのまま拾える。
                    std::string target = n + ".i";
                    AttrMap attrs;
                    if (n == "xyo.mod") {
                        auto it = modSubK.find(op);
                        int subk = (it == modSubK.end()) ? -1 : it->second;
                        attrs["subk"] = Attr::integer(subk);
                    }
                    std::vector<Value> ios;
                    bool ok = true;
                    for (const Value& v : op->operands) {
                        Value ivv = iOf(v, b, out);
                        if (!ivv) {
                            ok = false;
                            break;
                        }
                        ios.push_back(ivv);
                    }
                    if (ok) {
                        op->name = std::move(target);
                        op->operands = std::move(ios);
                        op->resultTypes = {Type::i32()};
                        op->attrs = std::move(attrs);
                        push(b, out, op);
                        continue;
                    }
                    // proof と不整合（到達不能のはず）。空のまま残し
                    // verifier に落とさせる。
                }
                // 昇格変数を直接読むか、影（box.f）から来た値なら f64 で
                // 計算できる。box.f を数えないと、影の連鎖
                // （mul.f → box.f → mod）で mod 以降が Value 演算のまま
                // 残り、毎反復 xyo_mod 呼び出しになっていた。
                bool promoted = false;
                for (const Value& v : op->operands) {
                    if (v.op && (getvarOf.count(v.op) ||
                                 v.op->name == "xyo.box.f"))
                        promoted = true;
                    else if (curType(v) == Type::i32() ||
                             curType(v) == Type::i64())
                        promoted = true;
                }
                if (promoted) {
                    std::string target = n == "xyo.mathop" ? "xyo.mathop.f"
                                       : n == "xyo.round" ? "xyo.round.f"
                                       : n + ".f";
                    AttrMap attrs;
                    if (const Attr* a = op->attr("op")) attrs["op"] = *a;
                    std::vector<Value> operands;
                    for (const Value& v : op->operands)
                        operands.push_back(dOf(v, b, out));
                    Op* f = make(target, std::move(operands), {Type::f64()},
                                 std::move(attrs));
                    push(b, out, f);
                    // 値としての同一性は保つ（bits 側は box.f で作る）。
                    op->name = "xyo.box.f";
                    op->operands = {f->result(0)};
                    op->resultTypes = {Type::value()};
                    op->attrs.clear();
                    push(b, out, op);
                    continue;
                }
                push(b, out, op);
                continue;
            }
            if ((n == "xyo.list.item" || n == "xyo.list.replace") &&
                intLoop && op->operands.size() >= 2 &&
                (intOkOf(op->operands[1]).ok ||
                 curType(op->operands[1]) == Type::i32())) {
                // 索引が整数で証明できる（または既に i32 に書き換え済み）
                // なら、i32 のまま lowering へ渡す。浮動小数の範囲・整数
                // 検査と fptosi が消える。
                Value idx = intOkOf(op->operands[1]).ok
                                ? iOf(op->operands[1], b, out)
                                : op->operands[1];
                if (!idx.op && !idx.isBlockArg()) {
                    // 空 Value は伝播させない（従来経路に任せる）。
                    push(b, out, op);
                    continue;
                }
                if (idx) {
                    if (n == "xyo.list.item") {
                        op->name = "xyo.list.item.i";
                        op->operands[1] = idx;
                        push(b, out, op);
                        continue;
                    }
                    if (op->operands.size() == 3) {
                        std::vector<Value> ops = op->operands;
                        ops[1] = idx;
                        // ITEM は整数運搬のときだけ from_int へ。entry 定数
                        // のみで運搬が float の変数は iOf が空を返すため、
                        // その場合は bits 影へ（未変換だと getvar が残り、
                        // verifier 落ちでスクリプト全体が fallback する）。
                        // intOkOf は正準形整数"文字列"リテラル（s:"1"）も
                        // int と評価するが、from_int にすると格納 bits が
                        // num(1) に変わり interp と型がずれる。const 等は
                        // 生 bits をそのまま書き、本当に i32 由来の値だけ
                        // from_int にする。
                        bool rawBits = false;
                        if (ops[2].op &&
                            (ops[2].op->name == "xyo.const" ||
                             ops[2].op->name == "xyo.const.f64"))
                            rawBits = true;
                        Value iv2 = !rawBits && intOkOf(ops[2]).ok
                                        ? iOf(ops[2], b, out)
                                        : Value{};
                        if (iv2) {
                            Op* fx = make("xyo.from_int", {iv2},
                                          {Type::value()});
                            push(b, out, fx);
                            ops[2] = fx->result(0);
                        } else {
                            ops[2] = bitsOf(ops[2], b, out);
                        }
                        op->name = "xyo.list.replace.i";
                        op->operands = std::move(ops);
                        push(b, out, op);
                        continue;
                    }
                }
            }
            if (n == "xyo.to_num" || n == "xyo.truthy" || n == "xyo.not" ||
                n == "xyo.and" || n == "xyo.or" || n == "xyo.band" ||
                n == "xyo.bor" || n == "xyo.bnot" || n == "xyo.select" ||
                n == "xyo.to_str" || n == "xyo.strlen" || n == "xyo.from_num" ||
                n == "xyo.boolval" || n == "xyo.cmp.f" ||
                n == "xyo.const" || n == "xyo.const.f64") {
                // オペランドに昇格変数があれば Value 形（bits）へ差し替える。
                bool touched = false;
                std::vector<Value> operands = op->operands;
                for (size_t i = 0; i < operands.size(); ++i) {
                    if (operands[i].op && getvarOf.count(operands[i].op)) {
                        operands[i] = bitsOf(operands[i], b, out);
                        touched = true;
                    } else if (curType(operands[i]) == Type::i32() ||
                               curType(operands[i]) == Type::i64()) {
                        operands[i] = bitsOf(operands[i], b, out);
                        touched = true;
                    }
                }
                if (touched) op->operands = std::move(operands);
                push(b, out, op);
                continue;
            }
            // 未対応 op（リスト操作など）でも、昇格変数を直接読んでいたら
            // 影の bits に差し替える。getvar は除去済みなので、そのまま
            // 残すと「定義前使用」で verifier に落ちる。
            {
                bool touched = false;
                std::vector<Value> operands = op->operands;
                for (size_t i = 0; i < operands.size(); ++i) {
                    if (operands[i].op && getvarOf.count(operands[i].op)) {
                        operands[i] = bitsOf(operands[i], b, out);
                        touched = true;
                    } else if (curType(operands[i]) == Type::i32() ||
                               curType(operands[i]) == Type::i64()) {
                        // 書き換えで整数化された SSA をそのまま渡すと
                        // 型が合わない（i32 と i64 の比較など）。box する。
                        operands[i] = bitsOf(operands[i], b, out);
                        touched = true;
                    }
                }
                if (touched) op->operands = std::move(operands);
            }
            push(b, out, op);
        }
        b.ops = std::move(out);
    }

    /// repeat until / while の昇格。cond 領域と body 領域の両方に
    /// iter_arg（bits, f64 の対）を足し、cond の terminator で forward、
    /// body の yield で戻す。0 回反復なら cond の phi が入口値のまま
    /// なので、脱出後の書き戻しはそのまま result を使ってよい。
    void runWhile() {
        Block* cond = loop->region(0) ? loop->region(0)->entry() : nullptr;
        Block* body = loop->region(1) ? loop->region(1)->entry() : nullptr;
        if (!cond || !body) return;
        if (!scanBlock(*cond, &scanTmp, 0)) {
            if (intDebug()) {
                std::string names;
                for (Op* op : cond->ops) names += op->name + " ";
                std::fprintf(stderr, "[mlir-int] while cond reject: %s\n",
                             names.c_str());
            }
            return;
        }
        if (!scanBlock(*body, &scanTmp, 0)) {
            if (intDebug()) {
                std::string names;
                for (Op* op : body->ops) names += op->name + " ";
                std::fprintf(stderr, "[mlir-int] while body reject: %s\n",
                             names.c_str());
            }
            return;
        }
        cands = std::move(scanTmp.cands);
        if (cands.empty()) return;
        for (auto& [id, c] : cands) order.push_back(id);
        // while 系ループも warp 手続き内なら静的 warp（フロントが印を付ける）。
        // 印が無い/非 warp は従来どおり毎反復 spill する。
        warpLoop = loop->attrInt("warp") != 0;
        intLoop = analyzeIntLoop();
        // entry 非定数で落ちた場合は入口ガード版を試す。
        bool useGuard = false;
        if (!intLoop && guardEnabled()) {
            scanIntConsts();
            std::map<std::string, std::pair<int64_t, int64_t>> guardMap;
            for (auto& [id, c] : cands) {
                if (!m.intEligibleVars.count(id)) continue;
                if (intConsts.count(id)) continue;
                guardMap[id] = {-kGuardBound, kGuardBound};
            }
            if (!guardMap.empty()) {
                intLoop = analyzeIntLoop(&guardMap);
                guardEntries_ = nullptr;
                useGuard = intLoop;
            }
            // 下側発散で落ちるループ向けに非負ボックス [0, 2^20] で
            // 再試行する（カウンタ・添字・蓄積の典型形）。
            std::map<std::string, std::pair<int64_t, int64_t>> boxMap;
            if (!intLoop && !guardMap.empty()) {
                for (auto& [id, box] : guardMap) box = {0, kGuardBound};
                intLoop = analyzeIntLoop(&guardMap);
                guardEntries_ = nullptr;
                guardNonNeg_ = intLoop;
                useGuard = intLoop;
            }
            // ループ条件でボックスを強制する版（途中脱出）。collatz の
            // 3n+1 のように区間が発散する本体は、条件側で 0..2^20 を
            // 強制すれば i32 で閉じる。範囲外は直後の float ループが
            // 引き継ぐので意味論は不変。
            if (!intLoop && !guardMap.empty() && boxEnabled() &&
                loop->name == "xyo.while") {
                for (auto& [id, v] : guardMap) boxMap[id] = {0, kGuardBound};
                if (analyzeIntLoop(&boxMap, /*clampToBox=*/true,
                                   /*relaxClean=*/true)) {
                    guardEntries_ = nullptr;
                    planKinds(/*allowD=*/false);
                    if (runBoxWhile()) return;
                    intLoop = false;
                }
                guardEntries_ = nullptr;
            }
        }
        planKinds(/*allowD=*/true);
        if (useGuard) {
            // ガード版は変数単位の int/float 運搬だけ使う（d-only は
            // 対象外）。失敗時は従来の float 昇格へ。
            planKinds(/*allowD=*/false);
            if (runGuardedWhile()) return;
            intLoop = false;
            planKinds(/*allowD=*/true);
        }
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            if (intLoop && c.isInt) {
                Op* k = m.create("xyo.const.i", {}, {Type::i32()},
                                 {{"v", Attr::integer(c.intEntry)}});
                k->parent = &parent;
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex), k);
                ++loopIndex;
                Op* fx = m.create("xyo.from_int", {k->result(0)},
                                   {Type::value()});
                fx->parent = &parent;
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex), fx);
                ++loopIndex;
                c.initBits = fx->result(0);
                loop->operands.push_back(k->result(0));
                loop->resultTypes.push_back(Type::i32());
                Value cb{};
                cb.block = cond;
                cb.argIndex = static_cast<uint32_t>(cond->args.size());
                cb.type = Type::i32();
                cond->args.push_back(cb);
                Value bb{};
                bb.block = body;
                bb.argIndex = static_cast<uint32_t>(body->args.size());
                bb.type = Type::i32();
                body->args.push_back(bb);
                c.iv = cb;
                continue;
            }
            Op* g = m.create("xyo.getvar", {}, {Type::value()},
                             {{"id", Attr::str(c.id)},
                              {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), g);
            ++loopIndex;
            Op* t = m.create("xyo.to_num", {g->result(0)}, {Type::f64()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), t);
            ++loopIndex;
            c.initBits = g->result(0);
            if (kindOf(c.id) == 'd') {
                // f64 だけを iter_arg にする（ビットは必要時に box.f）。
                c.bits = {};
                c.d = t->result(0);
                loop->operands.push_back(t->result(0));
                loop->resultTypes.push_back(Type::f64());
                Value cd0{};
                cd0.block = cond;
                cd0.argIndex = static_cast<uint32_t>(cond->args.size());
                cd0.type = Type::f64();
                cond->args.push_back(cd0);
                Value bd0{};
                bd0.block = body;
                bd0.argIndex = static_cast<uint32_t>(body->args.size());
                bd0.type = Type::f64();
                body->args.push_back(bd0);
                c.d = cd0;
                continue;
            }
            loop->operands.push_back(g->result(0));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(t->result(0));
            loop->resultTypes.push_back(Type::f64());
            Value cb{};
            cb.block = cond;
            cb.argIndex = static_cast<uint32_t>(cond->args.size());
            cb.type = Type::value();
            cond->args.push_back(cb);
            Value cd = cb;
            cd.type = Type::f64();
            Value cdArg;
            cdArg.block = cond;
            cdArg.argIndex = static_cast<uint32_t>(cond->args.size());
            cdArg.type = Type::f64();
            cond->args.push_back(cdArg);
            Value bb{};
            bb.block = body;
            bb.argIndex = static_cast<uint32_t>(body->args.size());
            bb.type = Type::value();
            body->args.push_back(bb);
            Value bd{};
            bd.block = body;
            bd.argIndex = static_cast<uint32_t>(body->args.size());
            bd.type = Type::f64();
            body->args.push_back(bd);
            c.bits = cb;
            c.d = cdArg;
        }
        // cond 領域 → body へ forward
        condTerm = true;
        rewriteBlock(*cond);
        condTerm = false;
        // body 領域: cond から forward された値を現在値にする
        size_t aoff = 0;
        for (size_t i = 0; i < order.size(); ++i) {
            LoopCandidate& c = cands[order[i]];
            if (kindOf(c.id) == 'i') {
                c.iv = body->args[aoff++];
                continue;
            }
            if (kindOf(c.id) == 'd') {
                c.d = body->args[aoff++];
                c.bits = {};
                continue;
            }
            c.bits = body->args[aoff++];
            c.d = body->args[aoff++];
        }
        rewriteBlock(*body);
        // 脱出時の書き戻し（while の結果 = cond 側の phi）。
        // 整数ループは from_int で Value に戻す。
        {
            std::vector<Op*> wb;
            size_t roff = 0;
            for (size_t i = 0; i < order.size(); ++i) {
                LoopCandidate& c = cands[order[i]];
                Value wv;
                if (intLoop && c.isInt) {
                    Op* fx = m.create("xyo.from_int", {loop->result(roff)},
                                       {Type::value()});
                    fx->parent = &parent;
                    wb.push_back(fx);
                    wv = fx->result(0);
                    ++roff;
                    Op* st = m.create("xyo.setvar", {wv}, {},
                                      {{"id", Attr::str(c.id)},
                                       {"name", Attr::str(c.name)}});
                    st->parent = &parent;
                    wb.push_back(st);
                    continue;
                }
                if (kindOf(c.id) == 'd') {
                    Op* bx = m.create("xyo.box.f", {loop->result(roff)},
                                       {Type::value()});
                    bx->parent = &parent;
                    wb.push_back(bx);
                    ++roff;
                    Op* st = m.create("xyo.setvar", {bx->result(0)}, {},
                                      {{"id", Attr::str(c.id)},
                                       {"name", Attr::str(c.name)}});
                    st->parent = &parent;
                    wb.push_back(st);
                    continue;
                }
                wv = loop->result(roff);
                roff += 2;
                Op* st = m.create("xyo.setvar", {wv}, {},
                                  {{"id", Attr::str(c.id)},
                                   {"name", Attr::str(c.name)}});
                st->parent = &parent;
                wb.push_back(st);
            }
            for (size_t i = 0; i < wb.size(); ++i)
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex + 1 + i),
                                  wb[i]);
        }
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            if (intLoop) ++stats->intLoops;
        }
    }

    /// ガード版の配線（float 運搬）。init は親で作った getvar/to_num を
    /// 共有する（guard 検査と同一値）。
    void wireFloat(const std::map<std::string, Value>& gv,
                   const std::map<std::string, Value>& dv) {
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            c.initBits = gv.at(id);
            if (kindOf(id) == 'd') {
                // f64 だけを運ぶ。bits は必要時に box.f で作る。
                loop->operands.push_back(dv.at(id));
                loop->resultTypes.push_back(Type::f64());
                Value ad;
                ad.block = &body;
                ad.argIndex = static_cast<uint32_t>(body.args.size());
                ad.type = Type::f64();
                body.args.push_back(ad);
                c.bits = {};
                c.d = ad;
                continue;
            }
            loop->operands.push_back(gv.at(id));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(dv.at(id));
            loop->resultTypes.push_back(Type::f64());
            Value abits;
            abits.block = &body;
            abits.argIndex = static_cast<uint32_t>(body.args.size());
            abits.type = Type::value();
            body.args.push_back(abits);
            Value ad;
            ad.block = &body;
            ad.argIndex = static_cast<uint32_t>(body.args.size());
            ad.type = Type::f64();
            body.args.push_back(ad);
            c.bits = abits;
            c.d = ad;
        }
    }

    /// ガード版の配線（int 運搬）。int 変数は to_int(getvar) を iter_arg
    /// にする（ガード成功で正確）。非 int は float 運搬。
    void wireInt(const std::map<std::string, Value>& gv,
                 const std::map<std::string, Value>& dv,
                 const std::map<std::string, Value>& tv) {
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            if (c.isInt) {
                c.initBits = gv.at(id);
                loop->operands.push_back(tv.at(id));
                loop->resultTypes.push_back(Type::i32());
                Value ai;
                ai.block = &body;
                ai.argIndex = static_cast<uint32_t>(body.args.size());
                ai.type = Type::i32();
                body.args.push_back(ai);
                c.iv = ai;
                continue;
            }
            c.initBits = gv.at(id);
            if (kindOf(id) == 'd') {
                loop->operands.push_back(dv.at(id));
                loop->resultTypes.push_back(Type::f64());
                Value ad;
                ad.block = &body;
                ad.argIndex = static_cast<uint32_t>(body.args.size());
                ad.type = Type::f64();
                body.args.push_back(ad);
                c.bits = {};
                c.d = ad;
                continue;
            }
            loop->operands.push_back(gv.at(id));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(dv.at(id));
            loop->resultTypes.push_back(Type::f64());
            Value abits;
            abits.block = &body;
            abits.argIndex = static_cast<uint32_t>(body.args.size());
            abits.type = Type::value();
            body.args.push_back(abits);
            Value ad;
            ad.block = &body;
            ad.argIndex = static_cast<uint32_t>(body.args.size());
            ad.type = Type::f64();
            body.args.push_back(ad);
            c.bits = abits;
            c.d = ad;
        }
    }

    /// 入口ガード付きループ複製（repeat 専用）。前提: ガード範囲つき
    /// analyzeIntLoop が成功し、cands[].isInt、order、warpLoop が確定。
    /// 元ループは else 分岐へ移し、整数化した複製を then 分岐へ置く。
    /// ガード偽なら元ループが走るため意味論は不変。成功時は true。
    /// 失敗時（int 変数なし）は IR を変更せず false を返す。
    bool runGuarded() {
        bool hasInt = false;
        for (const std::string& id : order)
            if (cands[id].isInt) {
                hasInt = true;
                break;
            }
        if (!hasInt) return false;
        // (1) 親ブロックに guard + 両分岐の init を作る。
        std::map<std::string, Value> gv, dv, tv;
        std::vector<Value> oks;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            Op* g = make("xyo.getvar", {}, {Type::value()},
                         {{"id", Attr::str(c.id)},
                          {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              g);
            ++loopIndex;
            gv[id] = g->result(0);
            Op* d = make("xyo.to_num", {g->result(0)}, {Type::f64()});
            d->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              d);
            ++loopIndex;
            dv[id] = d->result(0);
            if (!c.isInt) continue;
            AttrMap ga;
            if (guardNonNeg_) ga["nonneg"] = Attr::integer(1);
            Op* ok = make("xyo.int_exact_ok", {g->result(0)},
                          {Type::boolean()}, std::move(ga));
            ok->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              ok);
            ++loopIndex;
            oks.push_back(ok->result(0));
            Op* t = make("xyo.to_int", {g->result(0)}, {Type::i32()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              t);
            ++loopIndex;
            tv[id] = t->result(0);
        }
        // (2) guard 結合（2 項 and の連鎖）。
        Value guardVal = oks[0];
        for (size_t i = 1; i < oks.size(); ++i) {
            Op* a = make("xyo.and", {guardVal, oks[i]}, {Type::boolean()});
            a->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              a);
            ++loopIndex;
            guardVal = a->result(0);
        }
        // (3) xyo.if の殻を作る。結果は (bits,f64)×変数で両分岐統一。
        // この経路の if は (bits,f64) ペアを前提に書き戻すので、f64 運搬の
        // 変数はここではペア運搬へ降格する（意味論は不変）。
        for (const std::string& id : order)
            if (kindOf(id) == 'd') kinds_[id] = 'b';
        std::vector<Type> ifResults;
        for (size_t i = 0; i < order.size(); ++i) {
            ifResults.push_back(Type::value());
            ifResults.push_back(Type::f64());
        }
        Op* ifOp = make("xyo.if", {guardVal}, std::move(ifResults));
        Region* rThen = ifOp->addRegion();
        Block* thenB = rThen->addBlock();
        Region* rElse = ifOp->addRegion();
        Block* elseB = rElse->addBlock();
        ifOp->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          ifOp);
        ++loopIndex;
        // (4) 配線前の素朴なループを複製（then 用）し、元を else へ移す。
        Cloner cl{m};
        Op* thenLoop = cl.cloneInto(loop, *thenB);
        Block* thenBody = thenLoop->region(0)->entry();
        auto it = std::find(parent.ops.begin(), parent.ops.end(), loop);
        if (it != parent.ops.end()) parent.ops.erase(it);
        loop->parent = elseB;
        elseB->ops.push_back(loop);
        // (5a) else（元ループ・float 運搬）を配線＋書き換え。
        // isInt は落とす（else は整数運搬しない。bitsOf が空の iv を
        // 使って from_int を作るのを防ぐ）。
        Promoter pe(m, *elseB, 0, loop, body, {}, {}, {}, stats);
        pe.cands = cands;
        for (auto& [id, c] : pe.cands) c.isInt = false;
        pe.order = order;
        pe.warpLoop = warpLoop;
        pe.planKinds(false);
        pe.wireFloat(gv, dv);
        pe.rewriteBlock(body);
        {
            std::vector<Value> outs;
            for (size_t i = 0; i < loop->resultTypes.size(); ++i)
                outs.push_back(loop->result(i));
            Op* y = pe.make("xyo.yield", std::move(outs), {});
            y->parent = elseB;
            elseB->ops.push_back(y);
        }
        // (5b) then（複製・int 運搬）を配線＋書き換え。証明セットは
        // 複製先へ付け替える（素朴名での再評価も健全だが、改名後の
        // ヒットにはポインタ対応が要る）。
        Promoter pt(m, *thenB, 0, thenLoop, *thenBody, {}, {}, {}, stats);
        pt.cands = cands;
        pt.order = order;
        pt.warpLoop = warpLoop;
        pt.intLoop = true;
        pt.planKinds(false);
        for (auto& [o, k] : modSubK) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.modSubK[f->second] = k;
        for (auto& [o, r] : divRanges_) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.divRanges_[f->second] = r;
        }
        }
        for (Op* o : intProvenOps) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.intProvenOps.insert(f->second);
        }
        pt.wireInt(gv, dv, tv);
        pt.rewriteBlock(*thenBody);
        {
            // then の結果を (bits,f64) に揃える（ガード成功で正確）。
            // ループ結果（thenB 内で定義）を使う。body 引数は別
            // リージョンのため参照できない。
            std::vector<Value> outs;
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = pt.cands[id];
                if (c.isInt) {
                    Op* fx = pt.make("xyo.from_int", {thenLoop->result(roff)},
                                     {Type::value()});
                    fx->parent = thenB;
                    thenB->ops.push_back(fx);
                    Op* dn = pt.make("xyo.to_num", {fx->result(0)},
                                     {Type::f64()});
                    dn->parent = thenB;
                    thenB->ops.push_back(dn);
                    outs.push_back(fx->result(0));
                    outs.push_back(dn->result(0));
                    ++roff;
                } else {
                    outs.push_back(thenLoop->result(roff));
                    outs.push_back(thenLoop->result(roff + 1));
                    roff += 2;
                }
            }
            Op* y = pt.make("xyo.yield", std::move(outs), {});
            y->parent = thenB;
            thenB->ops.push_back(y);
        }
        // (6) 脱出後の書き戻し（従来の repeat commit と同じ）。
        // initBits はループ前の実メモリ値（select の 0 回反復側）。
        for (const std::string& id : order) cands[id].initBits = gv.at(id);
        commitRepeat(ifOp);
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            ++stats->guardedLoops;
            ++stats->intLoops;
        }
        return true;
    }

    void run() {
        if (loop->name == "xyo.while") {
            runWhile();
            return;
        }
        Scan scan;
        if (!scanBlock(body, &scan, 0)) {
            if (intDebug()) {
                std::string names;
                for (Op* op : body.ops) names += op->name + " ";
                std::fprintf(stderr, "[mlir-int] scan reject %s: %s\n",
                             loop->name.c_str(), names.c_str());
            }
            return;
        }
        cands = std::move(scan.cands);
        if (cands.empty()) return;
        nestedOps_ = std::move(scan.nestedOps);
        nestedWrites_ = std::move(scan.nestedWrites);
        warpLoop = loop->attrInt("warp") != 0;
        // 非 warp ループは毎反復で他スクリプトへ譲るため、昇格しても
        // 毎回 real メモリへ spill/reload が必要になり得る。A/B 用の
        // 非常口（既定は従来どおり昇格する）。
        static const bool noNonWarp = [] {
            const char* e = std::getenv("XYO_MLIR_PROMOTE_WARP_ONLY");
            return e && e[0] == '1';
        }();
        if (noNonWarp && !warpLoop) return;
        const Value countOperand =
            loop->operands.empty() ? Value{} : loop->operands[0];
        for (auto& [id, c] : cands) order.push_back(id);
        // 整数プロモーションの可否判定（all-or-nothing）。
        intLoop = analyzeIntLoop();
        // entry 非定数で落ちた場合は入口ガード版を試す（変数単位
        // 整数化＋入口検査）。成功すれば複製ループを出す。
        bool useGuard = false;
        bool useBoxRepeat = false;
        // 入れ子ループ入りのループは複製版（guard/box）を作らない。
        // 複製は入れ子ループも複製するため、spill/reload の対象 op を
        // ポインタで追えなくなる。
        const bool nested = !nestedOps_.empty();
        const bool hasNestedLoop = nested;
        if (!intLoop && guardEnabled() && !nested) {
            scanIntConsts();
            std::map<std::string, std::pair<int64_t, int64_t>> guardMap;
            for (auto& [id, c] : cands) {
                if (!m.intEligibleVars.count(id)) continue;
                if (intConsts.count(id)) continue;
                guardMap[id] = {-kGuardBound, kGuardBound};
            }
            if (!guardMap.empty()) {
                intLoop = analyzeIntLoop(&guardMap);
                guardEntries_ = nullptr;
                useGuard = intLoop;
            }
            // 下側発散で落ちるループ向けに非負ボックス [0, 2^20] で
            // 再試行する（カウンタ・添字・蓄積の典型形）。
            if (!intLoop && !guardMap.empty()) {
                for (auto& [id, box] : guardMap) box = {0, kGuardBound};
                intLoop = analyzeIntLoop(&guardMap);
                guardEntries_ = nullptr;
                guardNonNeg_ = intLoop;
                useGuard = intLoop;
            }
            // repeat 版ボックス（途中脱出つき整数化）。プロジェクト全体の
            // 非整数書き込みで落ちる repeat（添字・カウンタの典型形）を、
            // ループ内書き込みだけが整数式であること＋入口ガード＋
            // 反復内 inrange＋float 継続で救う。動的 trip は上限を
            // 仮定して解析し、実行時ガードで保証する。
            // 定数 entry の変数も対象にする（guardMap は非定数のみで
            // 空になり得るが、count 不一致の定数 entry 変数こそ
            // relaxClean で救うべき）。clampToBox が guardEntries_ を
            // ボックスとして使うため、定数 entry でもボックス全体を
            // 入れる（正確な定数は intConsts 側に残るが、guardEntries_
            // が優先されて intGuarded になる。範囲はボックスに収まる
            // ので健全。ガードが実行時に検証する）。
            if (!intLoop && boxEnabled() && !hasNestedLoop &&
                loop->name == "xyo.repeat") {
                std::map<std::string, std::pair<int64_t, int64_t>> boxMap;
                for (auto& [id, c] : cands) {
                    if (!m.intEligibleVars.count(id)) continue;
                    boxMap[id] = {0, kGuardBound};
                }
                // 動的 trip は上限つきで解析（実行時ガードが保証する）。
                // 定数 trip はそのまま（tripBound が解決する）。
                bool dynTrip = tripBound() < 0;
                if (dynTrip) forcedTrips_ = kGuardBound;
                if (intDebug()) {
                    std::string bk;
                    for (auto& [id, v] : boxMap) {
                        if (!bk.empty()) bk += ",";
                        bk += id;
                    }
                    std::fprintf(stderr,
                                 "[mlir-int] box-repeat try boxMap=%zu dynTrip=%d ids=%s\n",
                                 boxMap.size(), (int)dynTrip, bk.c_str());
                }
                if (!boxMap.empty() &&
                    analyzeIntLoop(&boxMap, /*clampToBox=*/true,
                                   /*relaxClean=*/true)) {
                    guardEntries_ = nullptr;
                    forcedTrips_ = -1;
                    guardNonNeg_ = true;
                    useBoxRepeat = true;
                    intLoop = true;
                    intTrace("box-repeat analyze ok (%s)", std::string());
                } else {
                    guardEntries_ = nullptr;
                    forcedTrips_ = -1;
                }
            }
        }
        planKinds(/*allowD=*/true);
        if (!nestedOps_.empty()) {
            // 入れ子ループの spill は bits を使う。d-only 運搬（bits を
            // 持たない）では渡せないので、該当変数だけペア運搬へ降格する
            // （昇格自体は維持する）。
            for (const std::string& id : order)
                if (kindOf(id) == 'd') kinds_[id] = 'b';
        }
        if (useGuard) {
            if (runGuarded()) return;
            // ガード版を断念 → 従来の float 昇格へ。
            intLoop = false;
        }
        if (useBoxRepeat) {
            if (runBoxRepeat()) return;
            // ボックス版を断念 → 従来の float 昇格へ。
            intLoop = false;
        }
        // 1) ループ入口で実メモリから (bits, f64) を取り出して iter_arg に。
        // 整数ループは entry 定数だけを i64 で渡す。
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            if (intLoop && c.isInt) {
                Op* k = m.create("xyo.const.i", {}, {Type::i32()},
                                 {{"v", Attr::integer(c.intEntry)}});
                k->parent = &parent;
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex), k);
                ++loopIndex;
                Op* fx = m.create("xyo.from_int", {k->result(0)},
                                   {Type::value()});
                fx->parent = &parent;
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex), fx);
                ++loopIndex;
                c.initBits = fx->result(0);
                loop->operands.push_back(k->result(0));
                loop->resultTypes.push_back(Type::i32());
                Value ai;
                ai.block = &body;
                ai.argIndex = static_cast<uint32_t>(body.args.size());
                ai.type = Type::i32();
                body.args.push_back(ai);
                c.iv = ai;
                continue;
            }
            // ループ直前の同一ブロックに `set v to <数値定数>` があれば、
            // その定数を初期値にする（メモリ経由の読みをやめ、LLVM が
            // ループを折り畳めるようにする）。
            Op* g = m.create("xyo.getvar", {}, {Type::value()},
                             {{"id", Attr::str(c.id)},
                              {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), g);
            ++loopIndex;
            Op* t = m.create("xyo.to_num", {g->result(0)}, {Type::f64()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), t);
            ++loopIndex;
            c.initBits = g->result(0);
            if (kindOf(id) == 'd') {
                loop->operands.push_back(t->result(0));
                loop->resultTypes.push_back(Type::f64());
                Value ad0;
                ad0.block = &body;
                ad0.argIndex = static_cast<uint32_t>(body.args.size());
                ad0.type = Type::f64();
                body.args.push_back(ad0);
                c.bits = {};
                c.d = ad0;
                continue;
            }
            c.bits = g->result(0);
            c.d = t->result(0);
            loop->operands.push_back(g->result(0));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(t->result(0));
            loop->resultTypes.push_back(Type::f64());
            Value abits;
            abits.block = &body;
            abits.argIndex = static_cast<uint32_t>(body.args.size());
            abits.type = Type::value();
            body.args.push_back(abits);
            Value ad;
            ad.block = &body;
            ad.argIndex = static_cast<uint32_t>(body.args.size());
            ad.type = Type::f64();
            body.args.push_back(ad);
            // body 側の block arg を現在値にする。
            c.bits = abits;
            c.d = ad;
        }
        // 2) 本体を書き換える。
        rewriteBlock(body);
        commitRepeat(loop);
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            if (intLoop) ++stats->intLoops;
        }
    }

    /// ガード版 while の配線（float 運搬）。cond と body の両方に
    /// (bits,f64) の iter_arg を足す。
    void wireFloatWhile(const std::map<std::string, Value>& gv,
                        const std::map<std::string, Value>& dv) {
        Block* cond = loop->region(0) ? loop->region(0)->entry() : nullptr;
        if (!cond) return;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            c.initBits = gv.at(id);
            loop->operands.push_back(gv.at(id));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(dv.at(id));
            loop->resultTypes.push_back(Type::f64());
            Value cb;
            cb.block = cond;
            cb.argIndex = static_cast<uint32_t>(cond->args.size());
            cb.type = Type::value();
            cond->args.push_back(cb);
            Value cdArg;
            cdArg.block = cond;
            cdArg.argIndex = static_cast<uint32_t>(cond->args.size());
            cdArg.type = Type::f64();
            cond->args.push_back(cdArg);
            Value bb;
            bb.block = &body;
            bb.argIndex = static_cast<uint32_t>(body.args.size());
            bb.type = Type::value();
            body.args.push_back(bb);
            Value bd;
            bd.block = &body;
            bd.argIndex = static_cast<uint32_t>(body.args.size());
            bd.type = Type::f64();
            body.args.push_back(bd);
            c.bits = cb;
            c.d = cdArg;
        }
    }

    /// ガード版 while の配線（int 運搬）。
    void wireIntWhile(const std::map<std::string, Value>& gv,
                      const std::map<std::string, Value>& dv,
                      const std::map<std::string, Value>& tv) {
        Block* cond = loop->region(0) ? loop->region(0)->entry() : nullptr;
        if (!cond) return;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            c.initBits = gv.at(id);
            if (c.isInt) {
                loop->operands.push_back(tv.at(id));
                loop->resultTypes.push_back(Type::i32());
                Value ci;
                ci.block = cond;
                ci.argIndex = static_cast<uint32_t>(cond->args.size());
                ci.type = Type::i32();
                cond->args.push_back(ci);
                Value bi;
                bi.block = &body;
                bi.argIndex = static_cast<uint32_t>(body.args.size());
                bi.type = Type::i32();
                body.args.push_back(bi);
                c.iv = ci;
                continue;
            }
            loop->operands.push_back(gv.at(id));
            loop->resultTypes.push_back(Type::value());
            loop->operands.push_back(dv.at(id));
            loop->resultTypes.push_back(Type::f64());
            Value cb;
            cb.block = cond;
            cb.argIndex = static_cast<uint32_t>(cond->args.size());
            cb.type = Type::value();
            cond->args.push_back(cb);
            Value cdArg;
            cdArg.block = cond;
            cdArg.argIndex = static_cast<uint32_t>(cond->args.size());
            cdArg.type = Type::f64();
            cond->args.push_back(cdArg);
            Value bb;
            bb.block = &body;
            bb.argIndex = static_cast<uint32_t>(body.args.size());
            bb.type = Type::value();
            body.args.push_back(bb);
            Value bd;
            bd.block = &body;
            bd.argIndex = static_cast<uint32_t>(body.args.size());
            bd.type = Type::f64();
            body.args.push_back(bd);
            c.bits = cb;
            c.d = cdArg;
        }
    }

    /// cond 書き換え後、body の影を body 引数に戻す（cond 領域の値は
    /// body から見えないため。runWhile と同じ）。
    void resetBodyShadows() {
        size_t aoff = 0;
        for (size_t i = 0; i < order.size(); ++i) {
            LoopCandidate& c = cands[order[i]];
            if (kindOf(c.id) == 'i') {
                c.iv = body.args[aoff++];
                continue;
            }
            if (kindOf(c.id) == 'd') {
                c.d = body.args[aoff++];
                c.bits = {};
                continue;
            }
            c.bits = body.args[aoff];
            c.d = body.args[aoff + 1];
            aoff += 2;
        }
    }

    /// 入口ガード付きループ複製（while/repeat_until 専用）。
    bool runGuardedWhile() {
        bool hasInt = false;
        for (const std::string& id : order)
            if (cands[id].isInt) {
                hasInt = true;
                break;
            }
        if (!hasInt) return false;
        Block* origCond =
            loop->region(0) ? loop->region(0)->entry() : nullptr;
        if (!origCond) return false;
        // (1) 親ブロックに guard + 両分岐の init を作る。
        std::map<std::string, Value> gv, dv, tv;
        std::vector<Value> oks;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            Op* g = make("xyo.getvar", {}, {Type::value()},
                         {{"id", Attr::str(c.id)},
                          {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              g);
            ++loopIndex;
            gv[id] = g->result(0);
            Op* d = make("xyo.to_num", {g->result(0)}, {Type::f64()});
            d->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              d);
            ++loopIndex;
            dv[id] = d->result(0);
            if (!c.isInt) continue;
            AttrMap ga;
            if (guardNonNeg_) ga["nonneg"] = Attr::integer(1);
            Op* ok = make("xyo.int_exact_ok", {g->result(0)},
                          {Type::boolean()}, std::move(ga));
            ok->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              ok);
            ++loopIndex;
            oks.push_back(ok->result(0));
            Op* t = make("xyo.to_int", {g->result(0)}, {Type::i32()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              t);
            ++loopIndex;
            tv[id] = t->result(0);
        }
        // (2) guard 結合。
        Value guardVal = oks[0];
        for (size_t i = 1; i < oks.size(); ++i) {
            Op* a = make("xyo.and", {guardVal, oks[i]}, {Type::boolean()});
            a->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              a);
            ++loopIndex;
            guardVal = a->result(0);
        }
        // (3) xyo.if の殻を作る。結果は (bits,f64)×変数。
        std::vector<Type> ifResults;
        for (size_t i = 0; i < order.size(); ++i) {
            ifResults.push_back(Type::value());
            ifResults.push_back(Type::f64());
        }
        Op* ifOp = make("xyo.if", {guardVal}, std::move(ifResults));
        Region* rThen = ifOp->addRegion();
        Block* thenB = rThen->addBlock();
        Region* rElse = ifOp->addRegion();
        Block* elseB = rElse->addBlock();
        ifOp->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          ifOp);
        ++loopIndex;
        // (4) 配線前の素朴なループを複製（then 用）し、元を else へ移す。
        Cloner cl{m};
        Op* thenLoop = cl.cloneInto(loop, *thenB);
        Block* thenBody = thenLoop->region(1)->entry();
        Block* thenCond = thenLoop->region(0)->entry();
        auto it = std::find(parent.ops.begin(), parent.ops.end(), loop);
        if (it != parent.ops.end()) parent.ops.erase(it);
        loop->parent = elseB;
        elseB->ops.push_back(loop);
        // (5a) else（元ループ・float 運搬）。
        Promoter pe(m, *elseB, 0, loop, body, {}, {}, {}, stats);
        pe.cands = cands;
        for (auto& [id, c] : pe.cands) c.isInt = false;
        pe.order = order;
        pe.warpLoop = warpLoop;
        pe.planKinds(false);
        pe.wireFloatWhile(gv, dv);
        {
            Block* elseCond =
                loop->region(0) ? loop->region(0)->entry() : nullptr;
            pe.condTerm = true;
            pe.rewriteBlock(*elseCond);
            pe.condTerm = false;
            pe.resetBodyShadows();
            pe.rewriteBlock(body);
        }
        {
            std::vector<Value> outs;
            for (size_t i = 0; i < loop->resultTypes.size(); ++i)
                outs.push_back(loop->result(i));
            Op* y = pe.make("xyo.yield", std::move(outs), {});
            y->parent = elseB;
            elseB->ops.push_back(y);
        }
        // (5b) then（複製・int 運搬）。
        Promoter pt(m, *thenB, 0, thenLoop, *thenBody, {}, {}, {}, stats);
        pt.cands = cands;
        pt.order = order;
        pt.warpLoop = warpLoop;
        pt.intLoop = true;
        pt.planKinds(false);
        for (auto& [o, k] : modSubK) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.modSubK[f->second] = k;
        for (auto& [o, r] : divRanges_) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.divRanges_[f->second] = r;
        }
        }
        for (Op* o : intProvenOps) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pt.intProvenOps.insert(f->second);
        }
        pt.wireIntWhile(gv, dv, tv);
        {
            pt.condTerm = true;
            pt.rewriteBlock(*thenCond);
            pt.condTerm = false;
            pt.resetBodyShadows();
            pt.rewriteBlock(*thenBody);
        }
        {
            std::vector<Value> outs;
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = pt.cands[id];
                if (c.isInt) {
                    Op* fx = pt.make("xyo.from_int", {thenLoop->result(roff)},
                                     {Type::value()});
                    fx->parent = thenB;
                    thenB->ops.push_back(fx);
                    Op* dn = pt.make("xyo.to_num", {fx->result(0)},
                                     {Type::f64()});
                    dn->parent = thenB;
                    thenB->ops.push_back(dn);
                    outs.push_back(fx->result(0));
                    outs.push_back(dn->result(0));
                    ++roff;
                } else {
                    outs.push_back(thenLoop->result(roff));
                    outs.push_back(thenLoop->result(roff + 1));
                    roff += 2;
                }
            }
            Op* y = pt.make("xyo.yield", std::move(outs), {});
            y->parent = thenB;
            thenB->ops.push_back(y);
        }
        // (6) 脱出後の書き戻し（runWhile と同じく select なし）。
        for (size_t i = 0; i < order.size(); ++i) {
            LoopCandidate& c = cands[order[i]];
            Op* st = make("xyo.setvar", {ifOp->result(2 * i)}, {},
                          {{"id", Attr::str(c.id)},
                           {"name", Attr::str(c.name)}});
            st->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex + i),
                              st);
        }
        loopIndex += order.size();
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            ++stats->guardedLoops;
            ++stats->intLoops;
        }
        return true;
    }

    /// while 系の「範囲強制つき整数ループ＋後続 float ループ」（途中脱出）。
    /// 前提: ボックス [0,2^20] で analyzeIntLoop が成功し cands[].isInt と
    /// order が確定している。cond に inrange を足すので、値がボックスを
    /// 出た時点で int ループは抜け、直後の float ループが引き継ぐ
    /// （cond が偽なら float ループは 0 回反復なので二重実行にならない）。
    /// 入口の値が正確な整数でない場合は if の else（空）で int ループを
    /// 飛ばし、変数を汚さない。
    bool runBoxWhile() {
        bool hasInt = false;
        for (const std::string& id : order)
            if (cands[id].isInt) { hasInt = true; break; }
        Block* origCond = loop->region(0) ? loop->region(0)->entry() : nullptr;
        if (!hasInt || !origCond) return false;
        // (1) 入口値 + ガード。
        std::map<std::string, Value> gv, dv, tv;
        std::vector<Value> oks;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            Op* g = make("xyo.getvar", {}, {Type::value()},
                         {{"id", Attr::str(c.id)}, {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), g);
            ++loopIndex;
            gv[id] = g->result(0);
            Op* d = make("xyo.to_num", {g->result(0)}, {Type::f64()});
            d->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), d);
            ++loopIndex;
            dv[id] = d->result(0);
            if (!c.isInt) continue;
            Op* ok = make("xyo.int_exact_ok", {g->result(0)},
                          {Type::boolean()}, {{"nonneg", Attr::integer(1)}});
            ok->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), ok);
            ++loopIndex;
            oks.push_back(ok->result(0));
            Op* t = make("xyo.to_int", {g->result(0)}, {Type::i32()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), t);
            ++loopIndex;
            tv[id] = t->result(0);
        }
        Value guardVal = oks[0];
        for (size_t i = 1; i < oks.size(); ++i) {
            Op* a = make("xyo.and", {guardVal, oks[i]}, {Type::boolean()});
            a->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), a);
            ++loopIndex;
            guardVal = a->result(0);
        }
        // (2) if(guard) { intループ; 書き戻し } else { }。
        Op* ifOp = make("xyo.if", {guardVal}, {});
        Region* rThen = ifOp->addRegion();
        Block* thenB = rThen->addBlock();
        Region* rElse = ifOp->addRegion();
        Block* elseB = rElse->addBlock();
        ifOp->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex), ifOp);
        ++loopIndex;
        Cloner cl{m};
        Op* intLoop = cl.cloneInto(loop, *thenB);
        Block* intCond = intLoop->region(0)->entry();
        Block* intBody = intLoop->region(1)->entry();
        auto it = std::find(parent.ops.begin(), parent.ops.end(), loop);
        if (it != parent.ops.end()) parent.ops.erase(it);
        Promoter pi(m, *thenB, 0, intLoop, *intBody, {}, {}, {}, stats);
        pi.cands = cands;
        pi.order = order;
        pi.warpLoop = warpLoop;
        pi.intLoop = true;
        pi.boxLoop_ = true;
        // while の複製パスは (bits,f64) ペア前提の writeback なので
        // f64 運搬は使わない（kinds は pt/pe が決める）。
        pi.planKinds(false);
        for (auto& [o, k] : modSubK) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.modSubK[f->second] = k;
        for (auto& [o, r] : divRanges_) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.divRanges_[f->second] = r;
        }
        }
        for (Op* o : intProvenOps) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.intProvenOps.insert(f->second);
        }
        pi.wireIntWhile(gv, dv, tv);
        pi.condTerm = true;
        pi.rewriteBlock(*intCond);
        pi.condTerm = false;
        // cond に inrange を足す（int 変数だけ。ボックスは [0,2^20]）。
        {
            Op* term = intCond->lastOp();
            if (!term || term->name != "xyo.condition" ||
                term->operands.empty()) {
                return false;
            }
            Value cond = term->operands[0];
            Op* hid = pi.make("xyo.const.i", {}, {Type::i32()},
                              {{"v", Attr::integer(kGuardBound)}});
            hid->parent = intCond;
            std::vector<Op*> chk{hid};
            for (const std::string& id : order) {
                if (!pi.cands[id].isInt) continue;
                Value iv = pi.cands[id].iv;
                Op* le = pi.make("xyo.cmp.i", {iv, hid->result(0)},
                                 {Type::boolean()}, {{"pred", Attr::integer(6)}});
                le->parent = intCond;
                chk.push_back(le);
                Op* nc = pi.make("xyo.and", {cond, le->result(0)},
                                 {Type::boolean()});
                nc->parent = intCond;
                chk.push_back(nc);
                cond = nc->result(0);
            }
            // cond を差し替える前に、term より前に chk を挿す。
            auto pos = std::find(intCond->ops.begin(), intCond->ops.end(), term);
            intCond->ops.insert(pos, chk.begin(), chk.end());
            term->operands[0] = cond;
        }
        pi.resetBodyShadows();
        pi.rewriteBlock(*intBody);
        {   // then 内で書き戻し（int → 実メモリ）。
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = pi.cands[id];
                Value wv;
                if (c.isInt) {
                    Op* fx = pi.make("xyo.from_int", {intLoop->result(roff)},
                                     {Type::value()});
                    fx->parent = thenB;
                    thenB->ops.push_back(fx);
                    wv = fx->result(0);
                    ++roff;
                } else if (pi.kindOf(id) == 'd') {
                    Op* bx = pi.make("xyo.box.f", {intLoop->result(roff)},
                                     {Type::value()});
                    bx->parent = thenB;
                    thenB->ops.push_back(bx);
                    wv = bx->result(0);
                    ++roff;
                } else {
                    wv = intLoop->result(roff);
                    roff += 2;
                }
                Op* st = pi.make("xyo.setvar", {wv}, {},
                                 {{"id", Attr::str(c.id)},
                                  {"name", Attr::str(c.name)}});
                st->parent = thenB;
                thenB->ops.push_back(st);
            }
            // if は結果を持たないので、then の終端は空 yield。
            Op* y = pi.make("xyo.yield", {}, {});
            y->parent = thenB;
            thenB->ops.push_back(y);
        }
        {
            Op* y = make("xyo.yield", {}, {});
            y->parent = elseB;
            elseB->ops.push_back(y);
        }
        // (3) 後続の float ループ（元の意味論。int ループが範囲外で抜けた
        // 場合の引き継ぎ。cond が偽なら 0 回反復）。
        Op* floatLoop = cl.cloneInto(loop, parent);
        size_t flPos = parent.ops.size() - 1;
        // cl.cloneInto は末尾に push するので、順序を直す（if の直後）。
        {
            Op* tail = parent.ops.back();
            parent.ops.pop_back();
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), tail);
        }
        Block* fCond = floatLoop->region(0)->entry();
        Block* fBody = floatLoop->region(1)->entry();
        Promoter pf(m, parent, loopIndex, floatLoop, *fBody, {}, {}, {}, stats);
        pf.cands = cands;
        for (auto& [id, c] : pf.cands) c.isInt = false;
        pf.order = order;
        pf.warpLoop = warpLoop;
        pf.planKinds(false);
        // int ループが書き戻した最新値を読む（gv/dv は if の前の値なので
        // 使えない。使うと継続ループが古い値から再実行してしまう）。
        std::map<std::string, Value> gv2, dv2;
        for (const std::string& id : order) {
            LoopCandidate& c = pf.cands[id];
            Op* g = pf.make("xyo.getvar", {}, {Type::value()},
                            {{"id", Attr::str(c.id)},
                             {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), g);
            ++loopIndex;
            gv2[id] = g->result(0);
            Op* d = pf.make("xyo.to_num", {g->result(0)}, {Type::f64()});
            d->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex), d);
            ++loopIndex;
            dv2[id] = d->result(0);
        }
        pf.wireFloatWhile(gv2, dv2);
        pf.condTerm = true;
        pf.rewriteBlock(*fCond);
        pf.condTerm = false;
        pf.resetBodyShadows();
        pf.rewriteBlock(*fBody);
        // (4) float ループの書き戻し。
        {
            std::vector<Op*> wb;
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = pf.cands[id];
                Value wv;
                if (pf.kindOf(id) == 'd') {
                    Op* bx = pf.make("xyo.box.f", {floatLoop->result(roff)},
                                     {Type::value()});
                    bx->parent = &parent;
                    wb.push_back(bx);
                    wv = bx->result(0);
                    ++roff;
                } else {
                    wv = floatLoop->result(roff);
                    roff += 2;
                }
                Op* st = pf.make("xyo.setvar", {wv}, {},
                                 {{"id", Attr::str(c.id)},
                                  {"name", Attr::str(c.name)}});
                st->parent = &parent;
                wb.push_back(st);
            }
            for (size_t i = 0; i < wb.size(); ++i)
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex + 1 + i),
                                  wb[i]);
        }
        (void)flPos;
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            ++stats->guardedLoops;
            ++stats->intLoops;
        }
        return true;
    }

    /// 本体に loop_yield/warp_yield が末尾の結果計算以外にあるか。
    /// reload された値は検証前に .i 演算へ流れるため、途中 yield のある
    /// ループは box-repeat の対象外にする（従来の float へ）。
    /// 終端 yield は末尾の getvar/to_num/box.f/from_int/to_int/const/yield
    /// だけが後に続く場合に許す。
    bool hasMidBodyYield() {
        auto isPlumbing = [](const Op* o) {
            const std::string& n = o->name;
            if (n == "xyo.getvar" || n == "xyo.to_num" ||
                n == "xyo.box.f" || n == "xyo.from_int" ||
                n == "xyo.to_int" || n == "xyo.const" ||
                n == "xyo.const.f64" || n == "xyo.const.i" ||
                n == "xyo.yield")
                return o->regions.empty();
            return false;
        };
        std::function<bool(Block&)> scan = [&](Block& blk) -> bool {
            for (size_t i = 0; i < blk.ops.size(); ++i) {
                Op* o = blk.ops[i];
                if (o->name == "xyo.loop_yield" ||
                    o->name == "xyo.warp_yield") {
                    for (size_t j = i + 1; j < blk.ops.size(); ++j)
                        if (!isPlumbing(blk.ops[j])) return true;
                    continue;
                }
                for (auto& r : o->regions) {
                    if (!r) continue;
                    for (auto& cb : r->blocks)
                        if (scan(*cb)) return true;
                }
            }
            return false;
        };
        return scan(body);
    }

    /// ボックス版 repeat（途中脱出つき整数化）。
    /// 前提: boxMap+clampToBox+relaxClean の analyzeIntLoop が成功し、
    /// cands[].isInt と order が確定。loop は xyo.repeat。
    /// 入口ガード＋trip ガードで int ループへ入り、各反復末尾の inrange
    /// で範囲外・汚染を検出して bail し、float ループで最初から
    /// やり直す（継続ではなく再実行のため残り回数計算が要らない）。
    /// bail は稀なはず（通常は全反復 int で完走する）。
    bool runBoxRepeat() {
        if (loop->name != "xyo.repeat") return false;
        if (loop->operands.empty()) return false;
        bool hasInt = false;
        for (const std::string& id : order)
            if (cands[id].isInt) {
                hasInt = true;
                break;
            }
        if (!hasInt) return false;
        Value tripVal = loop->operands[0];
        if (hasMidBodyYield()) {
            intTrace("box-repeat mid-body yield reject (%s)", std::string());
            return false;
        }
        // (1) 入口ガード＋trip ガード。
        std::map<std::string, Value> gv, dv, tv;
        std::vector<Value> oks;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            Op* g = make("xyo.getvar", {}, {Type::value()},
                         {{"id", Attr::str(c.id)},
                          {"name", Attr::str(c.name)}});
            g->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              g);
            ++loopIndex;
            gv[id] = g->result(0);
            Op* d = make("xyo.to_num", {g->result(0)}, {Type::f64()});
            d->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              d);
            ++loopIndex;
            dv[id] = d->result(0);
            if (!c.isInt) continue;
            Op* ok = make("xyo.int_exact_ok", {g->result(0)},
                          {Type::boolean()},
                          {{"nonneg", Attr::integer(1)}});
            ok->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              ok);
            ++loopIndex;
            oks.push_back(ok->result(0));
            Op* t = make("xyo.to_int", {g->result(0)}, {Type::i32()});
            t->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              t);
            ++loopIndex;
            tv[id] = t->result(0);
        }
        // trip ガード（整数・非負・2^20 以下）。
        // int_exact_ok はビット列を取る（to_num した f64 ではない）。
        // trip が文字列リテラル（"10000" 等の文字列ボックス）の場合、
        // int_exact_ok は false と畳まれ int ループ全体が死ぬ。
        // repeat は入力を to_num するので数値 const へ正規化してよい。
        if (tripVal.op) {
            uint64_t tb = 0;
            if (constBitsOf(tripVal.op, &tb) && !isNumericBits(tb)) {
                Op* tc = make("xyo.const", {}, {Type::value()},
                              {{"bits", Attr::integer(
                                            static_cast<int64_t>(
                                                xyo::Value::numCanonical(
                                                    xyo_to_num(tb)).bits))}});
                tc->parent = &parent;
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(loopIndex),
                                  tc);
                ++loopIndex;
                tripVal = tc->result(0);
            }
        }
        Op* tripOk = make("xyo.int_exact_ok", {tripVal},
                          {Type::boolean()},
                          {{"nonneg", Attr::integer(1)}});
        tripOk->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          tripOk);
        ++loopIndex;
        oks.push_back(tripOk->result(0));
        Value guardVal = oks[0];
        for (size_t i = 1; i < oks.size(); ++i) {
            Op* a = make("xyo.and", {guardVal, oks[i]}, {Type::boolean()});
            a->parent = &parent;
            parent.ops.insert(parent.ops.begin() +
                                  static_cast<ptrdiff_t>(loopIndex),
                              a);
            ++loopIndex;
            guardVal = a->result(0);
        }
        // (2) if(guardVal) { intloop; } else { dummy }。
        // 結果は (vars..., bailed)。else 分岐の値は使われない。
        std::vector<Type> ifResults;
        for (const std::string& id : order) {
            LoopCandidate& c = cands[id];
            if (c.isInt)
                ifResults.push_back(Type::i32());
            else if (kindOf(id) == 'd')
                ifResults.push_back(Type::f64());
            else {
                ifResults.push_back(Type::value());
                ifResults.push_back(Type::f64());
            }
        }
        ifResults.push_back(Type::i32());  // bailed flag
        Op* ifOp = make("xyo.if", {guardVal}, std::move(ifResults));
        Region* rThen = ifOp->addRegion();
        Block* thenB = rThen->addBlock();
        Region* rElse = ifOp->addRegion();
        Block* elseB = rElse->addBlock();
        ifOp->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          ifOp);
        ++loopIndex;
        Cloner cl{m};
        Op* intLoop = cl.cloneInto(loop, *thenB);
        // float 継続用に元ループは親に残す（後で needFloat 側へ移す）。
        Promoter pi(m, *thenB, 0, intLoop,
                    *intLoop->region(0)->entry(), {}, {}, {}, stats);
        pi.cands = cands;
        pi.order = order;
        pi.warpLoop = warpLoop;
        pi.intLoop = true;
        pi.boxLoop_ = true;
        pi.planKinds(true);
        for (auto& [o, k] : modSubK) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.modSubK[f->second] = k;
        for (auto& [o, r] : divRanges_) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.divRanges_[f->second] = r;
        }
        }
        for (Op* o : intProvenOps) {
            auto f = cl.opMap.find(o);
            if (f != cl.opMap.end()) pi.intProvenOps.insert(f->second);
        }
        pi.wireInt(gv, dv, tv);
        Block* intBody = intLoop->region(0)->entry();
        Op* origYield = intBody->lastOp();
        // bail 不要証明: trip が定数で、全 int 変数の反復全体の区間が
        // [0, 2^20] に収まるなら guardian if と bail フラグは発火しない。
        // その場合はフラグ運搬と毎反復の if を丸ごと省く。
        bool noBail = false;
        {
            double tc = 0;
            const int64_t tb =
                constNumValue(tripVal, &tc) ? repeatTripCount(tc) : -1;
            if (tb >= 0) {
                std::unordered_map<std::string,
                                   std::pair<int64_t, int64_t>> iv;
                for (auto& [i2, c2] : cands)
                    if (c2.isInt) iv[i2] = {c2.intLo, c2.intHi};
                noBail = true;
                for (const std::string& id : order) {
                    const LoopCandidate& c = cands[id];
                    if (!c.isInt) continue;
                    // 入口が実行時検査（intGuarded）の変数は入口区間を
                    // 静かに持たないので消去対象外。
                    if (c.intGuarded) {
                        noBail = false;
                        break;
                    }
                    int64_t blo = 0, bhi = 0;
                    if (!repeatBoundOf(id, tb, iv, &blo, &bhi) ||
                        blo < 0 || bhi > kGuardBound) {
                        noBail = false;
                        break;
                    }
                }
            }
        }
        // 入口シャドウを保存（else 分岐用）。
        struct Shadow3 {
            Value bits, d, iv;
        };
        std::map<std::string, Shadow3> entrySh;
        for (auto& [id, c] : pi.cands)
            entrySh[id] = {c.bits, c.d, c.iv};
        if (noBail) {
            // guardian 無し: 本体をそのまま int 運搬で書き換え、
            // 終端 yield は変数値をそのまま運ぶ。
            pi.rewriteBlock(*intBody);
            std::vector<Value> outs;
            for (const std::string& id : order) {
                LoopCandidate& c = pi.cands[id];
                if (c.isInt)
                    outs.push_back(c.iv);
                else if (pi.kindOf(id) == 'd')
                    outs.push_back(c.d);
                else {
                    outs.push_back(c.bits);
                    outs.push_back(c.d);
                }
            }
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            auto it = std::find(intBody->ops.begin(), intBody->ops.end(),
                                origYield);
            if (it != intBody->ops.end()) {
                y->parent = intBody;
                *it = y;
            } else {
                y->parent = intBody;
                intBody->ops.push_back(y);
            }
        } else {
        // bail フラグの iter arg を追加（i32、初期値 0）。
        Op* flagInit = pi.make("xyo.const.i", {}, {Type::i32()},
                               {{"v", Attr::integer(0)}});
        flagInit->parent = thenB;
        {
            auto it = std::find(thenB->ops.begin(), thenB->ops.end(),
                                intLoop);
            if (it != thenB->ops.end())
                thenB->ops.insert(it, flagInit);
            else
                thenB->ops.push_back(flagInit);
        }
        intLoop->operands.push_back(flagInit->result(0));
        intLoop->resultTypes.push_back(Type::i32());
        Value flagArg;
        flagArg.block = intBody;
        flagArg.argIndex = static_cast<uint32_t>(intBody->args.size());
        flagArg.type = Type::i32();
        intBody->args.push_back(flagArg);
        // ガーディアン if(!bailed){本体} を作り、本体 ops を then へ移す。
        Op* grd = pi.make("xyo.if", {}, {});
        grd->parent = intBody;
        intBody->ops.insert(intBody->ops.begin(), grd);
        Region* gThenR = grd->addRegion();
        Block* gThenB = gThenR->addBlock();
        Region* gElseR = grd->addRegion();
        Block* gElseB = gElseR->addBlock();
        // 終端 yield を除く本体 ops を then へ移動する。
        std::vector<Op*> moveOps;
        for (auto it = intBody->ops.begin(); it != intBody->ops.end();
             ++it) {
            Op* o = *it;
            if (o == grd || o == origYield) continue;
            moveOps.push_back(o);
        }
        for (Op* o : moveOps) {
            auto it =
                std::find(intBody->ops.begin(), intBody->ops.end(), o);
            if (it != intBody->ops.end()) intBody->ops.erase(it);
            o->parent = gThenB;
            gThenB->ops.push_back(o);
        }
        // ガード条件 flag==0（親ブロックで計算）。
        Op* gZero = pi.make("xyo.const.i", {}, {Type::i32()},
                            {{"v", Attr::integer(0)}});
        gZero->parent = intBody;
        Op* gOne = pi.make("xyo.const.i", {}, {Type::i32()},
                           {{"v", Attr::integer(1)}});
        gOne->parent = intBody;
        {
            auto gpos =
                std::find(intBody->ops.begin(), intBody->ops.end(), grd);
            intBody->ops.insert(gpos, {gZero, gOne});
        }
        Op* gCmp = pi.make("xyo.cmp.i", {flagArg, gZero->result(0)},
                           {Type::boolean()}, {{"pred", Attr::integer(4)}});
        gCmp->parent = intBody;
        {
            auto gpos =
                std::find(intBody->ops.begin(), intBody->ops.end(), grd);
            intBody->ops.insert(gpos, gCmp);
        }
        grd->operands.push_back(gCmp->result(0));
        // then 側を書き換える（int 運搬）。
        pi.rewriteBlock(*gThenB);
        // 反復末尾の inrange 検査（int 変数のみ）＋bail 更新。
        // [0, 2^20] の検査は符号なし <= 1 命令で等価
        // （負数は符号なしで巨大値になり自動で偽）。
        Op* inHi = pi.make("xyo.const.i", {}, {Type::i32()},
                           {{"v", Attr::integer(kGuardBound)}});
        inHi->parent = gThenB;
        gThenB->ops.push_back(inHi);
        Value inOk;
        bool haveInOk = false;
        for (const std::string& id : order) {
            LoopCandidate& c = pi.cands[id];
            if (!c.isInt) continue;
            Op* le = pi.make("xyo.cmp.i", {c.iv, inHi->result(0)},
                             {Type::boolean()}, {{"pred", Attr::integer(6)}});
            le->parent = gThenB;
            gThenB->ops.push_back(le);
            if (!haveInOk) {
                inOk = le->result(0);
                haveInOk = true;
            } else {
                Op* nc = pi.make("xyo.and", {inOk, le->result(0)},
                                 {Type::boolean()});
                nc->parent = gThenB;
                gThenB->ops.push_back(nc);
                inOk = nc->result(0);
            }
        }
        // bail' = !ok（then 内では flag==0 が保証される）。
        // hasInt が真なので int 変数は必ず1つ以上あり、haveInOk は真。
        (void)haveInOk;
        Op* flagNew = pi.make("xyo.select", {inOk, gZero->result(0),
                                             gOne->result(0)},
                              {Type::i32()});
        flagNew->parent = gThenB;
        gThenB->ops.push_back(flagNew);
        {
            std::vector<Value> outs;
            for (const std::string& id : order) {
                LoopCandidate& c = pi.cands[id];
                if (c.isInt)
                    outs.push_back(c.iv);
                else if (pi.kindOf(id) == 'd')
                    outs.push_back(c.d);
                else {
                    outs.push_back(c.bits);
                    outs.push_back(c.d);
                }
            }
            outs.push_back(flagNew->result(0));
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            y->parent = gThenB;
            gThenB->ops.push_back(y);
        }
        // else 分岐: 変更なし＋bail=1（到達はしないが型を揃える）。
        {
            std::vector<Value> outs;
            for (const std::string& id : order) {
                Shadow3& e = entrySh[id];
                if (pi.cands[id].isInt)
                    outs.push_back(e.iv);
                else if (pi.kindOf(id) == 'd')
                    outs.push_back(e.d);
                else {
                    outs.push_back(e.bits);
                    outs.push_back(e.d);
                }
            }
            outs.push_back(gOne->result(0));
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            y->parent = gElseB;
            gElseB->ops.push_back(y);
        }
        // ガーディアン if の結果型を確定する。
        grd->resultTypes.clear();
        for (const std::string& id : order) {
            LoopCandidate& c = pi.cands[id];
            if (c.isInt)
                grd->resultTypes.push_back(Type::i32());
            else if (pi.kindOf(id) == 'd')
                grd->resultTypes.push_back(Type::f64());
            else {
                grd->resultTypes.push_back(Type::value());
                grd->resultTypes.push_back(Type::f64());
            }
        }
        grd->resultTypes.push_back(Type::i32());
        // 本体終端 yield（ガーディアン結果をそのまま通す）。
        {
            std::vector<Value> outs;
            for (size_t i = 0; i < grd->resultTypes.size(); ++i)
                outs.push_back(grd->result(i));
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            auto it = std::find(intBody->ops.begin(), intBody->ops.end(),
                                origYield);
            if (it != intBody->ops.end()) {
                y->parent = intBody;
                *it = y;
            } else {
                y->parent = intBody;
                intBody->ops.push_back(y);
            }
        }
        }  // !noBail
        // (3) then 分岐の終端: int ループ結果をそのまま yield。
        // noBail では int ループが flag を持たないので末尾に 0 を足す。
        {
            std::vector<Value> outs;
            for (size_t i = 0; i < intLoop->resultTypes.size(); ++i)
                outs.push_back(intLoop->result(i));
            if (noBail) {
                Op* z = pi.make("xyo.const.i", {}, {Type::i32()},
                                {{"v", Attr::integer(0)}});
                z->parent = thenB;
                thenB->ops.push_back(z);
                outs.push_back(z->result(0));
            }
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            y->parent = thenB;
            thenB->ops.push_back(y);
        }
        // else 分岐: 使われないダミー（needFloat=true になる）。
        {
            std::vector<Value> outs;
            for (const std::string& id : order) {
                LoopCandidate& c = pi.cands[id];
                if (c.isInt) {
                    Op* z = pi.make("xyo.const.i", {}, {Type::i32()},
                                    {{"v", Attr::integer(0)}});
                    z->parent = elseB;
                    elseB->ops.push_back(z);
                    outs.push_back(z->result(0));
                } else if (pi.kindOf(id) == 'd') {
                    Op* z = pi.make("xyo.const.f64", {}, {Type::f64()},
                                    {{"d", Attr::real(0.0)}});
                    z->parent = elseB;
                    elseB->ops.push_back(z);
                    outs.push_back(z->result(0));
                } else {
                    Op* z = pi.make("xyo.const", {}, {Type::value()},
                                    {{"bits", Attr::integer(0)}});
                    z->parent = elseB;
                    elseB->ops.push_back(z);
                    outs.push_back(z->result(0));
                    Op* z2 = pi.make("xyo.const.f64", {}, {Type::f64()},
                                     {{"d", Attr::real(0.0)}});
                    z2->parent = elseB;
                    elseB->ops.push_back(z2);
                    outs.push_back(z2->result(0));
                }
            }
            Op* one = pi.make("xyo.const.i", {}, {Type::i32()},
                              {{"v", Attr::integer(1)}});
            one->parent = elseB;
            elseB->ops.push_back(one);
            outs.push_back(one->result(0));
            Op* y = pi.make("xyo.yield", std::move(outs), {});
            y->parent = elseB;
            elseB->ops.push_back(y);
        }
        // (4) if(needFloat){ float; wb-f } else { wb-i }。
        // bailed は i32 なので i1 に変換してから分岐する。
        Value needFloatRaw = ifOp->result(ifOp->resultTypes.size() - 1);
        Op* needZero = make("xyo.const.i", {}, {Type::i32()},
                            {{"v", Attr::integer(0)}});
        needZero->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          needZero);
        ++loopIndex;
        Op* needCmp = make("xyo.cmp.i", {needFloatRaw, needZero->result(0)},
                           {Type::boolean()}, {{"pred", Attr::integer(5)}});
        needCmp->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          needCmp);
        ++loopIndex;
        Value needFloat = needCmp->result(0);
        std::vector<Value> intVals;
        for (size_t i = 0; i + 1 < ifOp->resultTypes.size(); ++i)
            intVals.push_back(ifOp->result(i));
        Op* finIf = make("xyo.if", {needFloat}, {});
        Region* fThenR = finIf->addRegion();
        Block* fThenB = fThenR->addBlock();
        Region* fElseR = finIf->addRegion();
        Block* fElseB = fElseR->addBlock();
        finIf->parent = &parent;
        parent.ops.insert(parent.ops.begin() +
                              static_cast<ptrdiff_t>(loopIndex),
                          finIf);
        ++loopIndex;
        // float ループ: 元ループを移す。
        {
            auto it =
                std::find(parent.ops.begin(), parent.ops.end(), loop);
            if (it != parent.ops.end()) parent.ops.erase(it);
            loop->parent = fThenB;
            fThenB->ops.push_back(loop);
        }
        Promoter pf(m, *fThenB, 0, loop, body, {}, {}, {}, stats);
        pf.cands = cands;
        for (auto& [id, c] : pf.cands) c.isInt = false;
        pf.order = order;
        pf.warpLoop = warpLoop;
        pf.planKinds(true);
        // bail 時は元の入口値で最初からやり直す（継続ではない）。
        // gv/dv はガード前の SSA 値なので int ループの spill に汚染されない。
        pf.wireFloat(gv, dv);
        pf.rewriteBlock(body);
        // float 書き戻し（ループ結果をそのまま書く。0 回反復なら
        // lowering が init を返すので select は要らない）。
        {
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = pf.cands[id];
                Value wv;
                if (pf.kindOf(id) == 'd') {
                    Op* bx = pf.make("xyo.box.f",
                                     {loop->result(roff)}, {Type::value()});
                    bx->parent = fThenB;
                    fThenB->ops.push_back(bx);
                    wv = bx->result(0);
                    ++roff;
                } else {
                    wv = loop->result(roff);
                    roff += 2;
                }
                Op* st = pf.make("xyo.setvar", {wv}, {},
                                 {{"id", Attr::str(c.id)},
                                  {"name", Attr::str(c.name)}});
                st->parent = fThenB;
                fThenB->ops.push_back(st);
            }
            Op* y = pf.make("xyo.yield", {}, {});
            y->parent = fThenB;
            fThenB->ops.push_back(y);
        }
        // int 書き戻し（bail なし側）。0 回反復ではループ結果では
        // なく入口値を書く（commitRepeat と同じ select）。
        Op* wbTripNum = make("xyo.to_num", {tripVal}, {Type::f64()});
        wbTripNum->parent = fElseB;
        fElseB->ops.push_back(wbTripNum);
        Op* wbHalf = make("xyo.const.f64", {}, {Type::f64()},
                          {{"d", Attr::real(0.5)}});
        wbHalf->parent = fElseB;
        fElseB->ops.push_back(wbHalf);
        Op* wbRan = make("xyo.cmp.f", {wbTripNum->result(0),
                                       wbHalf->result(0)},
                         {Type::boolean()}, {{"pred", Attr::integer(3)}});
        wbRan->parent = fElseB;
        fElseB->ops.push_back(wbRan);
        {
            size_t roff = 0;
            for (const std::string& id : order) {
                LoopCandidate& c = cands[id];
                Value wv;
                if (c.isInt) {
                    Op* fx = make("xyo.from_int", {intVals[roff]},
                                  {Type::value()});
                    fx->parent = fElseB;
                    fElseB->ops.push_back(fx);
                    // 0 回反復では from_int ではなく元の実体を書く。
                    Op* sel = make("xyo.select",
                                   {wbRan->result(0), fx->result(0),
                                    gv.at(id)},
                                   {Type::value()});
                    sel->parent = fElseB;
                    fElseB->ops.push_back(sel);
                    wv = sel->result(0);
                    ++roff;
                } else if (kindOf(id) == 'd') {
                    Op* bx = make("xyo.box.f", {intVals[roff]},
                                  {Type::value()});
                    bx->parent = fElseB;
                    fElseB->ops.push_back(bx);
                    // 0 回反復では f64 影ではなく元の実体を書く
                    // （文字列リテラルの型を失わない）。
                    Op* sel = make("xyo.select",
                                   {wbRan->result(0), bx->result(0),
                                    gv.at(id)},
                                   {Type::value()});
                    sel->parent = fElseB;
                    fElseB->ops.push_back(sel);
                    wv = sel->result(0);
                    ++roff;
                } else {
                    wv = intVals[roff];
                    roff += 2;
                }
                Op* st = make("xyo.setvar", {wv}, {},
                              {{"id", Attr::str(c.id)},
                               {"name", Attr::str(c.name)}});
                st->parent = fElseB;
                fElseB->ops.push_back(st);
            }
            Op* y = make("xyo.yield", {}, {});
            y->parent = fElseB;
            fElseB->ops.push_back(y);
        }
        if (stats) {
            ++stats->promotedLoops;
            stats->promotedVars += order.size();
            ++stats->guardedLoops;
            ++stats->intLoops;
        }
        return true;
    }

    void commitRepeat(Op* resOp) {
        // 3) ループ脱出時の書き戻し（0 回反復では初期値のまま）。
        // resOp はループ自身かガード版の xyo.if（どちらも (bits,f64)×
        // 変数の結果を持つ）。ループ自身の int 運搬のときだけ
        // from_int で戻す。
        const Value countOperand =
            loop->operands.empty() ? Value{} : loop->operands[0];
        const bool directInt = (resOp == loop);
        if (loop->name == "xyo.repeat" && countOperand.op) {
            // 反復回数が整数運搬の SSA（i32）になっていることがある。
            // to_num は Value を取るので、その場合は先に box する。
            std::vector<Op*> commit;
            Value countBits = bitsOf(countOperand, parent, commit);
            Op* num = m.create("xyo.to_num", {countBits}, {Type::f64()});
            Op* half = m.create("xyo.const.f64", {}, {Type::f64()},
                                {{"d", Attr::real(0.5)}});
            Op* ran = m.create("xyo.cmp.f", {num->result(0), half->result(0)},
                               {Type::boolean()}, {{"pred", Attr::integer(3)}});
            num->parent = &parent;
            half->parent = &parent;
            ran->parent = &parent;
            commit.push_back(num);
            commit.push_back(half);
            commit.push_back(ran);
            size_t roff = 0;
            for (size_t i = 0; i < order.size(); ++i) {
                LoopCandidate& c = cands[order[i]];
                Value resBits;
                if (directInt && intLoop && c.isInt) {
                    Op* fx = m.create("xyo.from_int", {resOp->result(roff)},
                                       {Type::value()});
                    fx->parent = &parent;
                    commit.push_back(fx);
                    resBits = fx->result(0);
                    ++roff;
                } else if (kindOf(c.id) == 'd') {
                    // f64 運搬の変数は結果を box して Value に戻す。
                    Op* bx = m.create("xyo.box.f", {resOp->result(roff)},
                                       {Type::value()});
                    bx->parent = &parent;
                    commit.push_back(bx);
                    resBits = bx->result(0);
                    ++roff;
                } else {
                    resBits = resOp->result(roff);
                    roff += 2;
                }
                Op* sel = m.create("xyo.select",
                                   {ran->result(0), resBits,
                                    c.initBits},
                                   {Type::value()});
                Op* st = m.create("xyo.setvar", {sel->result(0)}, {},
                                  {{"id", Attr::str(c.id)},
                                   {"name", Attr::str(c.name)}});
                sel->parent = &parent;
                st->parent = &parent;
                commit.push_back(sel);
                commit.push_back(st);
            }
            // resOp の実位置の直後へ挿入する。loopIndex はガード版では
            // ifOp 挿入のたびに進んでいるため +1 では resOp を 1 つ
            // 飛び越えてしまい、func.return の後に commit が置かれて
            // 「terminator is not the last op」で検証失敗していた。
            size_t resIndex = loopIndex;
            for (size_t ri = 0; ri < parent.ops.size(); ++ri) {
                if (parent.ops[ri] == resOp) {
                    resIndex = ri;
                    break;
                }
            }
            for (size_t i = 0; i < commit.size(); ++i)
                parent.ops.insert(parent.ops.begin() +
                                      static_cast<ptrdiff_t>(resIndex + 1 + i),
                                  commit[i]);
        }
    }
};

} // namespace

void canonicalize(Module& m, PassStats* stats) {
    Folder f{m, stats, 0};
    walkBlocks(m, [&](Block& b) { f.run(b); });
    if (stats) stats->folded += f.folded;
}

void cse(Module& m, PassStats* stats) {
    uint64_t hits = 0;
    walkBlocks(m, [&](Block& b) {
        std::map<std::string, Value> table;
        std::vector<Op*> dead;
        for (Op* op : b.ops) {
            if (dialect::isTerminator(op->name)) continue;
            const bool pure = cseEligible(*op);
            if (!pure) {
                // 副作用・メモリ読みの後では、同じ SSA でも値が変わり得る。
                table.clear();
                continue;
            }
            std::string key = cseKey(*op);
            auto it = table.find(key);
            if (it != table.end()) {
                replaceAllUses(m, op, 0, it->second);
                dead.push_back(op);
                ++hits;
            } else {
                table.emplace(std::move(key), op->result(0));
            }
        }
        for (Op* op : dead) eraseFromBlock(b, op);
    });
    if (stats) stats->cse += hits;
}

/// if 領域内の純粋式を親ブロックの同一計算へ統合する（領域またぎ CSE）。
/// 昇格が領域内に重複計算を作る（swap の読み直し等）ため、親ですでに
/// 計算済みの値を使い回す。意味は変えない（純粋 op のみ・読みは
/// 同一リストへの先行書き込みが無い場合だけ）。書き換えは領域内のみで、
/// 親ブロックには触らない（追加もしない＝投機実行なし）。
void hoistIfDuplicates(Module& m, PassStats* stats) {
    uint64_t hits = 0;
    // 値を一意に識別する（定数は値で、他は SSA 位置で）。
    auto valKey = [&](const Value& v) -> std::string {
        if (!v.op) {
            if (!v.block) return "?";
            return "b" + std::to_string((uintptr_t)v.block) + ":" +
                   std::to_string(v.argIndex);
        }
        const std::string& n = v.op->name;
        if (n == "xyo.const") {
            const Attr* a = v.op->attr("bits");
            if (a && a->isInt())
                return "c:i" + std::to_string(a->i);
            return "?";
        }
        if (n == "xyo.const.f64") {
            const Attr* a = v.op->attr("d");
            if (a && a->isDouble()) {
                uint64_t u = 0;
                double d = a->d;
                static_assert(sizeof(u) == sizeof(d));
                std::memcpy(&u, &d, sizeof(u));
                return "c:d" + std::to_string(u);
            }
            return "?";
        }
        if (n == "xyo.const.i") {
            return "c:I" + std::to_string(v.op->attrInt("v"));
        }
        return "o" + std::to_string((uintptr_t)v.op) + ":" +
               std::to_string(v.index);
    };
    auto attrKey = [&](const Op* op) {
        std::string k;
        for (auto& [an, a] : op->attrs) {
            k += an;
            k += '=';
            if (a.isInt())
                k += std::to_string(a.i);
            else if (a.isDouble()) {
                uint64_t u = 0;
                double d = a.d;
                std::memcpy(&u, &d, sizeof(u));
                k += std::to_string(u);
            } else
                k += a.s;
            k += ';';
        }
        return k;
    };
    // リスト id の取得（xyo.list.ptr のみ）。
    auto listIdOf = [&](const Value& v) -> std::string {
        if (!v.op || v.op->name != "xyo.list.ptr") return std::string();
        return std::string(v.op->attrStr("id")) + "\x1f" +
               std::string(v.op->attrStr("name"));
    };
    std::function<void(Block&)> walk = [&](Block& b) {
        for (size_t bi = 0; bi < b.ops.size(); ++bi) {
            Op* op = b.ops[bi];
            for (auto& r : op->regions)
                if (r)
                    for (auto& cb : r->blocks) walk(*cb);
            if (op->name != "xyo.if" || op->regions.size() != 2) continue;
            if (std::getenv("XYO_MLIR_HOISTDBG"))
                std::fprintf(stderr, "[mlir-hoist] if found\n");
            Block* rb[2] = {nullptr, nullptr};
            bool okShape = true;
            for (int r = 0; r < 2; ++r) {
                if (!op->region(r) || op->region(r)->blocks.size() != 1) {
                    okShape = false;
                    break;
                }
                rb[r] = op->region(r)->entry();
            }
            if (!okShape) continue;
            // 親スコープ: if より前の純粋 op と読み（読みはストアで無効化）。
            std::map<std::string, Value> scope;
            std::map<std::string, std::string> readLists;  // key -> list id
            auto listStoreId = [&](Op* o) -> std::string {
                if (o->name != "xyo.list.replace" &&
                    o->name != "xyo.list.replace.i" &&
                    o->name != "xyo.list.add" &&
                    o->name != "xyo.list.insert" &&
                    o->name != "xyo.list.delete" &&
                    o->name != "xyo.list.deleteall")
                    return std::string();
                if (o->operands.empty()) return std::string();
                // 不明なリストへの書き込みは "" ではなく特殊扱いする
                // ため、空文字は返さない（呼び出し側で区別）。
                std::string id = listIdOf(o->operands[0]);
                return id.empty() ? std::string("\xff") : id;
            };
            auto scopeKey = [&](Op* o,
                                const std::vector<Value>& args) -> std::string {
                std::string k = o->name;
                k += '(';
                for (auto& a : args) {
                    k += valKey(a);
                    k += ',';
                }
                k += ')';
                k += attrKey(o);
                return k;
            };
            for (Op* po : b.ops) {
                if (po == op) break;
                if (dialect::isTerminator(po->name)) continue;
                if (po->name == "xyo.list.replace" ||
                    po->name == "xyo.list.replace.i" ||
                    po->name == "xyo.list.add" ||
                    po->name == "xyo.list.insert" ||
                    po->name == "xyo.list.delete" ||
                    po->name == "xyo.list.deleteall") {
                    std::string lid = listStoreId(po);
                    if (lid.empty() || lid == "\xff") {
                        // 不明なストア: 読みの一致を全部捨てる。
                        for (auto it = scope.begin(); it != scope.end();) {
                            auto jt = readLists.find(it->first);
                            if (jt != readLists.end()) {
                                readLists.erase(jt);
                                it = scope.erase(it);
                            } else
                                ++it;
                        }
                    } else {
                        for (auto it = scope.begin(); it != scope.end();) {
                            auto jt = readLists.find(it->first);
                            if (jt != readLists.end() && jt->second == lid) {
                                readLists.erase(jt);
                                it = scope.erase(it);
                            } else
                                ++it;
                        }
                    }
                    continue;
                }
                if (po->name == "xyo.list.item" ||
                    po->name == "xyo.list.item.i" ||
                    po->name == "xyo.list.item.num" ||
                    po->name == "xyo.list.item.num.i" ||
                    po->name == "xyo.list.itemnum" ||
                    po->name == "xyo.list.length" ||
                    po->name == "xyo.list.contains" ||
                    po->name == "xyo.list.contents") {
                    // length/contents は operands[0]=list のみ。
                    bool noIndex = (po->name == "xyo.list.length" ||
                                    po->name == "xyo.list.contents");
                    if (po->operands.size() < (noIndex ? 1u : 2u)) continue;
                    std::string lid = listIdOf(po->operands[0]);
                    if (lid.empty()) continue;
                    std::vector<Value> args;
                    if (!noIndex) args.push_back(po->operands[1]);
                    std::string key = scopeKey(po, args);
                    // list id もキーに含める（同一 idx でも別リスト）。
                    key += "|L" + lid;
                    auto r = scope.emplace(key, po->result(0));
                    if (r.second) readLists[key] = lid;
                    if (std::getenv("XYO_MLIR_HOISTDBG"))
                        std::fprintf(stderr,
                                     "[mlir-hoist] parent-item %.120s\n",
                                     key.c_str());
                    continue;
                }
                if (!dialect::isPure(po->name)) {
                    // 副作用の可能性がある op の後では読み以外は有効。
                    // 純粋計算は SSA 値で不変なので残す。呼び出し・
                    // リージョン付き・yield 系は読みも捨てる。
                    bool unknownEffect = !po->regions.empty() ||
                                         po->name == "xyo.call" ||
                                         po->name == "xyo.loop_yield" ||
                                         po->name == "xyo.warp_yield";
                    if (!unknownEffect) {
                        bool mayStore = true;
                        // 純粋 op はメモリに触れないので常に有効。
                        if (dialect::isPure(po->name)) mayStore = false;
                        if (!mayStore) {
                            std::vector<Value> args;
                            for (auto& a : po->operands) args.push_back(a);
                            scope.emplace(scopeKey(po, args),
                                          po->result(0));
                        }
                    } else {
                        for (auto it = scope.begin();
                             it != scope.end();) {
                            if (readLists.count(it->first)) {
                                auto jt = readLists.find(it->first);
                                readLists.erase(jt);
                                it = scope.erase(it);
                            } else
                                ++it;
                        }
                    }
                    continue;
                }
                std::vector<Value> args;
                for (auto& a : po->operands) args.push_back(a);
                scope.emplace(scopeKey(po, args), po->result(0));
            }
            // 各領域を走査（読みの重複だけ除去。純粋計算の speculative
            // 持ち上げはしない）。
            for (int r = 0; r < 2; ++r) {
                Block* blk = rb[r];
                std::unordered_map<const Op*, Value> remap;
                std::unordered_set<std::string> stored;
                std::vector<Op*> dead;
                // 選択的 bail: 未知 op・呼び出し・ネスト制御・yield を
                // 見たら以降の読み hoist を止めるが、純粋計算は続行する
                // （純粋式は関数的なので位置によらず安全）。
                bool readsBlocked = false;
                auto resolve = [&](const Value& v, bool* unres) -> Value {
                    if (!v.op) {
                        // ブロック引数: 領域エントリのものは不可。
                        if (v.block == blk) {
                            *unres = true;
                            return v;
                        }
                        return v;
                    }
                    auto it = remap.find(v.op);
                    if (it != remap.end()) return it->second;
                    // 親スコープの op ならそのまま使える。
                    return v;
                };
                for (Op* o : blk->ops) {
                    const std::string& n = o->name;
                    if (n == "xyo.yield") {
                        for (auto& u : o->operands) {
                            bool un = false;
                            Value rv = resolve(u, &un);
                            if (!un) u = rv;
                        }
                        continue;
                    }
                    if (n == "xyo.setvar") {
                        if (!o->operands.empty()) {
                            bool un = false;
                            Value rv = resolve(o->operands[0], &un);
                            if (!un) o->operands[0] = rv;
                        }
                        continue;
                    }
                    if (n == "xyo.getvar" || n == "xyo.list.ptr") {
                        continue;  // そのまま（getvar は解決しない）。
                    }
                    if (n == "xyo.list.replace" ||
                        n == "xyo.list.replace.i" ||
                        n == "xyo.list.add" || n == "xyo.list.insert" ||
                        n == "xyo.list.delete" ||
                        n == "xyo.list.deleteall") {
                        if (!o->operands.empty()) {
                            std::string lid = listIdOf(o->operands[0]);
                            if (!lid.empty())
                                stored.insert(lid);
                            else {
                                // 不明リストへの書き込み: 以降の読みを止める。
                                readsBlocked = true;
                            }
                        }
                        continue;
                    }
                    static const bool hoistItems = [] {
                        const char* e =
                            std::getenv("XYO_MLIR_HOIST_ITEM");
                        // 既定 ON（sorth ミニで +11〜14% を確認）。
                        // スイート全体でも item 一致は 5 件のみで、
                        // fleet 低下の要因にはなり得ない。
                        // 無効化は =0（A/B 用）。
                        return !(e && e[0] == '0');
                    }();
                    bool isItem = (n == "xyo.list.item" ||
                                   n == "xyo.list.item.i" ||
                                   n == "xyo.list.item.num" ||
                                   n == "xyo.list.item.num.i" ||
                                   n == "xyo.list.itemnum" ||
                                   n == "xyo.list.length" ||
                                   n == "xyo.list.contains" ||
                                   n == "xyo.list.contents");
                    bool isRead = isItem && hoistItems;
                    // 読みの hoist 無効時は getvar 同様に読み飛ばす
                    // （bail すると純粋式の重複除去まで止まる）。
                    if (isItem && !hoistItems) continue;
                    bool pureCompute =
                        dialect::isPure(n) && !isRead;
                    if (!isRead && !pureCompute) {
                        if (std::getenv("XYO_MLIR_HOISTDBG"))
                            std::fprintf(stderr,
                                         "[mlir-hoist] block reads on %s\n",
                                         n.c_str());
                        // 読み以外は続行（純粋式は関数的なので安全）。
                        readsBlocked = true;
                        continue;
                    }
                    if (o->resultTypes.size() != 1) continue;
                    // length/contents は operands[0]=list のみ（index なし）。
                    size_t needOps = 0;
                    if (isRead)
                        needOps = (n == "xyo.list.length" ||
                                   n == "xyo.list.contents")
                                      ? 1u
                                      : 2u;
                    if (o->operands.size() < needOps) {
                        // 引数なし純粋 op（const 等）はそのまま。
                        continue;
                    }
                    // オペランドを解決する。
                    std::vector<Value> rargs;
                    bool unres = false;
                    size_t startIdx = 0;
                    std::string lid;
                    if (isRead) {
                        // ブロック後は読みを hoist しない（純粋式は継続）。
                        if (readsBlocked) continue;
                        lid = listIdOf(o->operands[0]);
                        if (std::getenv("XYO_MLIR_HOISTDBG"))
                            std::fprintf(stderr,
                                         "[mlir-hoist] read %s lid=%s stored=%d\n",
                                         n.c_str(), lid.c_str(),
                                         (int)(!lid.empty() && stored.count(lid)));
                        if (lid.empty() || stored.count(lid)) continue;
                        startIdx = 1;
                    }
                    for (size_t i = startIdx; i < o->operands.size();
                         ++i) {
                        bool un = false;
                        Value rv = resolve(o->operands[i], &un);
                        if (un) {
                            unres = true;
                            break;
                        }
                        // getvar 由来は追跡しない（保守的）。
                        if (rv.op && rv.op->name == "xyo.getvar" &&
                            rv.op->parent == blk) {
                            unres = true;
                            break;
                        }
                        rargs.push_back(rv);
                    }
                    if (unres) continue;
                    // キーを作る（読みは list id を含める。親スコープと
                    // 同じ形式にする）。
                    std::string key;
                    if (isRead) {
                        key = scopeKey(o, rargs);
                        key += "|L" + lid;
                    } else {
                        key = scopeKey(o, rargs);
                    }
                    if (std::getenv("XYO_MLIR_HOISTDBG") && isRead)
                        std::fprintf(stderr,
                                     "[mlir-hoist] lookup %s key=%.120s\n",
                                     n.c_str(), key.c_str());
                    auto f = scope.find(key);
                    if (f == scope.end()) continue;
                    // 一致: 後段で全 use を差し替える（今は記録だけ）。
                    if (std::getenv("XYO_MLIR_HOISTDBG"))
                        std::fprintf(stderr, "[mlir-hoist] match %s\n",
                                     o->name.c_str());
                    remap[o] = f->second;
                    dead.push_back(o);
                    ++hits;
                }
                // 全 use を親の値へ差し替える（全 remap は検証済み）。
                // 読みは readsBlocked で止めているので安全。
                // 入れ子 region（if/loop の本体）の operand も書き換える。
                // ここを見落とすと、入れ子の中でだけ使われている重複 op を
                // 「未使用」と誤判定して消し、lowering が未定義値参照で
                // 失敗する（実測: sched の 6018 op スクリプトで発生）。
                std::function<void(Op*, const std::function<void(Op*)>&)>
                    visitDeep = [&](Op* root,
                                    const std::function<void(Op*)>& fn) {
                        fn(root);
                        for (auto& rg : root->regions) {
                            if (!rg) continue;
                            for (auto& cb : rg->blocks)
                                for (Op* o : cb->ops) visitDeep(o, fn);
                        }
                    };
                for (auto& [o, pv] : remap) {
                    for (Op* top : blk->ops) {
                        visitDeep(top, [&](Op* u) {
                            for (auto& uu : u->operands)
                                if (uu.op == o) uu = pv;
                        });
                    }
                }
                // 使われなくなった重複を消す（入れ子 region 内の use も数える）。
                for (Op* o : dead) {
                    bool used = false;
                    for (Op* top : blk->ops) {
                        visitDeep(top, [&](Op* u) {
                            if (used || u == o) return;
                            for (auto& uu : u->operands)
                                if (uu.op == o) {
                                    used = true;
                                    return;
                                }
                        });
                        if (used) break;
                    }
                    if (!used) eraseFromBlock(*blk, o);
                }
            }
        }
    };
    walkBlocks(m, walk);
    if (stats) stats->cse += hits;
}

void dce(Module& m, PassStats* stats) {
    uint64_t removed = 0;
    runDce(m, &removed);
    if (stats) stats->dce += removed;
}

void promoteNumericLoopVars(Module& m, PassStats* stats) {
    // ループ op を見つけたら、その本体を書き換える。書き換えは同じ
    // block の op 列を変更するので、いったん対象を集めてから処理する。
    struct Target {
        Block* parent;
        size_t index;
        Op* loop;
    };
    std::vector<Target> targets;
    walkBlocks(m, [&](Block& b) {
        for (size_t i = 0; i < b.ops.size(); ++i)
            if (b.ops[i]->name == "xyo.repeat" ||
                b.ops[i]->name == "xyo.while")
                targets.push_back({&b, i, b.ops[i]});
    });
    // 昇格は 2 段階。
    //  1) 本体に入れ子ループを持つループを、外側から順に昇格する。
    //     入れ子はまだ昇格前（実メモリを読み書きする形）なので、
    //     「入る前に spill / 抜けたら reload」で安全に包める。
    //  2) 残りは従来どおり内側から（index のずれを避けるため後ろから）。
    // 1) を分けるのは、外側優先にすると他のループの昇格順まで変わって
    // しまい、無関係なベンチが動くため。
    auto tryPromote = [&](Target& t, bool nestedOnly) -> bool {
        Op* loop = t.loop;
        const bool whileLoop = loop->name == "xyo.while";
        if (loop->regions.size() != (whileLoop ? 2u : 1u)) return false;
        Region* r = loop->region(whileLoop ? 1 : 0);
        if (!r || r->blocks.size() != 1) return false;
        Block* body = r->entry();
        if (body->ops.empty() || body->ops.back()->name != "xyo.yield")
            return false;
        if (nestedOnly && !bodyHasPlainNestedLoop(*body)) return false;
        size_t index = 0;
        bool found = false;
        for (size_t k = 0; k < t.parent->ops.size(); ++k) {
            if (t.parent->ops[k] != loop) continue;
            index = k;
            found = true;
            break;
        }
        if (!found) return false;  // 外側の書き換えで消えた
        Promoter p{m, *t.parent, index, loop, *body, {}, {}, {}, stats};
        p.run();
        return true;
    };
    std::vector<bool> done(targets.size(), false);
    if (nestedLoopEnabled()) {
        for (size_t i = 0; i < targets.size(); ++i)
            done[i] = tryPromote(targets[i], /*nestedOnly=*/true);
    }
    for (size_t i = targets.size(); i-- > 0;) {
        if (done[i]) continue;
        tryPromote(targets[i], /*nestedOnly=*/false);
    }
}


// ============================================ 定数ループのコンパイル時評価 ====
//
// warp repeat の本体が純粋（整数・比較・選択のみ）で、反復回数と本体が
// 読む変数の値がコンパイル時に確定しているなら、ループをその場で解釈
// 実行して最終値を定数に置き換える。legacy codegen の「線形本体を閉形式に
// 畳む」最適化と同じ効果を、SSA 上の定数解釈として一般に得る。
// 意味論は厳密: 反復回数は実行時と同じ数え方（remaining >= 0.5 を 1 ずつ
// 減算）で求め、各 op は実行時ヘルパと同一関数で評価する。
// warp ループに限る（非 warp は毎反復 yield する意味論があるため）。

namespace {

/// 解釈できる op か（値計算のみ・副作用なし・region なし）。
bool interpOp(const Op& op) {
    static const std::unordered_set<std::string> kOk = {
        "xyo.const",   "xyo.const.f64", "xyo.getvar",  "xyo.setvar",
        "xyo.add",     "xyo.sub",       "xyo.mul",     "xyo.div",
        "xyo.mod",     "xyo.round",     "xyo.to_num",  "xyo.from_num",
        "xyo.lt",      "xyo.gt",        "xyo.eq",      "xyo.truthy",
        "xyo.and",     "xyo.or",        "xyo.not",     "xyo.select",
        "xyo.band",    "xyo.bor",       "xyo.bnot",
        "xyo.cmp.f",   "xyo.box.f",     "xyo.boolval", "xyo.num_ok",
        "xyo.loop_yield", "xyo.yield",  "xyo.warp_yield",
        "xyo.condition",  "xyo.for_each", "xyo.join", "xyo.if",
    };
    return kOk.count(op.name) != 0;
}

/// 本体の読み書きを集める。解釈不能なら false。
bool scanInterpBody(Block& b, std::unordered_set<std::string>* reads,
                    std::unordered_set<std::string>* writes, int depth = 0) {
    if (depth > 4) return false;
    for (Op* op : b.ops) {
        // 入れ子の定数ループ・if は再帰的に解釈できる。
        if (op->name == "xyo.if") {
            if (op->regions.size() != 2) return false;
            for (auto& r : op->regions) {
                if (!r || r->blocks.size() != 1) return false;
                if (!scanInterpBody(*r->entry(), reads, writes, depth + 1))
                    return false;
            }
            continue;
        }
        if (op->name == "xyo.repeat" || op->name == "xyo.while") {
            if (op->name == "xyo.repeat" && op->attrInt("warp") == 0)
                return false;
            if (op->regions.empty()) return false;
            for (auto& r : op->regions) {
                if (!r || r->blocks.size() != 1) return false;
                if (!scanInterpBody(*r->entry(), reads, writes, depth + 1))
                    return false;
            }
            continue;
        }
        if (!interpOp(*op)) return false;
        if (!op->regions.empty()) return false;
        if (op->name == "xyo.getvar")
            reads->insert(std::string(op->attrStr("id")));
        else if (op->name == "xyo.setvar") {
            if (op->operands.empty()) return false;
            writes->insert(std::string(op->attrStr("id")));
        }
    }
    return true;
}

/// 定数オペランドを実行時と同じ数値化で評価する（文字列定数も含む。
/// SStr は pinner が保持するのでコンパイル時に読んでも安全）。
bool interpFiniteConst(const Value& v, double* out) {
    uint64_t bits = 0;
    if (!v.op || !constBits(v.op, &bits)) return false;
    const double d = xyo_to_num(bits);
    if (!std::isfinite(d)) return false;
    *out = d;
    return true;
}

/// 実行時と同じ反復回数。非有限・巨大なら -1。
int64_t repeatTripCount(double c) {
    if (!std::isfinite(c) || c > 1e9) return -1;
    if (c < 0.5) return 0;
    // remaining を 1 ずつ減らし >= 0.5 の間実行 = floor(c + 0.5)
    return static_cast<int64_t>(std::floor(c + 0.5));
}

struct ConstInterp {
    bool ok = true;
    /// 畳み込み中に連結で作った総文字数（コンパイル時間の暴走防止）。
    uint64_t concatBytes = 0;
    std::unordered_map<std::string, uint64_t> vars;

    bool has(const std::string& id) const { return vars.count(id) != 0; }
    uint64_t get(const std::string& id) {
        auto it = vars.find(id);
        if (it == vars.end()) {
            ok = false;
            return 0;
        }
        return it->second;
    }
    void set(const std::string& id, uint64_t bits) { vars[id] = bits; }

    uint64_t evalOp(Op& op) {
        const std::string& n = op.name;
        auto arg = [&](size_t i) {
            if (i >= op.operands.size() || !op.operands[i].op) {
                ok = false;
                return uint64_t{0};
            }
            return evalOp(*op.operands[i].op);
        };
        if (n == "xyo.const") {
            uint64_t bits = 0;
            if (!constBits(&op, &bits)) {
                ok = false;
                return 0;
            }
            return bits;
        }
        if (n == "xyo.const.f64") {
            const Attr* a = op.attr("d");
            if (!a || !a->isDouble()) {
                ok = false;
                return 0;
            }
            return xyo_num(a->d);
        }
        if (n == "xyo.getvar") return get(std::string(op.attrStr("id")));
        if (n == "xyo.add") return xyo_add(arg(0), arg(1));
        if (n == "xyo.sub") return xyo_sub(arg(0), arg(1));
        if (n == "xyo.mul") return xyo_mul(arg(0), arg(1));
        if (n == "xyo.div") return xyo_div(arg(0), arg(1));
        if (n == "xyo.mod") return xyo_mod(arg(0), arg(1));
        if (n == "xyo.round") return xyo_num(xyo_round(xyo_to_num(arg(0))));
        if (n == "xyo.to_num") return xyo_num(xyo_to_num(arg(0)));
        if (n == "xyo.from_num") return xyo_num(xyo_to_num(arg(0)));
        if (n == "xyo.lt") return xyo_lt(arg(0), arg(1)) ? xyo::val::kTrue
                                                         : xyo::val::kFalse;
        if (n == "xyo.gt") return xyo_gt(arg(0), arg(1)) ? xyo::val::kTrue
                                                         : xyo::val::kFalse;
        if (n == "xyo.eq") return xyo_eq(arg(0), arg(1)) ? xyo::val::kTrue
                                                         : xyo::val::kFalse;
        if (n == "xyo.truthy")
            return xyo_truthy(arg(0)) ? xyo::val::kTrue : xyo::val::kFalse;
        if (n == "xyo.and" || n == "xyo.or" || n == "xyo.not" ||
            n == "xyo.band" || n == "xyo.bor" || n == "xyo.bnot") {
            const bool a = xyo_truthy(arg(0)) != 0;
            if (n == "xyo.not" || n == "xyo.bnot")
                return a ? xyo::val::kFalse : xyo::val::kTrue;
            const bool c = xyo_truthy(arg(1)) != 0;
            const bool r = (n == "xyo.and" || n == "xyo.band") ? (a && c)
                                                               : (a || c);
            return r ? xyo::val::kTrue : xyo::val::kFalse;
        }
        if (n == "xyo.select") {
            const bool c = arg(0) == xyo::val::kTrue;
            return c ? arg(1) : arg(2);
        }
        if (n == "xyo.cmp.f") {
            const int64_t pred = op.attrInt("pred");
            const double a = xyo_to_num(arg(0));
            const double c = xyo_to_num(arg(1));
            bool r = false;
            switch (pred) {
            case 0: r = a < c; break;
            case 1: r = a > c; break;
            case 2: r = a <= c; break;
            case 3: r = a >= c; break;
            case 4: r = a == c; break;
            case 5: r = a != c; break;
            default: ok = false; return 0;
            }
            return r ? xyo::val::kTrue : xyo::val::kFalse;
        }
        if (n == "xyo.box.f") {
            const double d = xyo_to_num(arg(0));
            return xyo_num(d);
        }
        if (n == "xyo.boolval") return arg(0);
        if (n == "xyo.num_ok") {
            const uint64_t bits = arg(0);
            const double d = xyo_to_num(bits);
            const bool r = bits < static_cast<uint64_t>(val::kNan) && d == d;
            return r ? xyo::val::kTrue : xyo::val::kFalse;
        }
        if (n == "xyo.join") {
            // 文字列連結。結果が長すぎる場合は畳まない（コンパイル時間と
            // メモリの保護）。実行時と同じヘルパで評価する。
            const uint64_t a = arg(0);
            const uint64_t b = arg(1);
            if (!ok) return 0;
            const uint64_t r = xyo_join(a, b);
            const xyo::Value rv{r};
            if (!rv.isString()) {
                if (std::getenv("XYO_MLIR_FOLDDBG"))
                    std::fprintf(stderr,
                                 "[fold] join not string a=%llx b=%llx r=%llx\n",
                                 (unsigned long long)a,
                                 (unsigned long long)b,
                                 (unsigned long long)r);
                ok = false;
                return 0;
            }
            const uint32_t len = rv.asStrPtr() ? rv.asStrPtr()->len : 0;
            concatBytes += len;
            if (len > 1u << 20 || concatBytes > 2ull * 1024u * 1024u * 1024u) {
                if (std::getenv("XYO_MLIR_FOLDDBG"))
                    std::fprintf(stderr,
                                 "[fold] concat budget: len=%u total=%llu\n",
                                 len, (unsigned long long)concatBytes);
                ok = false;
                return 0;
            }
            return r;
        }
        if (n == "xyo.loop_yield" || n == "xyo.warp_yield" ||
            n == "xyo.yield" || n == "xyo.condition")
            return 0;  // 値に影響しない
        if (std::getenv("XYO_MLIR_FOLDDBG"))
            std::fprintf(stderr, "[fold] unknown op in interp: %s\n",
                         n.c_str());
        ok = false;
        return 0;
    }

    /// ブロック（と入れ子の定数ループ）を解釈実行する。budget は評価回数の
    /// 上限で、超えたら ok=false（コンパイル時間の暴走を防ぐ）。
    void runBlock(Block& b, uint64_t* budget) {
        for (Op* op : b.ops) {
            if (!ok) return;
            if (*budget == 0) {
                ok = false;
                return;
            }
            --*budget;
            const std::string& n = op->name;
            if (n == "xyo.if") {
                if (op->regions.size() != 2 || op->operands.empty() ||
                    !op->operands[0].op) {
                    ok = false;
                    return;
                }
                const bool go =
                    evalOp(*op->operands[0].op) == xyo::val::kTrue;
                if (!ok) return;
                runBlock(*op->region(go ? 0 : 1)->entry(), budget);
                continue;
            }
            if (n == "xyo.for_each") {
                // 1..VALUE を数値ループとして解釈する（変数は都度 set）。
                Block* fbody =
                    op->regions.empty() || !op->region(0)->entry()
                        ? nullptr
                        : op->region(0)->entry();
                double count = 0;
                if (!fbody || op->operands.empty() ||
                    !interpFiniteConst(op->operands[0], &count)) {
                    ok = false;
                    return;
                }
                const std::string id(op->attrStr("id"));
                const double lim = std::ceil(count);
                for (double i = 1; i <= lim; ++i) {
                    set(id, xyo_num(i));
                    runBlock(*fbody, budget);
                    if (!ok || *budget == 0) {
                        ok = false;
                        return;
                    }
                }
                continue;
            }
            if (n == "xyo.repeat") {
                Block* body =
                    op->regions.empty() || !op->region(0)->entry()
                        ? nullptr
                        : op->region(0)->entry();
                double c = 0;
                if (!body || op->operands.empty() ||
                    !interpFiniteConst(op->operands[0], &c)) {
                    ok = false;
                    return;
                }
                const int64_t trip = repeatTripCount(c);
                if (trip < 0) {
                    ok = false;
                    return;
                }
                for (int64_t t = 0; t < trip && ok; ++t) {
                    runBlock(*body, budget);
                    if (*budget == 0) {
                        ok = false;
                        return;
                    }
                }
                continue;
            }
            if (n == "xyo.while") {
                Block* cond =
                    op->regions.size() > 0 && op->region(0)
                        ? op->region(0)->entry()
                        : nullptr;
                Block* body =
                    op->regions.size() > 1 && op->region(1)
                        ? op->region(1)->entry()
                        : nullptr;
                if (!cond || !body) {
                    ok = false;
                    return;
                }
                for (;;) {
                    runBlock(*cond, budget);
                    if (!ok) return;
                    Op* term = cond->lastOp();
                    if (!term || term->name != "xyo.condition" ||
                        term->operands.empty() || !term->operands[0].op) {
                        ok = false;
                        return;
                    }
                    const bool go = evalOp(*term->operands[0].op) ==
                                    xyo::val::kTrue;
                    if (!ok) return;
                    if (!go) break;
                    runBlock(*body, budget);
                    if (!ok || *budget == 0) {
                        ok = false;
                        return;
                    }
                }
                continue;
            }
            if (n == "xyo.setvar") {
                if (op->operands.empty() || !op->operands[0].op) {
                    if (std::getenv("XYO_MLIR_FOLDDBG"))
                        std::fprintf(stderr, "[fold] setvar no operand\n");
                    ok = false;
                    return;
                }
                const uint64_t v = evalOp(*op->operands[0].op);
                if (!ok) return;
                set(std::string(op->attrStr("id")), v);
                continue;
            }
            if (n == "xyo.yield" || n == "xyo.loop_yield" ||
                n == "xyo.warp_yield" || n == "xyo.condition")
                continue;
            (void)evalOp(*op);
        }
    }
};

}  // namespace

uint64_t foldConstantRepeatLoops(Module& m) {
    uint64_t folded = 0;
    walkBlocks(m, [&](Block& top) {
        std::unordered_map<std::string, uint64_t> known;
        for (size_t i = 0; i < top.ops.size();) {
            Op* op = top.ops[i];
            if (op->name == "xyo.setvar" && !op->operands.empty()) {
                const std::string id(op->attrStr("id"));
                uint64_t bits = 0;
                // 文字列定数も実行時と同じ数値化で扱えるので記録する
                // （fractch 由来の数値は文字列で来る）。SStr は pinner が
                // 保持しており、コンパイル時参照は安全。
                if (op->operands[0].op &&
                    op->operands[0].op->name == "xyo.const" &&
                    constBits(op->operands[0].op, &bits))
                    known[id] = bits;
                else
                    known.erase(id);
                ++i;
                continue;
            }
            const bool harmless = dialect::isPure(op->name) ||
                                  dialect::isReadOnly(op->name) ||
                                  op->name == "xyo.yield" ||
                                  op->name == "xyo.loop_yield" ||
                                  op->name == "xyo.warp_yield" ||
                                  op->name == "xyo.condition" ||
                                  op->name == "func.return";
            const bool isLoop = op->name == "xyo.repeat" ||
                                op->name == "xyo.while" ||
                                op->name == "xyo.for_each";
            if (!isLoop ||
                (op->name == "xyo.repeat" && op->attrInt("warp") == 0) ||
                op->regions.empty()) {
                if (!harmless) known.clear();
                ++i;
                continue;
            }
            Block* body =
                op->name == "xyo.repeat" || op->name == "xyo.for_each"
                    ? (op->region(0) ? op->region(0)->entry() : nullptr)
                    : (op->regions.size() > 1 && op->region(1)
                           ? op->region(1)->entry()
                           : nullptr);
            if (!body) {
                known.clear();
                ++i;
                continue;
            }
            std::unordered_set<std::string> reads, writes;
            const bool scanOk = scanInterpBody(*body, &reads, &writes);
            // for_each の添字変数は各反復で値が決まるので既知扱いにする。
            if (op->name == "xyo.for_each")
                reads.erase(std::string(op->attrStr("id")));
            if (std::getenv("XYO_MLIR_FOLDDBG"))
                std::fprintf(stderr,
                             "[fold] loop %s scan=%d reads=%zu writes=%zu known=%zu\n",
                             op->name.c_str(), (int)scanOk, reads.size(),
                             writes.size(), known.size());
            if (!scanOk) {
                known.clear();
                ++i;
                continue;
            }
            if (op->name == "xyo.while") {
                // 条件領域も解釈できることが前提。
                Block* cond = op->region(0) ? op->region(0)->entry()
                                            : nullptr;
                if (!cond || !scanInterpBody(*cond, &reads, &writes)) {
                    known.clear();
                    ++i;
                    continue;
                }
            }
            bool allKnown = true;
            for (const std::string& id : reads)
                if (!known.count(id)) allKnown = false;
            if (!allKnown) {
                if (std::getenv("XYO_MLIR_FOLDDBG")) {
                    std::string miss;
                    for (const std::string& id : reads)
                        if (!known.count(id)) miss += id + " ";
                    std::fprintf(stderr, "[fold]   unknown: %s\n",
                                 miss.c_str());
                }
                known.clear();
                ++i;
                continue;
            }
            if (std::getenv("XYO_MLIR_FOLDDBG")) {
                std::string bodyOps;
                for (Op* o : body->ops) bodyOps += o->name + " ";
                std::fprintf(stderr, "[fold]   body: %s\n", bodyOps.c_str());
            }
            ConstInterp in;
            in.vars = known;
            uint64_t budget = 4000000;
            if (op->name == "xyo.for_each") {
                double count = 0;
                if (!interpFiniteConst(op->operands[0], &count)) {
                    known.clear();
                    ++i;
                    continue;
                }
                const double lim = std::ceil(count);
                if (lim < 0 || lim > 200000 ||
                    static_cast<uint64_t>(lim) * (body->ops.size() + 1) >
                        2000000ULL) {
                    known.clear();
                    ++i;
                    continue;
                }
                const std::string vid(op->attrStr("id"));
                for (double t = 1; t <= lim && in.ok; t += 1.0) {
                    in.set(vid, xyo_num(t));
                    in.runBlock(*body, &budget);
                }
            } else if (op->name == "xyo.repeat") {
                double count = 0;
                if (!interpFiniteConst(op->operands[0], &count)) {
                    known.clear();
                    ++i;
                    continue;
                }
                const int64_t trip = repeatTripCount(count);
                if (trip < 0 || trip > 200000 ||
                    static_cast<uint64_t>(trip) * (body->ops.size() + 1) >
                        2000000ULL) {
                    known.clear();
                    ++i;
                    continue;
                }
                for (int64_t t = 0; t < trip && in.ok; ++t)
                    in.runBlock(*body, &budget);
            } else {
                in.runBlock(*op->region(1)->entry(), &budget);
                // while は条件評価を含めて解釈する（runBlock 内で処理）。
                Block* cond = op->region(0)->entry();
                Block* bodyB = op->region(1)->entry();
                // 上の runBlock(body) は 1 回だけなので、正しくは
                // while 全体を評価する必要がある。やり直す。
                in.vars = known;
                in.ok = true;
                for (;;) {
                    in.runBlock(*cond, &budget);
                    if (!in.ok) break;
                    Op* term = cond->lastOp();
                    if (!term || term->name != "xyo.condition" ||
                        term->operands.empty() || !term->operands[0].op) {
                        in.ok = false;
                        break;
                    }
                    const bool go =
                        in.evalOp(*term->operands[0].op) == xyo::val::kTrue;
                    if (!in.ok) break;
                    if (!go) break;
                    in.runBlock(*bodyB, &budget);
                }
            }
            if (std::getenv("XYO_MLIR_FOLDDBG")) {
                std::string vals;
                for (const std::string& id : writes) {
                    auto it = in.vars.find(id);
                    vals += id + "=" +
                            (it == in.vars.end()
                                 ? std::string("?")
                                 : std::to_string(xyo_to_num(it->second))) +
                            " ";
                }
                std::fprintf(stderr, "[fold]   interp ok=%d budget=%llu %s\n",
                             (int)in.ok, (unsigned long long)budget,
                             vals.c_str());
            }
            if (!in.ok) {
                known.clear();
                ++i;
                continue;
            }
            bool emitOk = true;
            for (const std::string& id : writes) {
                auto it = in.vars.find(id);
                if (it == in.vars.end()) continue;
                const xyo::Value v{it->second};
                if (v.isNumber() || v.isBool()) continue;
                // 文字列は pass が作った実体を frontend で pin して使う。
                if (v.isString() && v.bits != xyo::val::kEmptyStr &&
                    v.asStrPtr())
                    continue;
                emitOk = false;
            }
            if (!emitOk) {
                known.clear();
                ++i;
                continue;
            }
            std::vector<Op*> repl;
            for (const std::string& id : writes) {
                auto it = in.vars.find(id);
                if (it == in.vars.end()) continue;
                const xyo::Value cv{it->second};
                if (cv.isString() && cv.bits != xyo::val::kEmptyStr &&
                    cv.asStrPtr())
                    m.pinnedStrings.push_back(cv.asStrPtr());
                Op* c = m.create("xyo.const", {}, {Type::value()},
                                 {{"bits", Attr::integer(
                                       static_cast<int64_t>(it->second))}});
                c->parent = &top;
                Op* st = m.create("xyo.setvar", {c->result(0)}, {},
                                  {{"id", Attr::str(id)},
                                   {"name", Attr::str(id)}});
                st->parent = &top;
                repl.push_back(c);
                repl.push_back(st);
                known[id] = it->second;
            }
            top.ops.erase(top.ops.begin() + static_cast<ptrdiff_t>(i));
            for (size_t k = 0; k < repl.size(); ++k)
                top.ops.insert(top.ops.begin() +
                                   static_cast<ptrdiff_t>(i + k), repl[k]);
            i += repl.size();
            ++folded;
        }
    });
    return folded;
}

/// `to_num(list.item.i(ptr, idx))` が数値リストの読みなら、数値として直接
/// 読む `xyo.list.item.num.i` に融合する（タグ検査を消し、読みを 1 回にする）。
uint64_t fuseNumericListToNum(Module& m) {
    static const bool off = [] {
        const char* e = std::getenv("XYO_MLIR_NOFUSENUM");
        return e && e[0] == '1';
    }();
    if (off) return 0;
    std::unordered_map<Op*, int> uses;
    std::function<void(Block&)> countUses = [&](Block& b) {
        for (Op* op : b.ops) {
            for (const Value& v : op->operands)
                if (v.op) ++uses[v.op];
            for (auto& r : op->regions)
                if (r)
                    for (auto& cb : r->blocks) countUses(*cb);
        }
    };
    for (auto& f : m.functions())
        if (f.body) countUses(*f.body);
    uint64_t fused = 0;
    std::vector<Op*> toErase;
    std::function<void(Block&)> scan = [&](Block& b) {
        for (Op* op : b.ops) {
            for (auto& r : op->regions)
                if (r)
                    for (auto& cb : r->blocks) scan(*cb);
            if (op->name != "xyo.to_num" || op->operands.size() != 1) continue;
            Op* src = op->operands[0].op;
            const bool intIdx = src && src->name == "xyo.list.item.i";
            const bool valIdx = src && src->name == "xyo.list.item";
            if (!src || (!intIdx && !valIdx) || src->operands.size() != 2 ||
                uses[src] != 1)
                continue;
            Op* lp = src->operands[0].op;
            if (!lp || lp->name != "xyo.list.ptr" || lp->attrInt("numeric") == 0)
                continue;
            op->name = intIdx ? "xyo.list.item.num.i" : "xyo.list.item.num";
            op->operands = src->operands;
            op->resultTypes = {Type::f64()};
            toErase.push_back(src);
            ++fused;
        }
    };
    for (auto& f : m.functions())
        if (f.body) scan(*f.body);
    if (!toErase.empty()) {
        std::unordered_set<Op*> dead(toErase.begin(), toErase.end());
        std::function<void(Block&)> remove = [&](Block& b) {
            for (size_t i = b.ops.size(); i-- > 0;) {
                if (dead.count(b.ops[i])) {
                    b.ops[i]->parent = nullptr;
                    b.ops.erase(b.ops.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                for (auto& r : b.ops[i]->regions)
                    if (r)
                        for (auto& cb : r->blocks) remove(*cb);
            }
        };
        for (auto& f : m.functions())
            if (f.body) remove(*f.body);
    }
    return fused;
}

/// Value 算術・比較のオペランドを数値（f64）へ昇格する。
/// 昇格ループの外（非昇格ループや直線コード）でも toNumber のタグ検査を
/// まとめて消す。数値と証明できた値だけを対象にし、失敗したら何もしない。
uint64_t promoteNumericArith(Module& m) {
    std::unordered_map<Op*, int> uses;
    std::function<void(Block&)> countUses = [&](Block& b) {
        for (Op* op : b.ops) {
            for (const Value& v : op->operands)
                if (v.op) ++uses[v.op];
            for (auto& r : op->regions)
                if (r)
                    for (auto& cb : r->blocks) countUses(*cb);
        }
    };
    for (auto& f : m.functions())
        if (f.body) countUses(*f.body);
    auto numericListPtr = [&](const Value& v) {
        return v.op && v.op->name == "xyo.list.ptr" &&
               v.op->attrInt("numeric") != 0;
    };
    uint64_t rewritten = 0;
    std::function<void(Block&)> run = [&](Block& blk) {
        std::unordered_map<Op*, Value> f64Of;
        std::vector<Op*> out;
        for (Op* op : blk.ops) {
            for (auto& r : op->regions)
                if (r)
                    for (auto& cb : r->blocks) run(*cb);
            // オペランドを f64 へ。
            auto toF64 = [&](const Value& v, Value* outv) -> bool {
                if (!v.op) return false;
                if (v.type == Type::f64()) {
                    *outv = v;
                    return true;
                }
                auto mit = f64Of.find(v.op);
                if (mit != f64Of.end()) {
                    *outv = mit->second;
                    return true;
                }
                Op* o = v.op;
                if (o->name == "xyo.box.f" && !o->operands.empty()) {
                    *outv = o->operands[0];
                    return true;
                }
                if (o->name == "xyo.to_num") {
                    *outv = o->result(0);
                    return true;
                }
                double d = 0;
                if (o->name == "xyo.const") {
                    const Attr* a = o->attr("bits");
                    if (!a || !a->isInt()) return false;
                    const xyo::Value cv{static_cast<uint64_t>(a->i)};
                    if (!cv.isNumber()) return false;
                    d = cv.asNumberUnchecked();
                } else if (o->name == "xyo.getvar" && m.numericVars.count(
                               std::string(o->attrStr("id")))) {
                    Op* t = m.create("xyo.to_num", {v}, {Type::f64()});
                    t->parent = &blk;
                    out.push_back(t);
                    *outv = t->result(0);
                    return true;
                } else if ((o->name == "xyo.list.item" ||
                            o->name == "xyo.list.item.i") &&
                           o->operands.size() == 2 &&
                           numericListPtr(o->operands[0]) && uses[o] == 1) {
                    o->name = o->name == "xyo.list.item.i"
                                  ? "xyo.list.item.num.i"
                                  : "xyo.list.item.num";
                    o->resultTypes = {Type::f64()};
                    *outv = o->result(0);
                    return true;
                } else {
                    return false;
                }
                Op* c = m.create("xyo.const.f64", {}, {Type::f64()},
                                 {{"d", Attr::real(d)}});
                c->parent = &blk;
                out.push_back(c);
                *outv = c->result(0);
                return true;
            };
            if (op->name == "xyo.add" || op->name == "xyo.sub" ||
                op->name == "xyo.mul" || op->name == "xyo.div" ||
                op->name == "xyo.mod") {
                if (op->operands.size() == 2) {
                    Value a, c;
                    const size_t mark = out.size();
                    if (toF64(op->operands[0], &a) &&
                        toF64(op->operands[1], &c)) {
                        Op* f = m.create(op->name + ".f", {a, c}, {Type::f64()});
                        f->parent = &blk;
                        out.push_back(f);
                        f64Of[op] = f->result(0);
                        op->name = "xyo.box.f";
                        op->operands = {f->result(0)};
                        op->resultTypes = {Type::value()};
                        op->attrs.clear();
                        ++rewritten;
                        out.push_back(op);
                        continue;
                    }
                    out.resize(mark);
                }
            }
            if (op->name == "xyo.cmp.guarded" &&
                op->operands.size() >= 4) {
                Value a, c;
                const size_t mark = out.size();
                if (toF64(op->operands[0], &a) &&
                    toF64(op->operands[1], &c)) {
                    op->name = "xyo.cmp.f";
                    op->operands = {a, c};
                    op->resultTypes = {Type::boolean()};
                    out.push_back(op);
                    ++rewritten;
                    continue;
                }
                out.resize(mark);
            }
            out.push_back(op);
        }
        blk.ops = std::move(out);
    };
    for (auto& f : m.functions())
        if (f.body) run(*f.body);
    return rewritten;
}

void runStandardPipeline(Module& m, PassStats* stats) {
    canonicalize(m, stats);
    cse(m, stats);
    dce(m, stats);
    // 定数 warp ループのコンパイル時評価（閉形式化）。昇格より先に。
    static const bool foldLoops = [] {
        const char* e = std::getenv("XYO_MLIR_NOCONSTLOOP");
        return !(e && e[0] == '1');
    }();
    if (foldLoops) {
        // 内側から順に畳めるよう、変化が無くなるまで繰り返す。
        uint64_t folded = 0;
        for (int round = 0; round < 4; ++round) {
            const uint64_t f = foldConstantRepeatLoops(m);
            folded += f;
            if (!f) break;
            canonicalize(m, stats);
            cse(m, stats);
            dce(m, stats);
        }
        if (stats) stats->folded += folded;
    }
    // デバッグ用の非常口（A/B 用）: XYO_MLIR_NOPROMOTE=1 で昇格を止める。
    const char* noPromote = std::getenv("XYO_MLIR_NOPROMOTE");
    if (!noPromote || noPromote[0] != '1')
        promoteNumericLoopVars(m, stats);
    fuseNumericListToNum(m);
    promoteNumericArith(m);
    // if 領域内の重複純粋式を親へ統合（昇格で生じた重複読み・再計算の除去）。
    // XYO_MLIR_NOHOIST=1 で無効化（A/B 用）。
    static const bool noHoist = [] {
        const char* e = std::getenv("XYO_MLIR_NOHOIST");
        return e && e[0] == '1';
    }();
    if (!noHoist) hoistIfDuplicates(m, stats);
    canonicalize(m, stats);
    // hoist 後に現れる直鎖の重複を除去（box 版の複製等）。
    // XYO_MLIR_NOPOSTCSE=1 で無効化（A/B 用）。
    static const bool noPostCse = [] {
        const char* e = std::getenv("XYO_MLIR_NOPOSTCSE");
        return e && e[0] == '1';
    }();
    if (!noPostCse) cse(m, stats);
    dce(m, stats);
}

} // namespace xyo::mlir

#endif // XYO_JIT && !__EMSCRIPTEN__
