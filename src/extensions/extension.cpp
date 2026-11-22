// xyo-cpp: 自作拡張レジストリ（Phase 0）。
#include "extension.hpp"

#include <utility>

#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"

namespace xyo {

Value extensionPrimitive(const ArgValues& args, BlockUtility& util) {
    auto* ext = static_cast<ExtensionInstance*>(util.primitiveContext);
    if (!ext || !util.op) return Value::undefined();
    return ext->call(util.op->opcode, args, util);
}

bool ExtensionRegistry::add(Runtime& rt, std::unique_ptr<ExtensionInstance> ext,
                            std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!ext) return fail("extension is null");

    const ExtensionDef def = ext->def();
    if (def.id.empty()) return fail("extension id is empty");
    for (char c : def.id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) return fail("invalid extension id: " + def.id);
    }
    if (byId_.count(def.id))
        return fail("duplicate extension id: " + def.id);
    if (def.blocks.empty())
        return fail("extension has no blocks: " + def.id);

    const std::string prefix = def.id + "_";
    for (const ExtensionBlockDef& block : def.blocks) {
        if (block.opcode.rfind(prefix, 0) != 0)
            return fail("opcode does not start with extension id: " +
                        block.opcode);
        if (byOpcode_.count(block.opcode))
            return fail("duplicate opcode: " + block.opcode);
        if (rt.getOpcodeFunction(block.opcode))
            return fail("opcode already registered: " + block.opcode);
    }

    ExtensionInstance* raw = ext.get();
    for (const ExtensionBlockDef& block : def.blocks) {
        rt.registerPrimitive(block.opcode, &extensionPrimitive, raw);
        if (block.hat) {
            HatMeta meta;
            meta.edgeActivated = block.edgeActivated;
            meta.restartExistingThreads = block.restartExistingThreads;
            rt.registerHat(block.opcode, meta);
        }
        if (block.conditional || block.loop) {
            FlowingFlags flags;
            flags.conditional = block.conditional;
            flags.loop = block.loop;
            rt.registerFlowing(block.opcode, flags);
        }
        byOpcode_[block.opcode] = raw;
    }

    byId_[def.id] = raw;
    definitions_.push_back(def);
    instances_.push_back(std::move(ext));
    return true;
}

ExtensionInstance* ExtensionRegistry::find(const std::string& id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

PrimFn ExtensionRegistry::primitiveFor(const std::string& opcode) const {
    return byOpcode_.count(opcode) ? &extensionPrimitive : nullptr;
}

void* ExtensionRegistry::contextFor(const std::string& opcode) const {
    auto it = byOpcode_.find(opcode);
    return it == byOpcode_.end() ? nullptr : it->second;
}

} // namespace xyo
