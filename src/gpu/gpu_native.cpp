#include "gpu_native.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/cast.hpp"
#include "../core/color.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::gpu {

namespace {

constexpr const char* kProcRay = "r.render";
constexpr const char* kProcPath = "pt.pass";
constexpr const char* kProcTexSetup = "tex setup";

std::u16string fieldName(const BlockField& f) {
    if (f.value.isString()) {
        SStr* s = f.value.asStrPtr();
        if (s) return std::u16string(s->view());
        return {};
    }
    SStr* s = cast::toString(f.value);
    return s ? std::u16string(s->view()) : std::u16string();
}

std::string fmtF(double d) {
    if (std::isnan(d)) return "uintBitsToFloat(0x7FC00000u)";
    if (d == std::numeric_limits<double>::infinity())
        return "uintBitsToFloat(0x7F800000u)";
    if (d == -std::numeric_limits<double>::infinity())
        return "uintBitsToFloat(0xFF800000u)";
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", d);
    std::string s = buf;
    if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
    return s;
}

struct Ctx {
    bool skipDraw = false;
    bool rayPrefix = false;
};

struct Gen {
    Blocks* blocks = nullptr;
    Target* owner = nullptr;
    Target* stage = nullptr;
    bool path = false;
    bool sharedStack = false;
    bool inlineLocals = false;
    std::string* why = nullptr;
    bool failed = false;
    uint32_t depth = 0;

    NativeProgram prog;
    std::unordered_map<std::string, int32_t> varSlots;
    std::unordered_map<std::string, int32_t> listSlots;
    std::unordered_map<std::string, int32_t> accumCh;
    std::string stackId;
    std::unordered_set<std::string> written;

    std::unordered_map<std::string, int32_t> procFn;
    std::unordered_map<std::string, std::string> procDef;
    std::unordered_map<std::string, std::vector<std::u16string>> procNames;
    std::unordered_map<std::string, std::vector<std::string>> procIds;
    std::unordered_map<std::string, std::vector<Value>> procDefaults;
    std::unordered_set<std::string> procEmitted;
    std::unordered_set<std::string> emitting;
    std::string protos, funcs;
    int tempN = 0;
    std::string* cur = nullptr;
    std::string skipBlockId;

    const Block* rowStepBlock = nullptr;
    const Block* colStepBlock = nullptr;
    size_t outerIdx = 0, innerIdx = 0;
    std::string rowVarId, colVarId, piVarId, resVarId, pnVarId;
    std::string thisColorId, lastColorId;
    const BlockInput* rowStepIn = nullptr;
    const BlockInput* colStepIn = nullptr;

    static GlobalVar gv(const IdName& r) { return {r.id, r.name}; }
    void fail(const char* m) {
        if (why && why->empty()) *why = m;
        failed = true;
    }

    const Block* blk(const std::string& id) {
        const Block* b = blocks->getBlock(id);
        if (!b) fail("missing block");
        return b;
    }
    const Mutation* mut(const Block* b) { return blocks->getMutation(b); }
    std::string proccodeOf(const Block* b) {
        const Mutation* m = mut(b);
        return m ? m->attr("proccode") : std::string();
    }
    IdName refOf(const Block* b, const char* field) {
        auto it = b->fields.find(field);
        if (it == b->fields.end()) return {};
        return {it->second.id, fieldName(it->second)};
    }
    const BlockInput* inp(const Block* b, const char* name) {
        return b->inputs.find(name);
    }

    bool varIsList(const std::string& id) const {
        auto f = [&](Target* t) -> const Variable* {
            if (!t) return nullptr;
            auto it = t->variables.find(id);
            return it == t->variables.end() ? nullptr : &it->second;
        };
        if (const Variable* v = f(owner))
            return v->type == Variable::kListType;
        if (const Variable* v = f(stage))
            return v->type == Variable::kListType;
        return false;
    }

    int32_t varSlot(const IdName& ref) {
        auto it = varSlots.find(ref.id);
        if (it != varSlots.end()) return it->second;
        int32_t s = int32_t(prog.vars.size());
        prog.vars.push_back(gv(ref));
        varSlots[ref.id] = s;
        return s;
    }
    int32_t listSlot(const IdName& ref) {
        auto it = listSlots.find(ref.id);
        if (it != listSlots.end()) return it->second;
        int32_t s = int32_t(prog.lists.size());
        prog.lists.push_back(gv(ref));
        listSlots[ref.id] = s;
        return s;
    }
    int listKind(const IdName& ref, int& ch) {
        if (ref.id == stackId) return 1;
        auto it = accumCh.find(ref.id);
        if (it != accumCh.end()) { ch = it->second; return 2; }
        return 0;
    }

    size_t emitted = 0;
    void line(const std::string& s) {
        *cur += s;
        *cur += '\n';
        emitted += s.size() + 1;
        if (emitted > kNativeSourceCap) fail("source size cap");
    }
    std::string temp() { return "t" + std::to_string(tempN++); }
    std::string hoist(std::string e) {
        std::string t = temp();
        line("float " + t + " = " + e + ";");
        return t;
    }

    std::vector<const Block*> chain(const std::string& start) {
        std::vector<const Block*> out;
        std::unordered_set<std::string> seen;
        std::string id = start;
        while (!id.empty() && !failed) {
            if (!seen.insert(id).second) { fail("cycle in block chain"); break; }
            const Block* b = blocks->getBlock(id);
            if (!b) { fail("missing block"); break; }
            out.push_back(b);
            id = b->next;
        }
        return out;
    }
    bool subtreeHas(const std::string& id, bool (*pred)(const std::string&),
                    uint32_t guard = 0) {
        if (guard > kNativeDepthCap) {
            fail("subtree too deep");
            return false;
        }
        const Block* b = blocks->getBlock(id);
        if (!b) return false;
        if (pred(b->opcode)) return true;
        for (auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0) {
                for (const Block* s : chain(in.block))
                    if (subtreeHas(s->id, pred, guard + 1)) return true;
            } else {
                if (!in.block.empty() &&
                    subtreeHas(in.block, pred, guard + 1))
                    return true;
                if (!in.shadow.empty() &&
                    subtreeHas(in.shadow, pred, guard + 1))
                    return true;
            }
        }
        return false;
    }
    bool chainHas(const std::string& start,
                  bool (*pred)(const std::string&)) {
        for (const Block* b : chain(start))
            if (pred(b->opcode) || subtreeHas(b->id, pred)) return true;
        return false;
    }
    static bool isPenOrMotion(const std::string& op) {
        return op.rfind("pen_", 0) == 0 || op.rfind("motion_", 0) == 0;
    }
    bool writesVar(const Block* b, std::string* idOut = nullptr) const {
        if (b->opcode == "data_setvariableto" ||
            b->opcode == "data_changevariableby") {
            auto it = b->fields.find("VARIABLE");
            if (it != b->fields.end() && idOut) *idOut = it->second.id;
            return true;
        }
        return false;
    }

    bool litValue(const Block* b, Value& out) {
        if (b->opcode == "text") {
            auto it = b->fields.find("TEXT");
            if (it != b->fields.end()) { out = it->second.value; return true; }
            out = Value::emptyStr();
            return true;
        }
        if (b->opcode.rfind("math_", 0) == 0) {
            auto it = b->fields.find("NUM");
            if (it != b->fields.end()) { out = it->second.value; return true; }
            out = Value::num(0);
            return true;
        }
        if (b->opcode == "colour_picker") {
            auto it = b->fields.find("COLOUR");
            if (it != b->fields.end() && it->second.value.isString()) {
                SStr* s = it->second.value.asStrPtr();
                std::string u8 =
                    s ? SStr::toUtf8(s->view()) : std::string();
                if (auto rgb = color::hexToRgb(u8)) {
                    out = Value::num(double(color::rgbToDecimal(*rgb)));
                    return true;
                }
            }
            return false;
        }
        return false;
    }
    std::string litNum(const Block* b) {
        Value v;
        if (!litValue(b, v)) { fail("unsupported literal"); return "0.0"; }
        double d = cast::toNumberOrNaN(v);
        if (std::isnan(d)) { fail("nonnumeric literal in numeric graph"); return "0.0"; }
        return fmtF(d);
    }

    std::string in(const Block* b, const char* name) {
        const BlockInput* i = inp(b, name);
        if (!i) return "0.0";
        if (!i->block.empty()) return expr(i->block);
        if (!i->shadow.empty()) return expr(i->shadow);
        return "0.0";
    }
    std::string inOr(const BlockInput* i, const Value& def) {
        if (i) {
            if (!i->block.empty()) return expr(i->block);
            if (!i->shadow.empty()) return expr(i->shadow);
        }
        double d = cast::toNumberOrNaN(def);
        return fmtF(std::isnan(d) ? 0.0 : d);
    }

    std::string expr(const std::string& id) {
        const Block* b = blk(id);
        if (!b || failed) return "0.0";
        if (++depth > kNativeDepthCap) { fail("expression depth"); return "0.0"; }
        const std::string& op = b->opcode;
        std::string r;
        if (op == "data_variable") {
            IdName ref = refOf(b, "VARIABLE");
            if (ref.id.empty()) { fail("variable without field"); r = "0.0"; }
            else if (varIsList(ref.id)) { fail("list in scalar context"); r = "0.0"; }
            else r = "v" + std::to_string(varSlot(ref));
        } else if (op == "argument_reporter_string_number" ||
                   op == "argument_reporter_boolean") {
            r = "0.0";
            bool found = false;
            auto it = b->fields.find("VALUE");
            if (it != b->fields.end() && curArgs()) {
                std::u16string name = fieldName(it->second);
                const auto& names = *curArgs();
                for (size_t i = 0; i < names.size(); ++i)
                    if (names[i] == name) {
                        r = argIdents ? (*argIdents)[i]
                                      : "a" + std::to_string(i);
                        found = true;
                        break;
                    }
            }
            if (!found) { fail("unknown argument reporter"); r = "0.0"; }
        } else if (op.rfind("math_", 0) == 0 || op == "text" ||
                   op == "colour_picker") {
            r = litNum(b);
        } else if (op == "operator_add" || op == "operator_subtract" ||
                   op == "operator_multiply" || op == "operator_divide") {
            std::string a = in(b, "NUM1"), c = in(b, "NUM2");
            char o = op == "operator_add" ? '+' :
                     op == "operator_subtract" ? '-' :
                     op == "operator_multiply" ? '*' : '/';
            r = "(xnum_(" + a + ") " + o + " xnum_(" + c + "))";
        } else if (op == "operator_mod") {
            std::string a = in(b, "NUM1"), c = in(b, "NUM2");
            r = "xmod_(xnum_(" + a + "), xnum_(" + c + "))";
        } else if (op == "operator_round") {
            r = "xround_(xnum_(" + in(b, "NUM") + "))";
        } else if (op == "operator_mathop") {
            auto it = b->fields.find("OPERATOR");
            std::string f = it != b->fields.end()
                                ? SStr::toUtf8(fieldName(it->second))
                                : std::string();
            std::string a = "xnum_(" + in(b, "NUM") + ")";
            if (f == "abs") r = "abs(" + a + ")";
            else if (f == "floor") r = "floor(" + a + ")";
            else if (f == "ceiling") r = "ceil(" + a + ")";
            else if (f == "sqrt") r = "sqrt(" + a + ")";
            else if (f == "sin") r = "(xround_(sin(radians(" + a + ")) * 1e10) / 1e10)";
            else if (f == "cos") r = "(xround_(cos(radians(" + a + ")) * 1e10) / 1e10)";
            else if (f == "tan") r = "xtan_(" + a + ")";
            else if (f == "asin") r = "degrees(asin(" + a + "))";
            else if (f == "acos") r = "degrees(acos(" + a + "))";
            else if (f == "atan") r = "degrees(atan(" + a + "))";
            else if (f == "ln") r = "log(" + a + ")";
            else if (f == "log") r = "(log(" + a + ") / log(10.0))";
            else if (f == "e ^") r = "exp(" + a + ")";
            else if (f == "10 ^") r = "pow(10.0, " + a + ")";
            else { fail("unsupported mathop"); r = "0.0"; }
        } else if (op == "operator_random") {
            r = emitRandom(b);
        } else if (op == "operator_gt" || op == "operator_lt") {
            std::string a = in(b, "OPERAND1"), c = in(b, "OPERAND2");
            r = "(" + a + (op == "operator_gt" ? " > " : " < ") + c +
                " ? 1.0 : 0.0)";
        } else if (op == "operator_equals") {
            r = emitEquals(b);
        } else if (op == "operator_and") {
            std::string a = hoist(in(b, "OPERAND1"));
            std::string c = hoist(in(b, "OPERAND2"));
            r = "((" + a + " != 0.0 && " + c + " != 0.0) ? 1.0 : 0.0)";
        } else if (op == "operator_or") {
            std::string a = hoist(in(b, "OPERAND1"));
            std::string c = hoist(in(b, "OPERAND2"));
            r = "((" + a + " != 0.0 || " + c + " != 0.0) ? 1.0 : 0.0)";
        } else if (op == "operator_not") {
            r = "(" + in(b, "OPERAND") + " == 0.0 ? 1.0 : 0.0)";
        } else if (op == "data_itemoflist") {
            r = emitItemOf(b);
        } else if (op == "data_lengthoflist") {
            IdName ref = refOf(b, "LIST");
            int ch = 0;
            int k = listKind(ref, ch);
            if (k == 1) r = "float(pstkLen_)";
            else if (k == 2) r = "xalen_(" + std::to_string(ch) + "u)";
            else r = "xlen_(" + std::to_string(listSlot(ref)) + "u)";
        } else if (op == "motion_xposition") {
            r = "posX_";
        } else if (op == "motion_yposition") {
            r = "posY_";
        } else {
            fail("unsupported opcode in expression");
            r = "0.0";
        }
        --depth;
        return r;
    }

    const std::vector<std::u16string>* curArgs() const { return argsTop; }
    const std::vector<std::u16string>* argsTop = nullptr;
    const std::vector<std::string>* argIdents = nullptr;

    bool nonnumLiteral(const BlockInput* i, bool& isNone) {
        isNone = false;
        if (!i) return false;
        const Block* b = nullptr;
        if (!i->block.empty()) b = blocks->getBlock(i->block);
        else if (!i->shadow.empty()) b = blocks->getBlock(i->shadow);
        if (!b) return false;
        Value v;
        if (!litValue(b, v) || !v.isString()) return false;
        double d = cast::toNumberOrNaN(v);
        if (!std::isnan(d)) return false;
        SStr* s = v.asStrPtr();
        std::string u8 = s ? SStr::toUtf8(s->view()) : std::string();
        for (auto& c : u8) c = char(std::tolower((unsigned char)c));
        isNone = (u8 == "none");
        return true;
    }
    bool isItemOf(const BlockInput* i, const Block*& out) {
        out = nullptr;
        if (!i || i->block.empty()) return false;
        const Block* b = blocks->getBlock(i->block);
        if (b && b->opcode == "data_itemoflist") out = b;
        return out != nullptr;
    }
    struct Opnd {
        bool isItem = false;
        int kind = 0, ch = 0;
        std::string num, none;
        bool nnLit = false, noneLit = false;
        std::string code;
    };
    Opnd evalOpnd(const BlockInput* i) {
        Opnd o;
        if (!i) { o.code = "0.0"; return o; }
        if (nonnumLiteral(i, o.noneLit)) {
            if (!o.noneLit) fail("nonnumeric literal comparison");
            o.nnLit = true;
            return o;
        }
        const Block* it = nullptr;
        if (isItemOf(i, it)) {
            o.isItem = true;
            IdName ref = refOf(it, "LIST");
            o.kind = listKind(ref, o.ch);
            std::string idx = hoist(in(it, "INDEX"));
            if (o.kind == 1) {
                o.num = "xsget_(xnum_(" + idx + "))";
                o.none = "0.0";
            } else if (o.kind == 2) {
                o.num = "xaget_(" + std::to_string(o.ch) + "u, xnum_(" +
                        idx + "))";
                o.none = "0.0";
            } else {
                std::string li = std::to_string(listSlot(ref));
                o.num = "xlget_(" + li + "u, xnum_(" + idx + "))";
                o.none = "xlnone_(" + li + "u, xnum_(" + idx + "))";
            }
            o.code = o.num;
            return o;
        }
        o.code = inOr(i, Value::emptyStr());
        o.num = o.code;
        return o;
    }
    std::string emitEquals(const Block* b) {
        Opnd o1 = evalOpnd(inp(b, "OPERAND1"));
        Opnd o2 = evalOpnd(inp(b, "OPERAND2"));
        if (o1.nnLit && o2.nnLit)
            return o1.noneLit == o2.noneLit ? "1.0" : "0.0";
        if (o1.nnLit || o2.nnLit) {
            const Opnd& item = o1.nnLit ? o2 : o1;
            bool isNone = o1.nnLit ? o1.noneLit : o2.noneLit;
            if (!item.isItem || item.kind != 0) {
                fail("nonnumeric comparison");
                return "0.0";
            }
            return "(" + item.none + " == " + (isNone ? "1.0" : "0.0") +
                   " ? 1.0 : 0.0)";
        }
        if (o1.isItem || o2.isItem) {
            std::string none1 = o1.isItem ? o1.none : "0.0";
            std::string none2 = o2.isItem ? o2.none : "0.0";
            return "((" + o1.num + " == " + o2.num + " && " + none1 +
                   " == " + none2 + ") ? 1.0 : 0.0)";
        }
        return "(" + o1.num + " == " + o2.num + " ? 1.0 : 0.0)";
    }

    std::string emitItemOf(const Block* b) {
        IdName ref = refOf(b, "LIST");
        int ch = 0;
        int k = listKind(ref, ch);
        std::string idx = in(b, "INDEX");
        if (k == 1) return "xsget_(xnum_(" + idx + "))";
        if (k == 2)
            return "xaget_(" + std::to_string(ch) + "u, xnum_(" + idx + "))";
        return "xlget_(" + std::to_string(listSlot(ref)) + "u, xnum_(" +
               idx + "))";
    }

    std::string emitRandom(const Block* b) {
        const BlockInput* f = inp(b, "FROM");
        const BlockInput* t = inp(b, "TO");
        auto litOf = [&](const BlockInput* i, Value& v) -> bool {
            if (!i) return false;
            const Block* bb = nullptr;
            if (!i->block.empty()) bb = blocks->getBlock(i->block);
            else if (!i->shadow.empty()) bb = blocks->getBlock(i->shadow);
            if (!bb) return false;
            return litValue(bb, v);
        };
        Value fv, tv;
        if (!litOf(f, fv) || !litOf(t, tv)) {
            fail("non-literal random endpoint");
            return "0.0";
        }
        double nf = cast::toNumberOrNaN(fv), nt = cast::toNumberOrNaN(tv);
        if (std::isnan(nf) || std::isnan(nt)) {
            fail("nonnumeric random endpoint");
            return "0.0";
        }
        double lo = nf <= nt ? nf : nt, hi = nf <= nt ? nt : nf;
        if (lo == hi) return fmtF(lo);
        if (cast::isInt(fv) && cast::isInt(tv)) {
            return hoist("(" + fmtF(lo) + " + floor(xrnd_() * " +
                         fmtF(hi - lo + 1.0) + "))");
        }
        return hoist("(" + fmtF(lo) + " + xrnd_() * " + fmtF(hi - lo) +
                     ")");
    }

    void emitChain(const std::string& start, Ctx ctx) {
        auto stmts = chain(start);
        for (const Block* b : stmts) {
            if (failed) return;
            stmt(b, ctx);
        }
    }

    void stmt(const Block* b, Ctx ctx) {
        if (++depth > kNativeDepthCap) { fail("statement depth"); return; }
        const std::string& op = b->opcode;
        if (ctx.skipDraw && b->id != skipBlockId &&
            subtreeHas(b->id, isPenOrMotion)) {
            std::vector<const Block*> st;
            std::unordered_set<std::string> seen;
            st.push_back(b);
            for (auto& [name, i2] : b->inputs.items)
                if (!i2.block.empty())
                    collectIds(i2.block, st, seen);
            if (!failed) {
                for (const Block* x : st) {
                    if (x->opcode == "procedures_call") {
                        fail("skipped subtree calls procedure");
                        break;
                    }
                    std::string w;
                    if (writesVar(x, &w) && w != thisColorId &&
                        w != lastColorId) {
                        fail("skipped subtree writes variable");
                        break;
                    }
                    if (x->opcode == "data_addtolist" ||
                        x->opcode == "data_insertatlist" ||
                        x->opcode == "data_deleteoflist" ||
                        x->opcode == "data_deletealloflist" ||
                        x->opcode == "data_replaceitemoflist") {
                        fail("skipped subtree mutates list");
                        break;
                    }
                }
            }
            --depth;
            return;
        }
        if (b->id == skipBlockId) { --depth; return; }
        if (ctx.rayPrefix) {
            if (op == "pen_clear" ||
                (op == "procedures_call" &&
                 proccodeOf(b) == kProcTexSetup)) {
                --depth;
                return;
            }
        }
        if (op == "data_setvariableto") {
            IdName ref = refOf(b, "VARIABLE");
            std::string e = in(b, "VALUE");
            if (!ref.id.empty() && !varIsList(ref.id)) {
                int32_t s = varSlot(ref);
                line("v" + std::to_string(s) + " = " + e + ";");
                written.insert(ref.id);
            }
        } else if (op == "data_changevariableby") {
            IdName ref = refOf(b, "VARIABLE");
            std::string e = in(b, "VALUE");
            if (!ref.id.empty() && !varIsList(ref.id)) {
                int32_t s = varSlot(ref);
                line("v" + std::to_string(s) + " = xnum_(v" +
                     std::to_string(s) + ") + xnum_(" + e + ");");
                written.insert(ref.id);
            }
        } else if (op == "data_addtolist") {
            IdName ref = refOf(b, "LIST");
            std::string e = in(b, "ITEM");
            if (ref.id == stackId) line("xspush_(" + e + ");");
            else fail("addtolist on unsupported list");
        } else if (op == "data_deleteoflist") {
            IdName ref = refOf(b, "LIST");
            std::string e = in(b, "INDEX");
            if (ref.id == stackId) line("xsdel_(xnum_(" + e + "));");
            else fail("deleteoflist on unsupported list");
        } else if (op == "data_deletealloflist") {
            IdName ref = refOf(b, "LIST");
            if (ref.id == stackId) line("pstkLen_ = 0u;");
            else fail("deletealloflist on unsupported list");
        } else if (op == "data_replaceitemoflist") {
            IdName ref = refOf(b, "LIST");
            std::string idx = in(b, "INDEX");
            std::string e = in(b, "ITEM");
            int ch = 0;
            int k = listKind(ref, ch);
            if (k == 1) line("xsrepl_(xnum_(" + idx + "), " + e + ");");
            else if (k == 2)
                line("xaset_(" + std::to_string(ch) + "u, xnum_(" + idx +
                     "), " + e + ");");
            else fail("replaceitemoflist on readonly list");
        } else if (op == "control_repeat") {
            std::string e = in(b, "TIMES");
            std::string t = temp();
            line("{ float " + t + " = xround_(xnum_(" + e + "));");
            std::string i = temp();
            line("for (float " + i + " = 0.0; " + i + " < " + t + "; " + i +
                 " += 1.0) {");
            line("if (++iters_ > " + std::to_string(kNativeIterBudget) +
                 "u) { err_ |= 1u; return; }");
            Ctx sub = ctx;
            sub.rayPrefix = false;
            emitChain(inputBlock(b, "SUBSTACK"), sub);
            line("} }");
        } else if (op == "control_repeat_until") {
            line("while (true) {");
            { std::string c = in(b, "CONDITION");
              line("if (" + c + " != 0.0) break;"); }
            line("if (++iters_ > " + std::to_string(kNativeIterBudget) +
                 "u) { err_ |= 1u; return; }");
            Ctx sub = ctx;
            sub.rayPrefix = false;
            emitChain(inputBlock(b, "SUBSTACK"), sub);
            line("}");
        } else if (op == "control_if") {
            std::string c = in(b, "CONDITION");
            line("if (" + c + " != 0.0) {");
            Ctx sub = ctx;
            sub.rayPrefix = false;
            emitChain(inputBlock(b, "SUBSTACK"), sub);
            line("}");
        } else if (op == "control_if_else") {
            std::string c = in(b, "CONDITION");
            line("if (" + c + " != 0.0) {");
            Ctx sub = ctx;
            sub.rayPrefix = false;
            emitChain(inputBlock(b, "SUBSTACK"), sub);
            line("} else {");
            emitChain(inputBlock(b, "SUBSTACK2"), sub);
            line("}");
        } else if (op == "procedures_call") {
            emitCall(b, ctx);
        } else if (op == "pen_penDown") {
            line("xpendown_();");
        } else if (op == "pen_penUp") {
            line("penDown_ = false;");
        } else if (op == "pen_setPenSizeTo") {
            std::string e = in(b, "SIZE");
            line("penSize_ = xpensize_(xnum_(" + e + "));");
        } else if (op == "pen_changePenSizeBy") {
            std::string e = in(b, "SIZE");
            line("penSize_ = xpensize_(penSize_ + xnum_(" + e + "));");
        } else if (op == "pen_setPenColorToColor") {
            std::string e = in(b, "COLOR");
            line("{ vec4 _c = xcoldec_(xnum_(" + e +
                 ")); penR_ = _c.x; penG_ = _c.y; penB_ = _c.z; penA_ = _c.w; }");
        } else if (op == "motion_gotoxy") {
            std::string x = in(b, "X"), y = in(b, "Y");
            line("xgotoxy_(xnum_(" + x + "), xnum_(" + y + "));");
        } else if (op == "motion_setx") {
            std::string e = in(b, "X");
            line("xgotoxy_(xnum_(" + e + "), posY_);");
        } else if (op == "motion_sety") {
            std::string e = in(b, "Y");
            line("xgotoxy_(posX_, xnum_(" + e + "));");
        } else if (op == "motion_changexby") {
            std::string e = in(b, "DX");
            line("xgotoxy_(posX_ + xnum_(" + e + "), posY_);");
        } else if (op == "motion_changeyby") {
            std::string e = in(b, "DY");
            line("xgotoxy_(posX_, posY_ + xnum_(" + e + "));");
        } else {
            fail("unsupported opcode in statement");
        }
        --depth;
    }

    std::string inputBlock(const Block* b, const char* name) {
        const BlockInput* i = inp(b, name);
        return i ? i->block : std::string();
    }

    void emitCall(const Block* b, Ctx ctx) {
        std::string pc = proccodeOf(b);
        if (pc.empty()) { fail("procedures_call without proccode"); return; }
        if (emitting.count(pc)) { fail("recursive procedure call"); return; }
        if (procNames.find(pc) == procNames.end()) {
            std::vector<std::u16string> names;
            std::vector<std::string> ids;
            std::vector<Value> defs;
            if (!blocks->getProcedureParamNamesIdsAndDefaults(pc, names, ids,
                                                            defs)) {
                fail("missing procedure definition");
                return;
            }
            procNames[pc] = names;
            procIds[pc] = ids;
            procDefaults[pc] = defs;
            procDef[pc] = blocks->getProcedureDefinition(pc);
            if (procDef[pc].empty()) {
                fail("missing procedure definition");
                return;
            }
        }
        const auto& ids = procIds[pc];
        const auto& defs = procDefaults[pc];
        std::vector<std::string> args;
        for (size_t i = 0; i < ids.size(); ++i)
            args.push_back(hoist(inOr(inp(b, ids[i].c_str()),
                                      i < defs.size() ? defs[i]
                                                      : Value::num(0))));
        if (inlineLocals) {
            const Block* def = blk(procDef[pc]);
            if (!def) return;
            line("{");
            emitting.insert(pc);
            const std::vector<std::u16string>* savedArgs = argsTop;
            const std::vector<std::string>* savedIdents = argIdents;
            argsTop = &procNames[pc];
            argIdents = &args;
            Ctx sub;
            sub.skipDraw = ctx.skipDraw;
            sub.rayPrefix = false;
            emitChain(def->next, sub);
            argsTop = savedArgs;
            argIdents = savedIdents;
            emitting.erase(pc);
            line("}");
            line("if (err_ != 0u) return;");
            return;
        }
        auto fn = procFn.find(pc);
        if (fn == procFn.end()) {
            int32_t idx = int32_t(procFn.size());
            procFn[pc] = idx;
            const auto& names = procNames[pc];
            std::string proto = "void p" + std::to_string(idx) + "(";
            for (size_t i = 0; i < names.size(); ++i)
                proto += (i ? ", " : "") + std::string("float a") +
                         std::to_string(i);
            if (names.empty()) proto += "void";
            proto += ");";
            protos += proto + "\n";
            emitted += proto.size() + 1;
            if (emitted > kNativeSourceCap) {
                fail("source size cap");
                return;
            }
        }
        int32_t idx = procFn[pc];
        std::string call = "p" + std::to_string(idx) + "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) call += ", ";
            call += args[i];
        }
        call += ");";
        line(call);
        line("if (err_ != 0u) return;");
        if (!procEmitted.count(pc)) {
            procEmitted.insert(pc);
            emitting.insert(pc);
            const Block* def = blk(procDef[pc]);
            if (!def) { emitting.erase(pc); return; }
            std::string body = "void p" + std::to_string(idx) + "(";
            const auto& names = procNames[pc];
            for (size_t i = 0; i < names.size(); ++i)
                body += (i ? ", " : "") + std::string("float a") +
                        std::to_string(i);
            if (names.empty()) body += "void";
            body += ") {\n";
            std::string inner;
            std::string* saved = cur;
            const std::vector<std::u16string>* savedArgs = argsTop;
            cur = &inner;
            Ctx sub;
            sub.skipDraw = ctx.skipDraw;
            sub.rayPrefix = false;
            argsTop = &names;
            emitChain(def->next, sub);
            argsTop = savedArgs;
            cur = saved;
            body += inner + "}\n";
            funcs += body;
            emitted += body.size();
            emitting.erase(pc);
        }
    }

    void collectIds(const std::string& id, std::vector<const Block*>& out,
                    std::unordered_set<std::string>& seen, uint32_t guard = 0) {
        if (id.empty()) return;
        if (guard > kNativeDepthCap) { fail("subtree depth"); return; }
        std::string cur = id;
        while (!cur.empty() && !failed) {
            if (!seen.insert(cur).second) break;
            const Block* b = blocks->getBlock(cur);
            if (!b) { fail("missing block"); return; }
            out.push_back(b);
            for (auto& [name, in] : b->inputs.items) {
                if (!in.block.empty())
                    collectIds(in.block, out, seen, guard + 1);
            }
            cur = b->next;
        }
    }

    void collectReachable(const std::string& start,
                          std::vector<const Block*>& out,
                          std::unordered_set<std::string>& seen,
                          std::unordered_set<std::string>& seenProcs,
                          uint32_t guard = 0) {
        if (guard > kNativeDepthCap) { fail("call graph depth"); return; }
        std::vector<const Block*> st;
        collectIds(start, st, seen);
        for (const Block* b : st) {
            if (failed) return;
            out.push_back(b);
            if (b->opcode == "procedures_call") {
                std::string pc = proccodeOf(b);
                if (pc == kProcTexSetup) continue;
                if (!pc.empty() && seenProcs.insert(pc).second) {
                    std::string def = blocks->getProcedureDefinition(pc);
                    if (!def.empty()) {
                        const Block* d = blocks->getBlock(def);
                        if (d)
                            collectReachable(d->next, out, seen, seenProcs,
                                             guard + 1);
                    }
                }
            }
        }
    }

    bool chainWritesVar(const std::vector<const Block*>& stmts,
                        const std::string& id) {
        for (const Block* b : stmts) {
            std::string w;
            if (writesVar(b, &w) && w == id) return true;
        }
        return false;
    }

    bool analyze() {
        for (Target* t : blocks->runtime->targets)
            if (t->blocks == blocks) { owner = t; break; }
        if (!owner) { fail("no owning target"); return false; }
        stage = blocks->runtime->getTargetForStage();

        std::string defId = blocks->getProcedureDefinition(
            path ? kProcPath : kProcRay);
        if (defId.empty()) { fail("render procedure not found"); return false; }
        const Block* def = blk(defId);
        if (!def) return false;
        auto body = chain(def->next);
        const Block* outer = nullptr;
        size_t outerAt = 0;
        for (size_t i = 0; i < body.size(); ++i) {
            const Block* b = body[i];
            if (b->opcode != "control_repeat") continue;
            std::string sub = inputBlock(b, "SUBSTACK");
            if (!sub.empty() &&
                chainHas(sub, [](const std::string& op) {
                    return op == "control_repeat";
                })) {
                outer = b;
                outerAt = i;
                break;
            }
        }
        if (!outer) { fail("nested repeat not found"); return false; }
        outerIdx = outerAt;
        std::vector<const Block*> prefix(body.begin(), body.begin() + outerAt);
        std::vector<const Block*> suffix(body.begin() + outerAt + 1,
                                         body.end());
        auto outerBody = chain(inputBlock(outer, "SUBSTACK"));
        const Block* inner = nullptr;
        size_t innerAt = 0;
        for (size_t i = 0; i < outerBody.size(); ++i)
            if (outerBody[i]->opcode == "control_repeat") {
                inner = outerBody[i];
                innerAt = i;
                break;
            }
        if (!inner) { fail("inner repeat not found"); return false; }
        innerIdx = innerAt;
        std::vector<const Block*> preamble(outerBody.begin(),
                                           outerBody.begin() + innerAt);
        std::vector<const Block*> epilogue(outerBody.begin() + innerAt + 1,
                                           outerBody.end());
        auto innerBody = chain(inputBlock(inner, "SUBSTACK"));

        for (const Block* b : epilogue) {
            const BlockInput* si = stepInput(b);
            if (!si) continue;
            auto vit = b->fields.find("VARIABLE");
            if (vit == b->fields.end()) continue;
            rowStepBlock = b;
            rowVarId = vit->second.id;
            rowStepIn = si;
        }
        if (!rowStepBlock) { fail("row step not found"); return false; }
        if (!chainWritesVar(prefix, rowVarId)) {
            fail("row variable not initialised in prefix");
            return false;
        }
        for (const Block* b : innerBody) {
            const BlockInput* si = stepInput(b);
            if (!si) continue;
            auto vit = b->fields.find("VARIABLE");
            if (vit == b->fields.end()) continue;
            if (!chainWritesVar(preamble, vit->second.id))
                continue;
            colStepBlock = b;
            colVarId = vit->second.id;
            colStepIn = si;
        }
        if (!colStepBlock) { fail("column step not found"); return false; }

        resVarId.clear();
        auto divideVar = [&](const Block* rep, double lit) -> std::string {
            const BlockInput* ti = inp(rep, "TIMES");
            if (!ti || ti->block.empty()) return {};
            const Block* d = blocks->getBlock(ti->block);
            if (!d || d->opcode != "operator_divide") return {};
            double v;
            if (!literalNum(inp(d, "NUM1"), v) || v != lit) return {};
            const BlockInput* n2 = inp(d, "NUM2");
            if (!n2 || n2->block.empty()) return {};
            const Block* vb = blocks->getBlock(n2->block);
            if (!vb || vb->opcode != "data_variable") return {};
            auto it = vb->fields.find("VARIABLE");
            return it == vb->fields.end() ? std::string() : it->second.id;
        };
        std::string resO = divideVar(outer, 360.0);
        std::string resI = divideVar(inner, 480.0);
        if (resO.empty() || resO != resI) {
            fail("unsupported pixel dimensions");
            return false;
        }
        resVarId = resO;
        auto stepIsRes = [&](const BlockInput* si) {
            if (!si || si->block.empty()) return false;
            const Block* vb = blocks->getBlock(si->block);
            if (!vb || vb->opcode != "data_variable") return false;
            auto it = vb->fields.find("VARIABLE");
            return it != vb->fields.end() && it->second.id == resVarId;
        };
        if (!stepIsRes(rowStepIn) || !stepIsRes(colStepIn)) {
            fail("step is not res variable");
            return false;
        }
        {
            std::unordered_set<std::string> called;
            std::vector<const Block*> st;
            std::unordered_set<std::string> seen;
            for (const Block* b : innerBody) collectIds(b->id, st, seen);
            for (const Block* x : st)
                if (x->opcode == "procedures_call")
                    called.insert(proccodeOf(x));
            static const char* kReqRay[] = {"r.trace", "r.shade", "r.reflect"};
            static const char* kReqPath[] = {"r.trace", "pt.sample"};
            const char** req = path ? kReqPath : kReqRay;
            int nReq = path ? 2 : 3;
            for (int i = 0; i < nReq; ++i)
                if (!called.count(req[i])) {
                    fail("required trace procedure not called");
                    return false;
                }
        }

        for (const Block* b : innerBody)
            if (b->opcode == "data_replaceitemoflist") {
                IdName ref = refOf(b, "LIST");
                if (ref.id.empty()) continue;
                const Variable* lv = nullptr;
                auto lo = owner->variables.find(ref.id);
                if (lo != owner->variables.end()) lv = &lo->second;
                else if (stage) {
                    auto ls = stage->variables.find(ref.id);
                    if (ls != stage->variables.end()) lv = &ls->second;
                }
                bool isAccum =
                    lv && lv->type == Variable::kListType &&
                    (lv->name == u"pt:r" || lv->name == u"pt:g" ||
                     lv->name == u"pt:b");
                if (!isAccum) {
                    fail("unsupported accum list");
                    return false;
                }
                if (!accumCh.count(ref.id)) {
                    int32_t ch = int32_t(accumCh.size());
                    accumCh[ref.id] = ch;
                    prog.accumVars[ch] = gv(ref);
                }
                const BlockInput* ix = inp(b, "INDEX");
                if (ix && !ix->block.empty()) {
                    const Block* vb = blocks->getBlock(ix->block);
                    if (vb && vb->opcode == "data_variable") {
                        auto it = vb->fields.find("VARIABLE");
                        if (it != vb->fields.end() && piVarId.empty())
                            piVarId = it->second.id;
                    }
                }
            }
        if (path) {
            if (accumCh.size() != 3) {
                fail("path accum channels != 3");
                return false;
            }
            auto pv = owner->variables.find(piVarId);
            if (piVarId.empty() || pv == owner->variables.end() ||
                pv->second.name != u"pt.pi") {
                fail("pt.pi index not found");
                return false;
            }
            int piWrites = 0;
            bool piStep1 = false;
            for (const Block* b : innerBody) {
                std::string w;
                if (!writesVar(b, &w) || w != piVarId) continue;
                ++piWrites;
                const BlockInput* si = stepInput(b);
                double sv;
                if (si && literalNum(si, sv) && sv == 1.0) piStep1 = true;
            }
            if (piWrites != 1 || !piStep1) {
                fail("pt.pi increment not exactly +1");
                return false;
            }
        }

        std::unordered_set<std::string> skipIds;
        if (!path) {
            auto mark = [&](const std::vector<const Block*>& stmts) {
                for (const Block* b : stmts)
                    if (subtreeHas(b->id, isPenOrMotion)) {
                        std::vector<const Block*> st;
                        std::unordered_set<std::string> s;
                        collectIds(b->id, st, s);
                        for (const Block* x : st) skipIds.insert(x->id);
                    }
            };
            mark(preamble);
            mark(innerBody);
            mark(epilogue);
            mark(suffix);
        }
        {
            std::vector<const Block*> all;
            std::unordered_set<std::string> seen, seenProcs;
            collectReachable(def->next, all, seen, seenProcs);
            for (const Block* b : all) {
                if (skipIds.count(b->id)) continue;
                const std::string& op = b->opcode;
                bool mut = op == "data_addtolist" ||
                           op == "data_insertatlist" ||
                           op == "data_deleteoflist" ||
                           op == "data_deletealloflist" ||
                           op == "data_replaceitemoflist";
                if (!mut) continue;
                IdName ref = refOf(b, "LIST");
                if (ref.id.empty() || accumCh.count(ref.id)) continue;
                if (stackId.empty()) stackId = ref.id;
                else if (stackId != ref.id) {
                    fail("multiple private stack candidates");
                    return false;
                }
            }
            if (!stackId.empty()) {
                auto it = owner->variables.find(stackId);
                if (it == owner->variables.end() ||
                    it->second.type != Variable::kListType ||
                    it->second.name != u"r.stack") {
                    fail("private list is not local r.stack");
                    return false;
                }
                prog.stackVar = {it->first, it->second.name};
            }
        }

        for (const Block* b : innerBody)
            if (b->opcode == "control_if" || b->opcode == "control_if_else") {
                std::vector<const Block*> st;
                std::unordered_set<std::string> seen;
                collectIds(inputBlock(b, "SUBSTACK"), st, seen);
                for (const Block* x : st) {
                    if (x->opcode != "data_setvariableto") continue;
                    const BlockInput* vin = inp(x, "VALUE");
                    if (!vin || vin->block.empty()) continue;
                    const Block* vb = blocks->getBlock(vin->block);
                    if (!vb || vb->opcode != "data_variable") continue;
                    auto vit = vb->fields.find("VARIABLE");
                    auto tit = x->fields.find("VARIABLE");
                    if (vit == vb->fields.end() || tit == x->fields.end())
                        continue;
                    lastColorId = tit->second.id;
                    thisColorId = vit->second.id;
                }
            }
        if (thisColorId.empty())
            for (const Block* b : innerBody) {
                std::vector<const Block*> st;
                std::unordered_set<std::string> seen;
                collectIds(b->id, st, seen);
                for (const Block* x : st) {
                    if (x->opcode != "pen_setPenColorToColor") continue;
                    const BlockInput* cin = inp(x, "COLOR");
                    if (!cin || cin->block.empty()) continue;
                    const Block* vb = blocks->getBlock(cin->block);
                    if (vb && vb->opcode == "data_variable") {
                        auto it = vb->fields.find("VARIABLE");
                        if (it != vb->fields.end())
                            thisColorId = it->second.id;
                    }
                }
            }
        if (thisColorId.empty() && !path) {
            fail("color variable not found");
            return false;
        }
        for (const Block* b : prefix)
            if (stepInput(b)) {
                auto vit = b->fields.find("VARIABLE");
                if (vit != b->fields.end()) { pnVarId = vit->second.id; break; }
            }
        return !failed;
    }

    bool literalNum(const BlockInput* i, double& out) {
        if (!i) return false;
        const Block* b = nullptr;
        if (!i->block.empty()) b = blocks->getBlock(i->block);
        else if (!i->shadow.empty()) b = blocks->getBlock(i->shadow);
        if (!b) return false;
        Value v;
        if (!litValue(b, v)) return false;
        out = cast::toNumberOrNaN(v);
        return !std::isnan(out);
    }
    const BlockInput* stepInput(const Block* b) {
        if (b->opcode == "data_changevariableby")
            return inp(b, "VALUE");
        if (b->opcode != "data_setvariableto") return nullptr;
        auto vit = b->fields.find("VARIABLE");
        if (vit == b->fields.end()) return nullptr;
        const BlockInput* vin = inp(b, "VALUE");
        if (!vin || vin->block.empty()) return nullptr;
        const Block* add = blocks->getBlock(vin->block);
        if (!add || add->opcode != "operator_add") return nullptr;
        auto isVar = [&](const BlockInput* i) {
            if (!i || i->block.empty()) return false;
            const Block* vb = blocks->getBlock(i->block);
            if (!vb || vb->opcode != "data_variable") return false;
            auto f = vb->fields.find("VARIABLE");
            return f != vb->fields.end() && f->second.id == vit->second.id;
        };
        const BlockInput* n1 = inp(add, "NUM1");
        const BlockInput* n2 = inp(add, "NUM2");
        if (isVar(n1) && !isVar(n2)) return n2;
        if (isVar(n2) && !isVar(n1)) return n1;
        return nullptr;
    }

    IdName varRef(const std::string& id) const {
        auto it = owner->variables.find(id);
        if (it != owner->variables.end()) return {id, it->second.name};
        if (stage) {
            auto sit = stage->variables.find(id);
            if (sit != stage->variables.end()) return {id, sit->second.name};
        }
        return {id, {}};
    }
    void emitOverrides() {
        if (!rowVarId.empty() && rowStepIn) {
            std::string s = inOr(rowStepIn, Value::num(0));
            line("v" + std::to_string(varSlot(varRef(rowVarId))) +
                 " += float(row_) * (" + s + ");");
        }
    }
    void emitColOverride() {
        if (!colVarId.empty() && colStepIn) {
            std::string s = inOr(colStepIn, Value::num(0));
            line("v" + std::to_string(varSlot(varRef(colVarId))) +
                 " += float(col_) * (" + s + ");");
        }
    }

    bool emit() {
        sharedStack = std::getenv("XYO_GPU_SHARED_STACK") != nullptr;
        const char* il = std::getenv("XYO_GPU_INLINE_LOCALS");
        inlineLocals = il && std::string(il) == "1";
        prog.sharedStack = sharedStack;
        prog.inlineLocals = inlineLocals;
        if (!analyze()) return false;
        std::string defId = blocks->getProcedureDefinition(
            path ? kProcPath : kProcRay);
        const Block* def = blk(defId);
        if (!def) return false;
        auto body = chain(def->next);
        const Block* outer = body[outerIdx];
        auto outerBody = chain(inputBlock(outer, "SUBSTACK"));
        const Block* inner = outerBody[innerIdx];

        std::string kb = "void kernelBody() {\n";
        std::string kcode;
        cur = &kcode;
        Ctx ctx;
        ctx.rayPrefix = !path;
        for (size_t i = 0; i < outerIdx && !failed; ++i) stmt(body[i], ctx);
        emitOverrides();
        ctx.rayPrefix = false;
        ctx.skipDraw = !path;
        for (size_t i = 0; i < innerIdx && !failed; ++i)
            stmt(outerBody[i], ctx);
        emitColOverride();
        if (!colVarId.empty() && !rowVarId.empty())
            line("if (pixel_ == 0u) { ob[kPenOff + 8u] = floatBitsToUint(v" +
                 std::to_string(varSlot(varRef(colVarId))) +
                 "); ob[kPenOff + 9u] = floatBitsToUint(v" +
                 std::to_string(varSlot(varRef(rowVarId))) + "); }");
        if (path && !piVarId.empty())
            line("v" + std::to_string(varSlot(varRef(piVarId))) +
                 " = float(pixel_);");
        skipBlockId.clear();
        emitChain(inputBlock(inner, "SUBSTACK"), ctx);
        skipBlockId = rowStepBlock ? rowStepBlock->id : std::string();
        for (size_t i = innerIdx + 1; i < outerBody.size() && !failed; ++i) {
            const Block* b = outerBody[i];
            if (b->id == skipBlockId) continue;
            std::string w;
            if (writesVar(b, &w)) { fail("unsupported epilogue write"); break; }
            stmt(b, ctx);
        }
        for (size_t i = outerIdx + 1; i < body.size() && !failed; ++i) {
            const Block* b = body[i];
            std::string w;
            if (writesVar(b, &w)) { fail("unsupported suffix write"); break; }
            stmt(b, ctx);
        }
        cur = nullptr;

        if (failed) return false;

        if (!thisColorId.empty()) varSlot(varRef(thisColorId));
        if (!lastColorId.empty()) varSlot(varRef(lastColorId));
        auto slot = [&](const std::string& id) -> int32_t {
            auto it = varSlots.find(id);
            return it == varSlots.end() ? -1 : it->second;
        };
        prog.slotRowVar = slot(rowVarId);
        prog.slotColVar = slot(colVarId);
        prog.slotPI = slot(piVarId);
        prog.slotPN = slot(pnVarId);
        prog.slotRes = slot(resVarId);
        prog.slotThisColor = slot(thisColorId);
        prog.slotLastColor = slot(lastColorId);
        auto stepSlot = [&](const BlockInput* si) -> int32_t {
            if (!si || si->block.empty()) return -1;
            const Block* vb = blocks->getBlock(si->block);
            if (!vb || vb->opcode != "data_variable") return -1;
            auto it = vb->fields.find("VARIABLE");
            return it == vb->fields.end() ? -1 : slot(it->second.id);
        };
        prog.slotStepX = stepSlot(colStepIn);
        prog.slotStepY = stepSlot(rowStepIn);
        for (const std::string& w : written) {
            auto it = varSlots.find(w);
            if (it != varSlots.end()) prog.writtenVars.push_back(it->second);
        }
        std::sort(prog.writtenVars.begin(), prog.writtenVars.end());
        prog.path = path;

        const uint32_t nWb = uint32_t(prog.writtenVars.size());
        std::string src;
        src.reserve(16384);
        src += "#version 430 core\n";
        src += "layout(local_size_x=64) in;\n";
        src += "layout(std430, binding=0) readonly buffer P { float pr[]; };\n";
        src += "layout(std430, binding=1) readonly buffer LM { uvec2 lmeta[]; };\n";
        src += "layout(std430, binding=2) writeonly buffer R { vec4 recs[]; };\n";
        src += "layout(std430, binding=3) writeonly buffer N { uint cnts[]; };\n";
        src += "layout(std430, binding=4) buffer O { uint ob[]; };\n";
        src += "layout(std430, binding=5) readonly buffer LE { vec2 le[]; };\n";
        src += "layout(std430, binding=6) readonly buffer AI { vec4 acin[]; };\n";
        src += "layout(std430, binding=7) writeonly buffer AO { vec4 acout[]; };\n";
        src += "uniform uint uBasePixel;\nuniform uint uDispatchPixels;\n";
        src += "uniform uint uCols;\nuniform uint uTotal;\nuniform uint uSeed;\n";
        src += "const uint kPenOff = " + std::to_string(2 + nWb) + "u;\n";
        src += "const uint kColOff = " + std::to_string(2 + nWb + 10) + "u;\n";
        src += "uint pixel_, col_, row_, err_, recN_, iters_, pstkLen_, seed_;\n";
        src += "float posX_, posY_, penSize_, penR_, penG_, penB_, penA_;\n";
        src += "bool penDown_;\n";
        if (sharedStack) {
            src += "shared float pstkShared_[128 * 64];\n";
            src += "uint localLane_;\n";
        } else {
            src += "float pstk_[128];\n";
        }
        src += "vec4 acur_;\n";
        if (!inlineLocals)
            for (size_t i = 0; i < prog.vars.size(); ++i)
                src += "float v" + std::to_string(i) + ";\n";
        src += R"GLSL(
float xround_(float a) { return floor(a + 0.5); }
float xnum_(float v) { return isnan(v) ? 0.0 : v; }
float xmod_(float a, float b) { return a - b * floor(a / b); }
float xtan_(float a) {
    a = mod(a, 360.0);
    if (a == -270.0 || a == 90.0) return uintBitsToFloat(0x7F800000u);
    if (a == -90.0 || a == 270.0) return uintBitsToFloat(0xFF800000u);
    return xround_(tan(radians(a)) * 1e10) / 1e10;
}
float xrnd_() {
    seed_ ^= seed_ << 13u; seed_ ^= seed_ >> 17u; seed_ ^= seed_ << 5u;
    return float(seed_ >> 8u) * (1.0 / 16777216.0);
}
float xlen_(uint li) { return float(lmeta[li].y); }
float xlget_(uint li, float idx) {
    uvec2 m = lmeta[li];
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(m.y))) return 0.0;
    return le[m.x + uint(fi) - 1u].x;
}
float xlnone_(uint li, float idx) {
    uvec2 m = lmeta[li];
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(m.y))) return 0.0;
    return le[m.x + uint(fi) - 1u].y;
}
)GLSL";
        if (sharedStack) {
            src += R"GLSL(
float xsget_(float idx) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return 0.0;
    return pstkShared_[(uint(fi) - 1u) * 64u + localLane_];
}
void xspush_(float v) {
    if (pstkLen_ >= 128u) { err_ |= 8u; return; }
    pstkShared_[(pstkLen_++) * 64u + localLane_] = v;
}
void xsdel_(float idx) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return;
    uint i = uint(fi) - 1u;
    for (uint k = i; k + 1u < pstkLen_; ++k)
        pstkShared_[k * 64u + localLane_] =
            pstkShared_[(k + 1u) * 64u + localLane_];
    --pstkLen_;
}
void xsrepl_(float idx, float v) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return;
    pstkShared_[(uint(fi) - 1u) * 64u + localLane_] = v;
}
)GLSL";
        } else {
            src += R"GLSL(
float xsget_(float idx) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return 0.0;
    return pstk_[uint(fi) - 1u];
}
void xspush_(float v) {
    if (pstkLen_ >= 128u) { err_ |= 8u; return; }
    pstk_[pstkLen_++] = v;
}
void xsdel_(float idx) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return;
    uint i = uint(fi) - 1u;
    for (uint k = i; k + 1u < pstkLen_; ++k) pstk_[k] = pstk_[k + 1u];
    --pstkLen_;
}
void xsrepl_(float idx, float v) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= float(pstkLen_))) return;
    pstk_[uint(fi) - 1u] = v;
}
)GLSL";
        }
        src += R"GLSL(
float xalen_(uint ch) { return pr[20u + ch]; }
float xaget_(uint ch, float idx) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= xalen_(ch))) return 0.0;
    if (uint(fi) != pixel_ + 1u) { err_ |= 4u; return 0.0; }
    return acur_[ch];
}
void xaset_(uint ch, float idx, float v) {
    float fi = floor(idx);
    if (!(fi >= 1.0 && fi <= xalen_(ch))) return;
    if (uint(fi) != pixel_ + 1u) { err_ |= 4u; return; }
    acur_[ch] = v;
}
vec4 xcoldec_(float n) {
    float nn = n < 0.0 ? floor(4294967296.0 + n) : floor(n);
    uint ci = uint(mod(nn, 4294967296.0));
    uint a = (ci >> 24u) & 0xFFu;
    vec3 c = vec3(float((ci >> 16u) & 0xFFu),
                  float((ci >> 8u) & 0xFFu), float(ci & 0xFFu)) / 255.0;
    float al = (a > 0u && a < 255u) ? float(a) / 255.0 : 1.0;
    return vec4(c, al);
}
void xstroke_(float x0, float y0, float x1, float y1) {
    if (isnan(x0) || isinf(x0) || isnan(y0) || isinf(y0) ||
        isnan(x1) || isinf(x1) || isnan(y1) || isinf(y1)) return;
    if (recN_ >= uint(pr[24])) { err_ |= 2u; return; }
    uint i = (pixel_ * uint(pr[24]) + recN_) * 3u;
    recs[i] = vec4(x0, y0, x1, y1);
    recs[i + 1u] = vec4(penSize_, penR_, penG_, penB_);
    recs[i + 2u] = vec4(penA_, 0.0, 0.0, 0.0);
    ++recN_;
}
void xgotoxy_(float nx, float ny) {
    float fl = pr[5], fr = pr[6], fb = pr[7], ft = pr[8];
    if (nx > fr) nx = floor(fr); else if (nx < fl) nx = ceil(fl);
    if (ny > ft) ny = floor(ft); else if (ny < fb) ny = ceil(fb);
    if (penDown_) xstroke_(posX_, posY_, nx, ny);
    posX_ = nx; posY_ = ny;
}
void xpendown_() { penDown_ = true; xstroke_(posX_, posY_, posX_, posY_); }
float xpensize_(float v) {
    return pr[18] != 0.0 ? clamp(v, 1.0, 1200.0) : max(v, 0.0);
}
)GLSL";
        src += protos;
        std::string tail;
        tail += "cnts[pixel_] = recN_;\n";
        if (prog.slotThisColor >= 0)
            tail += "ob[kColOff + pixel_] = floatBitsToUint(v" +
                    std::to_string(prog.slotThisColor) + ");\n";
        else
            tail += "ob[kColOff + pixel_] = 0u;\n";
        if (!accumCh.empty()) tail += "acout[pixel_] = acur_;\n";
        tail += "#if XYO_NATIVE_CAPTURE\n";
        for (uint32_t i = 0; i < nWb; ++i)
            tail += "ob[" + std::to_string(2 + i) +
                    "u] = floatBitsToUint(v" +
                    std::to_string(prog.writtenVars[i]) + ");\n";
        tail += "ob[kPenOff + 0u] = floatBitsToUint(penSize_);\n";
        tail += "ob[kPenOff + 1u] = floatBitsToUint(penR_);\n";
        tail += "ob[kPenOff + 2u] = floatBitsToUint(penG_);\n";
        tail += "ob[kPenOff + 3u] = floatBitsToUint(penB_);\n";
        tail += "ob[kPenOff + 4u] = floatBitsToUint(penA_);\n";
        tail += "ob[kPenOff + 5u] = floatBitsToUint(posX_);\n";
        tail += "ob[kPenOff + 6u] = floatBitsToUint(posY_);\n";
        tail += "ob[kPenOff + 7u] = floatBitsToUint(penDown_ ? 1.0 : 0.0);\n";
        tail += "#endif\n";

        src += funcs;
        if (inlineLocals)
            for (size_t i = 0; i < prog.vars.size(); ++i)
                kb += "float v" + std::to_string(i) + " = pr[" +
                      std::to_string(kNativeParamBase + i) + "];\n";
        kb += kcode;
        if (inlineLocals) {
            kb += "if (err_ != 0u) return;\n";
            kb += tail;
        }
        kb += "}\n";
        src += kb;
        src += "void main() {\n";
        src += "uint lid_ = gl_GlobalInvocationID.x;\n";
        src += "if (lid_ >= uDispatchPixels) return;\n";
        src += "pixel_ = uBasePixel + lid_;\n";
        src += "if (pixel_ >= uTotal) return;\n";
        if (sharedStack)
            src += "localLane_ = gl_LocalInvocationID.x;\n";
        src += "err_ = 0u; recN_ = 0u; iters_ = 0u; pstkLen_ = 0u;\n";
        src += "seed_ = pixel_ * 2654435761u ^ uSeed;\n";
        src += "if (seed_ == 0u) seed_ = 2654435769u;\n";
        src += "posX_ = pr[9]; posY_ = pr[10]; penSize_ = pr[11];\n";
        src += "penR_ = pr[12]; penG_ = pr[13]; penB_ = pr[14]; penA_ = pr[15];\n";
        src += "penDown_ = pr[16] != 0.0;\n";
        if (accumCh.empty())
            src += "acur_ = vec4(0.0);\n";
        else
            src += "acur_ = (pr[19] != 0.0) ? vec4(0.0) : acin[pixel_];\n";
        src += "col_ = pixel_ % uCols; row_ = pixel_ / uCols;\n";
        if (!inlineLocals)
            for (size_t i = 0; i < prog.vars.size(); ++i)
                src += "v" + std::to_string(i) + " = pr[" +
                       std::to_string(kNativeParamBase + i) + "];\n";
        src += "kernelBody();\n";
        src += "if (err_ != 0u) { atomicOr(ob[0], err_); return; }\n";
        if (!inlineLocals) src += tail;
        src += "}\n";
        if (src.size() > kNativeSourceCap) {
            fail("source size cap");
            return false;
        }
        prog.source = std::move(src);
        return !failed;
    }
};

}

std::optional<NativeProgram> compileRayKernel(Blocks* blocks, bool path,
                                              std::string* why) {
    if (why) why->clear();
    if (!blocks || !blocks->runtime) {
        if (why) *why = "no blocks/runtime";
        return std::nullopt;
    }
    Gen g;
    g.blocks = blocks;
    g.path = path;
    g.why = why;
    if (!g.emit()) return std::nullopt;
    return std::move(g.prog);
}

}
