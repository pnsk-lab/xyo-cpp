// xyo-cpp: AOT コンパイラ本体（emit / meta / 自己実行ファイル）。
#include "aot.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX          // std::min/std::max をマクロで壊させない
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../core/fileio.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../jit/aot_emit.hpp"
#include "../jit/jit.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"

namespace xyo {

/// codegen.cpp（JIT と共通の emit）。
bool jitEmitScript(llvm::Module&, llvm::LLVMContext&, Runtime&, Target*,
                   Blocks*, const std::string&, const std::string&,
                   CompiledScript&, JitPinned&, std::vector<std::string>&,
                   std::map<std::string, std::string>&, AotEmit*);

namespace aot {
namespace {

constexpr uint64_t kFooterMagic = 0x3130544f41594f58ull; // "XYOAOT01"
constexpr size_t kFooterSize = sizeof(uint64_t) * 4;

struct Footer {
    uint64_t magic;
    uint64_t sb3Len;
    uint64_t objLen;
    uint64_t hash;
};

struct ScriptMeta {
    std::string top;
    std::string entry;
    int ti = 0;   // Runtime::targets 順のターゲット index
    bool mayYield = true;
    std::map<std::string, std::string> procs;
};

void appendEscaped(std::string& s, std::string_view v) {
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : v) {
        switch (c) {
        case '"': s += "\\\""; break;
        case '\\': s += "\\\\"; break;
        case '\n': s += "\\n"; break;
        case '\r': s += "\\r"; break;
        case '\t': s += "\\t"; break;
        default:
            if (c < 0x20) {
                s += "\\u00";
                s.push_back(hex[c >> 4]);
                s.push_back(hex[c & 15]);
            } else {
                s.push_back(char(c));
            }
        }
    }
}

std::string hexOf(const std::string& raw) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

std::string buildMeta(uint64_t hash, const std::vector<ScriptMeta>& scripts,
                      const AotEmit& emit) {
    std::string s;
    s.reserve(256 + scripts.size() * 128 + emit.slots.size() * 48);
    s += "{\"v\":1,\"hash\":\"";
    char hb[32];
    std::snprintf(hb, sizeof(hb), "%016llx",
                  (unsigned long long)hash);
    s += hb;
    s += "\",\"scripts\":[";
    bool first = true;
    for (const ScriptMeta& m : scripts) {
        if (!first) s += ',';
        first = false;
        s += "{\"top\":\"";
        appendEscaped(s, m.top);
        s += "\",\"ti\":";
        s += std::to_string(m.ti);
        s += ",\"entry\":\"";
        appendEscaped(s, m.entry);
        s += "\",\"mayYield\":";
        s += m.mayYield ? "true" : "false";
        s += ",\"procs\":[";
        bool fp = true;
        for (const auto& [vk, sym] : m.procs) {
            if (!fp) s += ',';
            fp = false;
            s += "[\"";
            appendEscaped(s, vk);
            s += "\",\"";
            appendEscaped(s, sym);
            s += "\"]";
        }
        s += "]}";
    }
    s += "],\"slots\":[";
    bool fs = true;
    for (const AotSlot& sl : emit.slots) {
        if (!fs) s += ',';
        fs = false;
        s += "{\"k\":";
        s += std::to_string(static_cast<int>(sl.kind));
        s += ",\"g\":\"";
        appendEscaped(s, sl.global);
        s += '"';
        if (sl.kind == AotSlotKind::StrLit) {
            s += ",\"h\":\"";
            s += hexOf(sl.key);
            s += '"';
        } else {
            s += ",\"s\":\"";
            appendEscaped(s, sl.key);
            s += '"';
        }
        s += '}';
    }
    s += "]}";
    return s;
}

/// UTF-16 生バイト列 → std::u16string（アラインメント非依存）。
std::u16string u16FromBytes(const std::string& raw) {
    std::u16string u;
    u.resize(raw.size() / 2);
    if (!u.empty()) std::memcpy(u.data(), raw.data(), u.size() * 2);
    return u;
}

} // namespace

uint64_t hashBytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool compileProject(Runtime& rt, uint64_t projectHash, BuildResult& out,
                    std::string* why) {
    // GPU 並列ループは emit 時にホスト側ヒープの Program を指す定数を
    // 埋め込む（xyo_gpu_repeat へポインタを渡す）。別プロセスでは無効
    // なので AOT モードでは codegen 側が CPU ループへフォールバック
    // させる（jitEmitScript の aot 引数、env は変更しない）。
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    llvm::Triple triple(llvm::sys::getProcessTriple());
    std::string err;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        if (why) *why = "no target: " + err;
        return false;
    }
    llvm::TargetOptions opt;
    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, llvm::sys::getHostCPUName().str(),
                                    "", opt, llvm::Reloc::PIC_));
    if (!tm) {
        if (why) *why = "TargetMachine creation failed";
        return false;
    }

    llvm::LLVMContext lc;
    auto mod = std::make_unique<llvm::Module>("xyo_aot", lc);
    mod->setDataLayout(tm->createDataLayout());
    mod->setTargetTriple(triple);

    AotEmit emit;
    std::vector<ScriptMeta> scripts;
    int n = 0;
    int ti = -1;
    for (Target* t : rt.targets) {
        ++ti;
        if (!t || !t->blocks || !t->isOriginal) continue;
        emit.targetIndex = uint32_t(ti);
        for (const std::string& sid : t->blocks->getScripts()) {
            const std::string entry =
                "xyo_aot_script_" + std::to_string(n);
            CompiledScript cs;
            JitPinned pinned;
            std::vector<std::string> fns;
            std::map<std::string, std::string> procs;
            if (!jitEmitScript(*mod, lc, rt, t, t->blocks, sid, entry, cs,
                               pinned, fns, procs, &emit))
                continue;
            ScriptMeta m;
            m.top = sid;
            m.entry = entry;
            m.ti = ti;
            m.mayYield = cs.mayYield;
            m.procs = std::move(procs);
            scripts.push_back(std::move(m));
            ++n;
        }
    }
    if (scripts.empty()) {
        if (why) *why = "no compilable scripts";
        return false;
    }

    const std::string meta = buildMeta(projectHash, scripts, emit);
    llvm::Constant* metaC =
        llvm::ConstantDataArray::getString(lc, meta, true);
    new llvm::GlobalVariable(*mod, metaC->getType(), true,
                             llvm::GlobalValue::ExternalLinkage, metaC,
                             "xyo_aot_meta");

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        if (why) *why = "module verification failed";
        return false;
    }

    {
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2)
            .run(*mod, mam);
    }

    llvm::SmallVector<char, 0> buf;
    llvm::raw_svector_ostream os(buf);
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        if (why) *why = "object emission unsupported";
        return false;
    }
    pm.run(*mod);

    out.object.assign(buf.data(), buf.size());
    out.meta = meta;
    out.scripts = n;
    out.slots = static_cast<int>(emit.slots.size());
    return true;
}

namespace {

/// 実行中バイナリを outPath へコピーする。
bool copySelf(const std::string& outPath, std::string* why) {
#ifdef _WIN32
    const std::string self = selfExePath();
    if (self.empty()) {
        if (why) *why = "cannot locate the running executable";
        return false;
    }
    const std::wstring src = pathFromUtf8(self).wstring();
    const std::wstring dst = pathFromUtf8(outPath).wstring();
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        if (why) *why = "CopyFile failed";
        return false;
    }
    return true;
#elif defined(__linux__)
    const std::string self = selfExePath();
    std::FILE* in = fopenUtf8(self.c_str(), "rb");
    if (!in) {
        if (why) *why = "cannot open the running executable";
        return false;
    }
    std::FILE* out = fopenUtf8(outPath.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        if (why) *why = "cannot create output";
        return false;
    }
    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    for (;;) {
        const size_t r = std::fread(buf.data(), 1, buf.size(), in);
        if (r) {
            if (std::fwrite(buf.data(), 1, r, out) != r) { ok = false; break; }
        }
        if (r < buf.size()) {
            if (std::ferror(in)) ok = false;
            break;
        }
    }
    if (std::fclose(in) != 0) ok = false;
    if (std::fclose(out) != 0) ok = false;
    if (!ok && why) *why = "copy failed";
    return ok;
#else
    if (why) *why = "self-executable output is unsupported on this platform";
    return false;
#endif
}

/// 出力先が実行中のバイナリ自身か（上書きすると壊れる）。
bool isSelf(const std::string& outPath) {
    const std::string self = selfExePath();
    if (self.empty()) return false;
#ifdef _WIN32
    const std::wstring sw = pathFromUtf8(self).wstring();
    const std::wstring ow = pathFromUtf8(outPath).wstring();
    wchar_t a[MAX_PATH * 4], b[MAX_PATH * 4];
    if (GetFullPathNameW(sw.c_str(), MAX_PATH * 4, a, nullptr) == 0)
        return false;
    if (GetFullPathNameW(ow.c_str(), MAX_PATH * 4, b, nullptr) == 0)
        return false;
    return _wcsicmp(a, b) == 0;
#else
    struct stat a {}, b {};
    return ::stat(self.c_str(), &a) == 0 &&
           ::stat(outPath.c_str(), &b) == 0 && a.st_dev == b.st_dev &&
           a.st_ino == b.st_ino;
#endif
}

bool appendPayload(const std::string& path, const void* sb3, size_t sb3Len,
                   const BuildResult& build, uint64_t projectHash,
                   std::string* why) {
    std::FILE* f = fopenUtf8(path.c_str(), "ab");
    if (!f) {
        if (why) *why = "cannot open output for append";
        return false;
    }
    const Footer ftr{kFooterMagic, sb3Len, build.object.size(),
                     projectHash};
    bool ok = std::fwrite(sb3, 1, sb3Len, f) == sb3Len &&
              std::fwrite(build.object.data(), 1, build.object.size(), f) ==
                  build.object.size() &&
              std::fwrite(&ftr, 1, sizeof(ftr), f) == sizeof(ftr);
    if (std::fclose(f) != 0) ok = false;
    if (!ok && why) *why = "append failed";
    return ok;
}

} // namespace

bool writeObjectFile(const std::string& path, const BuildResult& build,
                     std::string* why) {
    std::FILE* f = fopenUtf8(path.c_str(), "wb");
    if (!f) {
        if (why) *why = "cannot create object file";
        return false;
    }
    const bool ok = std::fwrite(build.object.data(), 1,
                                build.object.size(), f) ==
                    build.object.size();
    if (std::fclose(f) != 0 || !ok) {
        if (why) *why = "object write failed";
        return false;
    }
    return true;
}

bool writeExecutable(const std::string& outPath, const void* sb3,
                     size_t sb3Len, const BuildResult& build,
                     uint64_t projectHash, std::string* why) {
    if (isSelf(outPath)) {
        if (why) *why = "output would overwrite the running binary";
        return false;
    }
    if (!copySelf(outPath, why)) return false;
    if (!appendPayload(outPath, sb3, sb3Len, build, projectHash, why))
        return false;
#ifndef _WIN32
    ::chmod(outPath.c_str(), 0755);
#endif
    return true;
}

bool readSelf(std::vector<uint8_t>& sb3, std::vector<uint8_t>& object,
              uint64_t* projectHash) {
    const std::string self = selfExePath();
    if (self.empty()) return false;
    std::FILE* f = fopenUtf8(self.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        if (std::fseek(f, 0, SEEK_END) != 0) break;
        const long size = std::ftell(f);
        if (size <= long(sizeof(Footer))) break;
        Footer ftr{};
        if (std::fseek(f, size - long(sizeof(Footer)), SEEK_SET) != 0) break;
        if (std::fread(&ftr, 1, sizeof(ftr), f) != sizeof(ftr)) break;
        if (ftr.magic != kFooterMagic) break;
        const uint64_t payload = ftr.sb3Len + ftr.objLen;
        if (payload > uint64_t(size) - sizeof(Footer)) break;
        if (std::fseek(f,
                       long(uint64_t(size) - sizeof(Footer) - payload),
                       SEEK_SET) != 0)
            break;
        sb3.assign(size_t(ftr.sb3Len), 0);
        object.assign(size_t(ftr.objLen), 0);
        if (!sb3.empty() &&
            std::fread(sb3.data(), 1, sb3.size(), f) != sb3.size())
            break;
        if (!object.empty() &&
            std::fread(object.data(), 1, object.size(), f) != object.size())
            break;
        if (projectHash) *projectHash = ftr.hash;
        ok = true;
    } while (false);
    std::fclose(f);
    if (!ok) {
        sb3.clear();
        object.clear();
    }
    return ok;
}


} // namespace aot
} // namespace xyo
