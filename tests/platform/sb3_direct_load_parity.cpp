// Verify that the direct Scratch SB3 importer preserves the complete
// runtime Block structure, including generated primitive block IDs and
// script order, compared with the legacy intermediate BlockJson importer.
// Generated IDs are normalized by insertion sequence, which is stable
// across independent imports of the same project.
#include "load/sb3.hpp"
#include "engine/runtime.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "core/sstr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

void setImporter(bool direct) {
#ifdef _WIN32
    _putenv_s("XYO_SB3_DIRECT_BUILD", direct ? "1" : "0");
#else
    setenv("XYO_SB3_DIRECT_BUILD", direct ? "1" : "0", 1);
#endif
}

std::string normalizeId(const xyo::Blocks& blocks, const std::string& id) {
    if (id.empty()) return "";
    const auto* block = blocks.getBlock(id);
    return block ? "#" + std::to_string(block->seq) : "external:" + id;
}

bool sameField(const xyo::BlockField& a, const xyo::BlockField& b) {
    if (a.id != b.id) return false;
    const auto& x = a.value;
    const auto& y = b.value;
    if (x.isString() != y.isString() || x.isBool() != y.isBool() ||
        x.isColor() != y.isColor() || x.isUndefined() != y.isUndefined())
        return false;
    if (x.isString()) {
        const std::string left = x.bits == xyo::val::kEmptyStr
            ? "" : xyo::SStr::toUtf8(x.asStrPtr()->view());
        const std::string right = y.bits == xyo::val::kEmptyStr
            ? "" : xyo::SStr::toUtf8(y.asStrPtr()->view());
        return left == right;
    }
    return x.bits == y.bits;
}

bool sameBlock(const xyo::Block& a, const xyo::Block& b,
               const xyo::Blocks& aBlocks, const xyo::Blocks& bBlocks) {
    if (a.seq != b.seq || a.opcode != b.opcode ||
        a.topLevel != b.topLevel || a.shadow != b.shadow ||
        a.x != b.x || a.y != b.y || a.comment != b.comment ||
        normalizeId(aBlocks, a.next) != normalizeId(bBlocks, b.next) ||
        normalizeId(aBlocks, a.parent) != normalizeId(bBlocks, b.parent))
        return false;
    if (a.inputs.items.size() != b.inputs.items.size() ||
        a.fields.size() != b.fields.size() ||
        a.mutation.has_value() != b.mutation.has_value())
        return false;
    for (size_t i = 0; i < a.inputs.items.size(); ++i) {
        const auto& [nameA, inA] = a.inputs.items[i];
        const auto& [nameB, inB] = b.inputs.items[i];
        if (nameA != nameB ||
            normalizeId(aBlocks, inA.block) != normalizeId(bBlocks, inB.block) ||
            normalizeId(aBlocks, inA.shadow) != normalizeId(bBlocks, inB.shadow))
            return false;
    }
    for (const auto& [name, field] : a.fields) {
        auto it = b.fields.find(name);
        if (it == b.fields.end() || !sameField(field, it->second))
            return false;
    }
    if (a.mutation &&
        (a.mutation->tagName != b.mutation->tagName ||
         a.mutation->attrs != b.mutation->attrs))
        return false;
    return true;
}

std::vector<const xyo::Block*> inSequence(xyo::Blocks& blocks) {
    std::vector<const xyo::Block*> ordered(blocks.size(), nullptr);
    for (const auto& [id, block] : blocks.allBlocks()) {
        if (block.seq == 0 || block.seq > ordered.size() ||
            ordered[block.seq - 1] != nullptr)
            return {};
        ordered[block.seq - 1] = &block;
    }
    return ordered;
}

bool compare(xyo::Blocks& a, xyo::Blocks& b) {
    if (a.size() != b.size()) return false;
    auto left = inSequence(a), right = inSequence(b);
    if (left.size() != right.size() || left.size() != a.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (!left[i] || !right[i] ||
            !sameBlock(*left[i], *right[i], a, b)) {
            std::cerr << "Block mismatch at insertion sequence " << i + 1 << '\n';
            return false;
        }
    }
    const auto& aScripts = a.getScripts();
    const auto& bScripts = b.getScripts();
    if (aScripts.size() != bScripts.size()) return false;
    for (size_t i = 0; i < aScripts.size(); ++i)
        if (normalizeId(a, aScripts[i]) != normalizeId(b, bScripts[i]))
            return false;
    return true;
}

bool verifyFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto legacy = std::make_unique<xyo::Runtime>();
    setImporter(false);
    const auto oldLoad = xyo::loadSb3File(*legacy, bytes.data(), bytes.size());
    if (!oldLoad.ok) {
        std::cerr << filename << ": old importer error: " << oldLoad.error << '\n';
        return false;
    }
    auto direct = std::make_unique<xyo::Runtime>();
    setImporter(true);
    const auto newLoad = xyo::loadSb3File(*direct, bytes.data(), bytes.size());
    if (!newLoad.ok) {
        std::cerr << filename << ": direct importer error: " << newLoad.error << '\n';
        return false;
    }
    if (oldLoad.extensionIds != newLoad.extensionIds ||
        legacy->ownedSprites.size() != direct->ownedSprites.size())
        return false;
    size_t blocks = 0;
    for (size_t i = 0; i < legacy->ownedSprites.size(); ++i) {
        auto& a = *legacy->ownedSprites[i]->blocks;
        auto& b = *direct->ownedSprites[i]->blocks;
        if (!compare(a, b)) {
            std::cerr << filename << ": target " << i << " differs\n";
            return false;
        }
        blocks += a.size();
    }
    std::cout << "PASS " << filename << ": " << blocks
              << " blocks; direct and legacy structural equality\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: xyo-sb3-direct-load-parity <project.sb3>...\n";
        return 2;
    }
    for (int i = 1; i < argc; ++i)
        if (!verifyFile(argv[i])) return 1;
}
