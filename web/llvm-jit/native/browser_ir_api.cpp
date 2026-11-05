// Experimental standalone prototype, not connected to the product. IR inspection
// only: the syntactic subset, mayYield gate and slot allowlist are not proof that
// this IR is executable or that untrusted SB3 parsing is a security boundary.
#include "browser_ir_api.h"
#include "aot_emit.hpp"
#include "jit.hpp"
#include "llvm_compat.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/sstr.hpp"
#include "core/value.hpp"

namespace xyo {
bool jitEmitScript(llvm::Module&, llvm::LLVMContext&, Runtime&, Target*, Blocks*,
                   const std::string&, const std::string&, CompiledScript&,
                   JitPinned&, std::vector<std::string>&,
                   std::map<std::string, std::string>&, AotEmit*);
}
struct XyoBrowserIrResult { std::string ir, metadata, error; bool ok = false; };

// Hex preserves arbitrary bytes (including embedded NUL), not a C string.
static std::string hexBytes(const std::string& input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(input.size() * 2);
    for (unsigned char c : input) { out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    return out;
}
static std::string quoteJson(const std::string& input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(char(c)); }
        else if (c < 0x20 || c >= 0x80) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        else out.push_back(char(c));
    }
    return out + '"';
}
// This partial diagnostic fingerprint is NOT an ABI-compatibility proof or authentication.
static std::string descriptor(const llvm::Module& module) {
    std::string s = "xyo-browser-ir-abi-v2;llvm=" LLVM_VERSION_STRING ";triple=" + module.getTargetTriple().str() +
        ";layout=" + module.getDataLayoutStr();
    auto add = [&](const char* key, size_t value) { s += ';'; s += key; s += '='; s += std::to_string(value); };
    add("ptr_size", sizeof(void*)); add("ptr_align", alignof(void*));
    add("ctx_size", sizeof(xyo::JitCtx)); add("ctx_align", alignof(xyo::JitCtx));
    add("ctx_thread", offsetof(xyo::JitCtx, thread)); add("ctx_target", offsetof(xyo::JitCtx, target));
    add("ctx_runtime", offsetof(xyo::JitCtx, runtime)); add("ctx_var_slots", offsetof(xyo::JitCtx, varSlotData));
    add("variable_size", sizeof(xyo::Variable)); add("variable_align", alignof(xyo::Variable));
    add("variable_value", offsetof(xyo::Variable, value)); add("variable_list", offsetof(xyo::Variable, list));
    add("value_size", sizeof(xyo::Value)); add("value_align", alignof(xyo::Value));
    add("sstr_size", sizeof(xyo::SStr)); add("sstr_align", alignof(xyo::SStr));
    add("sstr_len", offsetof(xyo::SStr, len)); add("sstr_num_cache", offsetof(xyo::SStr, numCache));
    add("sstr_num_state", offsetof(xyo::SStr, numState));
    // NaN-boxing tag constants: a runtime rebuilt with different tags but an
    // identical struct layout must NOT compare equal (same-layout/different-tag
    // rebuild would otherwise validate while producing invalid boxed bits).
    auto addHex = [&](const char* key, uint64_t value) {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
        s += ';'; s += key; s += '='; s += buf;
    };
    addHex("tag_base", xyo::val::kTagBase); addHex("tag_str", xyo::val::kTagStr);
    addHex("tag_color", xyo::val::kTagColor); addHex("tag_nan", xyo::val::kNan);
    addHex("tag_false", xyo::val::kFalse); addHex("tag_true", xyo::val::kTrue);
    addHex("tag_emptystr", xyo::val::kEmptyStr); addHex("tag_undefined", xyo::val::kUndefined);
    addHex("tag_ptrmask", xyo::val::kPtrMask);
    // Emitted-code contract for the one helper this subset can reference.
    // Both sides record the identical literal; any signature drift mismatches.
    s += ";helper_xyo_varslot_init=void(ptr,i64,ptr,ptr,i32)";
    s += ";helper_xyo_to_num=double(i64)";
    return s;
}
static std::string fnv64(const std::string& input) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : input) { h ^= c; h *= 1099511628211ull; }
    char buf[17]; std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}
static std::string buildMetadata(const llvm::Module& module, const xyo::AotEmit& aot) {
    const auto abi = descriptor(module);
    std::string out = "{\"version\":1,\"targetIndex\":" + std::to_string(aot.targetIndex) +
        ",\"targetTriple\":" + quoteJson(module.getTargetTriple().str()) +
        ",\"dataLayout\":" + quoteJson(module.getDataLayoutStr()) +
        ",\"abiDescriptor\":" + quoteJson(abi) + ",\"abiFingerprintFNV1a64\":" + quoteJson(fnv64(abi)) +
        ",\"slots\":[";
    bool first = true;
    for (const auto& slot : aot.slots) {
        if (!first) out += ','; first = false;
        out += "{\"kind\":" + std::to_string(static_cast<unsigned>(slot.kind)) +
            ",\"global\":" + quoteJson(slot.global) + ",\"keyHex\":" + quoteJson(hexBytes(slot.key)) + "}";
    }
    return out + "]}";
}

namespace {
// Strict fail-closed ZIP preflight. ZipReader::extract resizes its output to
// the untrusted central-directory uncompressedSize before validating it, so a
// patched multi-GiB size would OOM/abort the Wasm runtime. Bound every such
// allocation here, mirroring ZipReader::open/extract acceptance (stored and
// deflate entries only), without allocating. Any deviation rejects.
bool preflightZip(const uint8_t* data, size_t size, std::string& error) {
    auto fail = [&](const char* msg) { error = msg; return false; };
    if (!data || size < 22) return fail("zip too small");
    auto rd16 = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
    };
    auto rd32 = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    // Same EOCD scan window as ZipReader::open.
    size_t scanStart = size > 22 + 0xFFFF ? size - 22 - 0xFFFF : 0;
    size_t pos = size;
    for (size_t i = size - 22 + 1; i-- > scanStart;) {
        if (data[i] == 0x50 && data[i + 1] == 0x4B &&
            data[i + 2] == 0x05 && data[i + 3] == 0x06) {
            pos = i;
            break;
        }
    }
    if (pos == size) return fail("zip EOCD not found");
    uint32_t cdCount = rd16(data + pos + 10);
    uint32_t cdOffset = rd32(data + pos + 16);
    if (cdCount == 0 || cdCount > 4096) return fail("bad zip entry count");
    if (cdOffset > size) return fail("bad zip central offset");
    const uint8_t* p = data + cdOffset;
    const uint8_t* end = data + size;
    uint64_t totalUncompressed = 0;
    bool sawProjectJson = false;
    std::vector<std::string> names;
    names.reserve(cdCount);
    for (uint32_t i = 0; i < cdCount; ++i) {
        size_t remaining = size_t(end - p);
        if (remaining < 46 || rd32(p) != 0x02014B50) return fail("bad zip entry");
        uint32_t method = rd16(p + 10);
        if (method != 0 && method != 8) return fail("unsupported zip method");
        uint32_t compSize = rd32(p + 20);
        uint32_t uncompSize = rd32(p + 24);
        uint32_t nameLen = rd16(p + 28);
        uint32_t extraLen = rd16(p + 30);
        uint32_t commentLen = rd16(p + 32);
        uint32_t localOff = rd32(p + 42);
        if (nameLen == 0 || nameLen > 1024) return fail("bad zip name length");
        size_t tail = 46ull + nameLen + extraLen + commentLen;
        if (tail > remaining) return fail("zip entry overruns input");
        std::string name(reinterpret_cast<const char*>(p + 46), nameLen);
        for (const auto& seen : names) {
            if (seen == name) return fail("duplicate zip entry");
        }
        names.push_back(name);
        if (localOff + 30 < localOff || localOff + 30 > size) return fail("bad zip local offset");
        if (compSize > size) return fail("bad zip compressed size");
        if (uncompSize > 16u * 1024u * 1024u) return fail("zip entry too large");
        totalUncompressed += uncompSize;
        if (totalUncompressed > 64u * 1024u * 1024u) return fail("zip total too large");
        if (method == 0 && compSize != uncompSize) return fail("stored size mismatch");
        if (name == "project.json") sawProjectJson = true;
        p += tail;
    }
    if (!sawProjectJson) return fail("project.json not found");
    return true;
}
} // namespace

extern "C" XyoBrowserIrResult* xyo_browser_ir_compile(const uint8_t* data, size_t size) {
    auto r = std::make_unique<XyoBrowserIrResult>();
    auto fail = [&](const std::string& msg) { r->error = msg; return r.release(); };
    if (!data || !size || size > 16u * 1024u * 1024u) return fail("invalid input size");
    std::string zipError;
    if (!preflightZip(data, size, zipError)) return fail("zip preflight rejected: " + zipError);
    xyo::Runtime rt;
    auto loaded = xyo::loadSb3File(rt, data, size);
    if (!loaded.ok) return fail("load failed: " + loaded.error);
    xyo::Target* target = nullptr;
    std::string top;
    uint32_t ti = 0, selected = 0, scripts = 0;
    for (auto* t : rt.targets) {
        if (t && t->isOriginal && t->blocks) {
            for (const auto& id : t->blocks->getScripts()) {
                ++scripts;
                auto* b = t->blocks->getBlock(id);
                if (b && b->opcode == "event_whenflagclicked") {
                    if (target) return fail("more than one green flag script");
                    target = t; top = id; selected = ti;
                }
            }
        }
        ++ti;
    }
    // Match the real host gate: exactly one script overall, and it is the flag.
    if (scripts != 1 || !target) return fail("expected exactly one green flag script");
    auto* hat = target->blocks->getBlock(top);
    // Deliberately minimal, closed subset: a short straight-line chain of set
    // and change blocks with numeric shadows (no procedures, branches, loops
    // or arbitrary reporters). changevariableby lowers inline for constant
    // deltas (no helper call); mayYield below still rejects anything that can
    // yield. Chains are capped so modules stay small and bounded.
    {
        auto* chain = target->blocks->getBlock(hat->next);
        size_t blocks = 0;
        for (; chain; chain = target->blocks->getBlock(chain->next)) {
            if (++blocks > 8) return fail("script chain too long");
            if (chain->opcode != "data_setvariableto" && chain->opcode != "data_changevariableby")
                return fail("expected only variable set/change blocks");
            if (chain->mutation || chain->inputs.size() != 1 || !chain->fields.count("VARIABLE"))
                return fail("malformed set/change block");
            // Variable ids become C strings (VarId slot) and the display name
            // becomes a NUL-terminated UTF-16 global; embedded NUL would
            // silently truncate, so reject before codegen. StrLit keys keep
            // explicit lengths and may hold NUL.
            const auto& varField = chain->fields.find("VARIABLE")->second;
            if (varField.id.empty() || varField.id.find('\0') != std::string::npos)
                return fail("VARIABLE id must be non-empty without NUL");
            if (varField.value.isString() && varField.value.bits != xyo::val::kEmptyStr) {
                const xyo::SStr* name = varField.value.asStrPtr();
                if (name && name->view().find(u'\0') != std::u16string_view::npos)
                    return fail("VARIABLE name must not contain NUL");
            }
            auto* input = chain->inputs.find("VALUE");
            if (!input) return fail("missing VALUE");
            const std::string& child = input->block.empty() ? input->shadow : input->block;
            auto* literal = target->blocks->getBlock(child);
            if (!literal || !literal->shadow || literal->opcode != "math_number" ||
                !literal->next.empty() || !literal->inputs.empty() || literal->mutation ||
                !literal->fields.count("NUM")) return fail("VALUE must be a numeric shadow");
            if (chain->next.empty()) break;
        }
        if (blocks == 0) return fail("expected a variable set/change chain");
    }

    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();
    llvm::Triple triple("wasm32-unknown-emscripten");
    std::string error;
    const auto* backend = llvm::TargetRegistry::lookupTarget(xyo::llvmc::tripleArg(triple), error);
    if (!backend) return fail("target lookup: " + error);
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> tm(backend->createTargetMachine(
        xyo::llvmc::tripleArg(triple), "generic", "", options, llvm::Reloc::PIC_));
    if (!tm) return fail("cannot create wasm32 TargetMachine");
    llvm::LLVMContext context;
    llvm::Module module("xyo_browser_ir", context);
    module.setTargetTriple(xyo::llvmc::tripleArg(triple));
    module.setDataLayout(tm->createDataLayout());
    xyo::CompiledScript compiled;
    xyo::JitPinned pinned;
    xyo::AotEmit aot;
    aot.targetIndex = selected;
    std::vector<std::string> pending;
    std::map<std::string, std::string> procs;
    if (!xyo::jitEmitScript(module, context, rt, target, target->blocks, top,
                            "xyo_browser_script", compiled, pinned, pending, procs, &aot))
        return fail("jitEmitScript rejected script");
    // no-yield gate and strict slot allowlist; this still does not authorize execution.
    if (compiled.mayYield || !procs.empty()) return fail("yield/procedure rejected");
    for (const auto& slot : aot.slots) {
        if (slot.kind != xyo::AotSlotKind::VarId && slot.kind != xyo::AotSlotKind::StrLit)
            return fail("unexpected AOT slot kind");
        // VarId keys become C strings at bind time; NUL would truncate or be
        // rejected there, so fail here instead of emitting unrunnable IR.
        // StrLit keys carry explicit lengths and may preserve NUL.
        if ((slot.kind == xyo::AotSlotKind::VarId) &&
            slot.key.find('\0') != std::string::npos)
            return fail("VarId slot key must not contain NUL");
    }
    std::string diagnostics;
    llvm::raw_string_ostream errors(diagnostics);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush(); return fail("invalid LLVM module: " + diagnostics);
    }
    llvm::raw_string_ostream stream(r->ir);
    module.print(stream, nullptr);
    stream.flush();
    r->metadata = buildMetadata(module, aot);
    r->ok = true;
    return r.release();
}
extern "C" int xyo_browser_ir_ok(const XyoBrowserIrResult* r) { return r && r->ok; }
extern "C" const char* xyo_browser_ir_text(const XyoBrowserIrResult* r) { return r ? r->ir.c_str() : ""; }
extern "C" const char* xyo_browser_ir_metadata(const XyoBrowserIrResult* r) { return r ? r->metadata.c_str() : ""; }
extern "C" const char* xyo_browser_ir_error(const XyoBrowserIrResult* r) { return r ? r->error.c_str() : "null result"; }
extern "C" void xyo_browser_ir_destroy(XyoBrowserIrResult* r) { delete r; }
