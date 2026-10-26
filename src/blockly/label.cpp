// xyo-cpp: Blockly のラベル文字列の実装。
#include "label.hpp"

#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "model/blocks.hpp"

namespace xyo {
namespace blockly {

std::string fieldStr(Block* b, const std::string& name) {
    if (!b) return "";
    auto it = b->fields.find(name);
    if (it == b->fields.end()) return "";
    SStr* s = cast::toString(it->second.value);
    return s ? SStr::toUtf8(s->view()) : "";
}

std::string shortBlockLabel(const edit::BlockDef* d) {
    if (!d) return "?";
    std::string s = d->label;
    for (char ch = '1'; ch <= '9'; ch++) {
        std::string ph = std::string("%") + ch;
        size_t p;
        while ((p = s.find(ph)) != std::string::npos)
            s.replace(p, 2, "\u25a1");
    }
    return s;
}

std::string shadowValue(Blocks* blocks, Block* sh) {
    if (!sh) return "";
    if (sh->opcode == "math_number" || sh->opcode == "math_angle" ||
        sh->opcode == "math_positive_number" || sh->opcode == "math_whole_number" ||
        sh->opcode == "math_integer")
        return fieldStr(sh, "NUM");
    if (sh->opcode == "text") return fieldStr(sh, "TEXT");
    if (sh->opcode == "colour_picker") {
        std::string v = fieldStr(sh, "COLOUR");
        return v.empty() ? "#000000" : v;
    }
    const edit::BlockDef* sd = edit::findBlock(sh->opcode);
    if (sd) {
        for (const auto& a : sd->args)
            if (a.kind == edit::ArgKind::Dropdown) {
                std::string val;
                if (!sd->args.empty())
                    val = fieldStr(sh, a.name);
                if (val.empty()) val = fieldStr(sh, sh->fields.begin()->first);
                for (size_t i = 0; i + 1 < a.options.size(); i += 2)
                    if (a.options[i + 1] == val) return a.options[i];
                return val.empty() ? (a.options.size() ? a.options[0] : "")
                                   : val;
            }
        // menu 等: 自身のフィールドを表示
        if (!sh->fields.empty()) {
            std::string v = fieldStr(sh, sh->fields.begin()->first);
            if (!v.empty()) return v;
        }
        return shortBlockLabel(sd);
    }
    // カタログ外（*_menu_* 等）: 先頭フィールドをそのまま表示
    if (!sh->fields.empty())
        return fieldStr(sh, sh->fields.begin()->first);
    return sh->opcode;
}

std::string argDisplay(Blocks* blocks, Block* b, const edit::ArgDef& a) {
    switch (a.kind) {
    case edit::ArgKind::Number:
    case edit::ArgKind::Angle: {
        std::string v = fieldStr(b, a.name);
        return v.empty() ? a.dflt : v;
    }
    case edit::ArgKind::Dropdown:
    case edit::ArgKind::Variable: {
        std::string v = fieldStr(b, a.name);
        for (size_t i = 0; i + 1 < a.options.size(); i += 2)
            if (a.options[i + 1] == v) return a.options[i];
        return v.empty() ? (a.options.size() ? a.options[0] : "") : v;
    }
    case edit::ArgKind::Image: return "";
    case edit::ArgKind::Value: {
        if (blocks && b) {
            const BlockInput* in = b->inputs.find(a.name);
            if (in && !in->block.empty()) {
                Block* c = blocks->getBlock(in->block);
                if (c && c->shadow) return shadowValue(blocks, c);
                return blockLabelText(blocks, c);
            }
        }
        return "\u25a1";
    }
    default: return "";
    }
}

std::string blockLabelText(Blocks* blocks, Block* b) {
    if (!b) return "";
    const edit::BlockDef* d = edit::findBlock(b->opcode);
    if (!d) {
        if (b->mutation && b->mutation->has("proccode"))
            return b->mutation->attr("proccode");
        return b->opcode;
    }
    std::string s = d->label, out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '%' && i + 1 < s.size() && s[i + 1] >= '1' &&
            s[i + 1] <= '9') {
            int n = s[i + 1] - '0';
            if (n - 1 < (int)d->args.size())
                out += argDisplay(blocks, b, d->args[n - 1]);
            else
                out += "\u25a1";
            i += 2;
        } else {
            out += s[i];
            i++;
        }
    }
    return out;
}

bool shadowIsField(Block* sh, std::string* fieldName, bool* isNum) {
    if (!sh) return false;
    const char* op = sh->opcode.c_str();
    bool num = false;
    const char* fname = nullptr;
    if (sh->opcode == "math_number" || sh->opcode == "math_positive_number" ||
        sh->opcode == "math_whole_number" || sh->opcode == "math_integer") {
        fname = "NUM";
        num = true;
    } else if (sh->opcode == "math_angle") {
        fname = "NUM";
        num = true;
    } else if (sh->opcode == "text") {
        fname = "TEXT";
        num = false;
    } else if (sh->opcode == "colour_picker") {
        fname = "COLOUR";
        num = false;
    } else {
        (void)op;
        return false;
    }
    if (fieldName) *fieldName = fname;
    if (isNum) *isNum = num;
    return true;
}

} // namespace blockly
} // namespace xyo
