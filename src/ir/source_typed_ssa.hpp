// IR-001: detached typed SSA diagnostic representation.
// Shares RT-001 descriptor encoding but does NOT authorize execution or
// imply that any raw i64 is a safe GC reference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class SourceSsaKind : uint32_t {
    IntegerInstruction = 0,
    FloatingInstruction = 0x100,
    IntegerArgument = 0x200,
    FloatingArgument = 0x300,
};

struct SourceTypedSsaManifest {
    uint64_t revision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t procedureId = 0;
    uint64_t resumePc = 0;
    uint64_t siteId = 0;
    std::vector<uint64_t> descriptors;

    static bool validDescriptor(uint64_t descriptor) {
        const auto taggedWidth = uint32_t(descriptor >> 32);
        const auto kind = taggedWidth & ~uint32_t{0xff};
        const auto width = taggedWidth & 0xff;
        switch (kind) {
        case uint32_t(SourceSsaKind::IntegerInstruction):
        case uint32_t(SourceSsaKind::IntegerArgument):
            return width == 1 || width == 32 || width == 64;
        case uint32_t(SourceSsaKind::FloatingInstruction):
        case uint32_t(SourceSsaKind::FloatingArgument):
            return width == 32 || width == 64;
        default:
            return false;
        }
    }

    bool valid() const {
        if (!revision || (!digestLo && !digestHi) || !resumePc || !siteId ||
            descriptors.empty() || descriptors.size() > 256)
            return false;
        for (size_t i = 0; i < descriptors.size(); ++i) {
            if (!validDescriptor(descriptors[i])) return false;
            if (i && uint32_t(descriptors[i]) <=
                         uint32_t(descriptors[i - 1]))
                return false;
        }
        return true;
    }
};

// Owns copied bits, never pointers into native stack or compiler globals.
// Not an authenticated compiler manifest, nor a GC-rooted Value container.
struct SourceTypedSsaSnapshot {
    SourceTypedSsaManifest manifest;
    std::vector<uint64_t> words;

    static std::optional<SourceTypedSsaSnapshot> capture(
        const SourceTypedSsaManifest& manifest,
        std::span<const uint64_t> words) {
        if (!manifest.valid() || manifest.descriptors.size() != words.size())
            return std::nullopt;
        SourceTypedSsaSnapshot result{manifest,
                                     {words.begin(), words.end()}};
        return result.valid() ?
            std::optional<SourceTypedSsaSnapshot>(std::move(result)) :
            std::nullopt;
    }

    bool valid() const {
        if (!manifest.valid() || manifest.descriptors.size() != words.size())
            return false;
        for (size_t i = 0; i < words.size(); ++i) {
            const auto width = uint32_t(manifest.descriptors[i] >> 32) & 0xff;
            if (width == 1 && words[i] > 1) return false;
            if (width == 32 && words[i] > UINT32_MAX) return false;
        }
        return true;
    }

    bool matches(const SourceTypedSsaManifest& expected) const {
        return valid() && expected.valid() &&
            manifest.revision == expected.revision &&
            manifest.digestLo == expected.digestLo &&
            manifest.digestHi == expected.digestHi &&
            manifest.procedureId == expected.procedureId &&
            manifest.resumePc == expected.resumePc &&
            manifest.siteId == expected.siteId &&
            manifest.descriptors == expected.descriptors;
    }
};

} // namespace xyo::ir
