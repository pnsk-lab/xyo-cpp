// IR-001: pointer-free, compiler-described live SSA diagnostic snapshot.
// This is NOT an executable deoptimization certificate. The LLVM codegen
// must eventually emit exact live-value capture at each verified boundary.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace xyo::ir {

struct SourceSsaSlot {
    uint32_t id = 0;       // compiler-assigned stable SSA ID
    uint8_t width = 0;     // 1, 32 or 64 raw bits; never a GC pointer
    bool operator==(const SourceSsaSlot&) const = default;
};

// The compiler constructs one manifest for each exact source boundary.
// Empty slots are meaningful: the compiler explicitly proved no live SSA.
struct SourceSsaManifest {
    uint64_t revision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t procedureId = 0;
    uint64_t resumePc = 0;
    std::vector<SourceSsaSlot> slots;

    bool valid() const {
        if (!revision || !digestLo || !digestHi || !resumePc ||
            slots.size() > 4096) return false;
        uint32_t previous = 0;
        bool first = true;
        for (const auto& slot : slots) {
            if (slot.width != 1 && slot.width != 32 && slot.width != 64)
                return false;
            if (!first && slot.id <= previous) return false;
            previous = slot.id;
            first = false;
        }
        return true;
    }
};

// Independently owned raw words. Never interpret tagged Scratch Value or
// heap pointers as numeric SSA without a separate GC-rooted Value contract.
struct SourceSsaSnapshot {
    uint64_t revision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t procedureId = 0;
    uint64_t resumePc = 0;
    std::vector<SourceSsaSlot> slots;
    std::vector<uint64_t> values;

    static std::optional<SourceSsaSnapshot> capture(
        const SourceSsaManifest& manifest, std::span<const uint64_t> words) {
        if (!manifest.valid() || words.size() != manifest.slots.size())
            return std::nullopt;
        SourceSsaSnapshot result{
            manifest.revision, manifest.digestLo, manifest.digestHi,
            manifest.procedureId, manifest.resumePc, manifest.slots,
            {words.begin(), words.end()}};
        return result.matches(manifest) ?
            std::optional<SourceSsaSnapshot>(std::move(result)) : std::nullopt;
    }

    bool matches(const SourceSsaManifest& manifest) const {
        if (!manifest.valid() || revision != manifest.revision ||
            digestLo != manifest.digestLo || digestHi != manifest.digestHi ||
            procedureId != manifest.procedureId ||
            resumePc != manifest.resumePc || slots != manifest.slots ||
            values.size() != slots.size()) return false;
        for (size_t i = 0; i < slots.size(); ++i) {
            const uint8_t width = slots[i].width;
            if (width < 64 && (values[i] >> width) != 0) return false;
        }
        return true;
    }
};

} // namespace xyo::ir
