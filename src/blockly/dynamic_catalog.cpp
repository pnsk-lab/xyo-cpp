// xyo-cpp: 実行時に登録された拡張ブロックのエディタカタログ。
#include "dynamic_catalog.hpp"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

namespace xyo::edit {
namespace {

struct DynamicCatalog {
    std::deque<BlockDef> defs;
    std::deque<CategoryDef> categories;
    std::unordered_map<std::string, const BlockDef*> byOpcode;
    std::vector<std::string> ids;
};

DynamicCatalog& catalog() {
    static DynamicCatalog c;
    return c;
}

uint32_t scaleColor(uint32_t color, float factor) {
    auto channel = [&](int shift) {
        const uint32_t v = (color >> shift) & 0xFFu;
        return uint32_t(std::clamp(float(v) * factor, 0.0f, 255.0f));
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

Shape shapeOf(const ExtensionBlockDef& block) {
    if (block.shape == "reporter") return Shape::Reporter;
    if (block.shape == "boolean") return Shape::Boolean;
    if (block.shape == "hat" || block.hat) return Shape::Hat;
    if (block.shape == "end") return Shape::End;
    return Shape::Stack;
}

ArgDef makeArg(const ExtensionArgDef& arg) {
    ArgDef out;
    out.name = arg.name;
    if (arg.type == "dropdown") {
        out.kind = ArgKind::Dropdown;
        out.dflt = arg.dflt;
        out.options = arg.options;
    } else if (arg.type == "number") {
        out.kind = ArgKind::Value;
        out.shadow = "math_number";
        out.shadowField = "NUM";
        out.shadowDefault = arg.dflt.empty() ? "0" : arg.dflt;
    } else if (arg.type == "angle") {
        out.kind = ArgKind::Value;
        out.shadow = "math_angle";
        out.shadowField = "NUM";
        out.shadowDefault = arg.dflt.empty() ? "90" : arg.dflt;
    } else if (arg.type == "boolean") {
        out.kind = ArgKind::Value;
    } else {
        out.kind = ArgKind::Value;
        out.shadow = "text";
        out.shadowField = "TEXT";
        out.shadowDefault = arg.dflt;
    }
    return out;
}

BlockDef toBlockDef(const ExtensionDef& def,
                    const ExtensionBlockDef& block) {
    BlockDef out;
    out.opcode = block.opcode;
    out.label = block.label;
    out.shape = shapeOf(block);
    out.primary = def.color;
    out.secondary = scaleColor(def.color, 0.92f);
    out.tertiary = scaleColor(def.color, 0.82f);
    for (const ExtensionArgDef& arg : block.args)
        out.args.push_back(makeArg(arg));
    return out;
}

} // namespace

void registerExtensionDef(const ExtensionDef& def) {
    DynamicCatalog& c = catalog();
    if (std::find(c.ids.begin(), c.ids.end(), def.id) != c.ids.end()) return;

    CategoryDef category;
    category.id = def.id;
    category.name = def.name.empty() ? def.id : def.name;
    category.primary = def.color;
    for (const ExtensionBlockDef& block : def.blocks) {
        if (c.byOpcode.count(block.opcode)) continue;
        c.defs.push_back(toBlockDef(def, block));
        const BlockDef* stored = &c.defs.back();
        c.byOpcode.emplace(block.opcode, stored);
        category.items.push_back(block.opcode);
    }
    if (!category.items.empty()) c.categories.push_back(std::move(category));
    c.ids.push_back(def.id);
}

const BlockDef* findDynamicBlock(std::string_view opcode) {
    auto it = catalog().byOpcode.find(std::string(opcode));
    return it == catalog().byOpcode.end() ? nullptr : it->second;
}

bool isDynamicBlock(std::string_view opcode) {
    return catalog().byOpcode.count(std::string(opcode)) != 0;
}

std::vector<CategoryDef> dynamicExtensionCategories() {
    DynamicCatalog& c = catalog();
    return std::vector<CategoryDef>(c.categories.begin(), c.categories.end());
}

void clearDynamicExtensions() {
    DynamicCatalog& c = catalog();
    c.defs.clear();
    c.categories.clear();
    c.byOpcode.clear();
    c.ids.clear();
}

} // namespace xyo::edit
