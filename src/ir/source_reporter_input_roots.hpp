// IR-001: opt-in exact GC roots for SOURCE REPORTER string-valued
// LLVM SSA captured at a verified physical native checkpoint.
// A runtime tag alone never authorizes a root. The compiler-owned
// reporter binding and the real GC allocation registry are BOTH checked.
#pragma once

#include "source_reporter_ssa_location_bridge.hpp"
#include "../core/gc.hpp"
#include "gc_value_handles.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xyo::ir {

template<class Detached>
bool retainSourceReporterInputStrings(
        Detached& saved, const SourceReporterCompiledSite& compiled,
        const SourceReporterSsaLocationAudit& verified,
        const SourceTypedSsaManifest& manifest,
        Gc& gc = Gc::get()) {
    if (!manifest.valid() ||
        compiled.revision != manifest.revision ||
        compiled.digestLo != manifest.digestLo ||
        compiled.digestHi != manifest.digestHi ||
        compiled.physicalSiteId != manifest.siteId ||
        compiled.sourcePc != manifest.resumePc ||
        verified.physicalSiteId != manifest.siteId ||
        verified.sourcePc != manifest.resumePc ||
        verified.reporters.size() != compiled.reporters.size() ||
        saved.words.size() != manifest.descriptors.size() ||
        !saved.reporterInputHandles.empty() || saved.reporterInputRoots)
        return false;
    std::vector<RootedValueHandle> handles(saved.words.size());
    std::shared_ptr<GcValueHandles> roots;
    for (size_t n = 0; n < compiled.reporters.size(); ++n) {
        const auto& reporter = compiled.reporters[n];
        const auto& location = verified.reporters[n];
        if (reporter.reporterPc != location.reporterPc ||
            reporter.descriptor != location.descriptor ||
            reporter.origin != location.origin ||
            reporter.reporterBlock.empty() || reporter.parentBlock.empty() ||
            reporter.inputName.empty() || !reporter.inputOrdinal)
            return false;
        const auto it = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            reporter.descriptor, [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (it == manifest.descriptors.end() ||
            *it != reporter.descriptor)
            return false;
        const size_t i = size_t(it - manifest.descriptors.begin());
        const uint32_t kind = uint32_t(reporter.descriptor >> 32);
        // Root only exact boxed i64 instruction SSA that the compiler
        // attributed to a real Scratch reporter, never foreign pointers
        // or native f64/pointer-bit numerical representations.
        if (kind != 64 ||
            (uint32_t(reporter.descriptor) & 0x80000000u))
            continue;
        const Value boxed{saved.words[i]};
        if (!boxed.isString() || boxed.isEmptyString()) continue;
        // One original source string may also have a trusted generated
        // producer root or procedure argument alias. Retaining a second
        // exact handle is safe but never elevates its interpreter location.
        if (handles[i]) continue;
        const uintptr_t address =
            reinterpret_cast<uintptr_t>(boxed.asStrPtr());
        if (address < sizeof(GcHeader)) return false;
        auto* header = reinterpret_cast<GcHeader*>(
            address - sizeof(GcHeader));
        // Membership BEFORE object dereference: reject forged NaN-boxed
        // string tags, stale objects and interior pointers.
        if (!gc.isObject(header) || header->type != GcType::Str)
            return false;
        if (!roots) roots = std::make_shared<GcValueHandles>(gc);
        const auto handle = roots->retain(boxed);
        if (!handle) return false;
        handles[i] = *handle;
    }
    saved.reporterInputHandles = std::move(handles);
    saved.reporterInputRoots = std::move(roots);
    return true;
}

// No new GC roots are created here. Verification only resolves previously
// registered handles and compares exact bits against the *same* captured
// SSA descriptor, returning false if the detached snapshot was altered.
template<class Detached>
bool verifySourceReporterInputStrings(
        const Detached& saved, const SourceReporterCompiledSite& compiled,
        const SourceTypedSsaManifest& manifest) {
    if (!manifest.valid() || compiled.physicalSiteId != manifest.siteId ||
        compiled.sourcePc != manifest.resumePc ||
        saved.words.size() != manifest.descriptors.size() ||
        saved.reporterInputHandles.size() != saved.words.size() ||
        !saved.compilerManifest ||
        saved.compilerManifest->descriptors != manifest.descriptors)
        return false;
    std::vector<bool> expected(saved.words.size());
    for (const auto& reporter : compiled.reporters) {
        const auto it = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            reporter.descriptor, [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (it == manifest.descriptors.end() ||
            *it != reporter.descriptor)
            return false;
        const size_t i = size_t(it - manifest.descriptors.begin());
        const uint32_t kind = uint32_t(reporter.descriptor >> 32);
        if (kind != 64 ||
            (uint32_t(reporter.descriptor) & 0x80000000u))
            continue;
        const Value boxed{saved.words[i]};
        if (boxed.isString() && !boxed.isEmptyString())
            expected[i] = true;
    }
    size_t count = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto handle = saved.reporterInputHandles[i];
        if (bool(handle) != expected[i]) return false;
        if (!handle) continue;
        if (!saved.reporterInputRoots) return false;
        const auto resolved = saved.reporterInputRoots->resolve(handle);
        if (!resolved || resolved->bits != saved.words[i])
            return false;
        ++count;
    }
    return (count == 0 && !saved.reporterInputRoots) ||
           (saved.reporterInputRoots &&
            count == saved.reporterInputRoots->size());
}

} // namespace xyo::ir
