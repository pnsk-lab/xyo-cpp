// Binaryen C API adapter. On Linux, dlopen with local/deep symbol binding
// prevents libbinaryen's bundled LLVM symbols from interposing on xyo's LLVM.
#include "binaryen_c_api.hpp"

#if defined(XYO_HAVE_BINARYEN_C)
#include <binaryen-c.h>
#endif

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>

#if defined(XYO_BINARYEN_DLOPEN)
#include <dlfcn.h>
#endif

namespace xyo::wasm {

#if defined(XYO_HAVE_BINARYEN_C)
namespace {

#if defined(XYO_BINARYEN_DLOPEN)
struct BinaryenApi {
    void* library = nullptr;
    decltype(&BinaryenModuleRead) moduleRead = nullptr;
    decltype(&BinaryenModuleDispose) moduleDispose = nullptr;
    decltype(&BinaryenModuleValidate) moduleValidate = nullptr;
    decltype(&BinaryenGetOptimizeLevel) getOptimizeLevel = nullptr;
    decltype(&BinaryenGetShrinkLevel) getShrinkLevel = nullptr;
    decltype(&BinaryenSetOptimizeLevel) setOptimizeLevel = nullptr;
    decltype(&BinaryenSetShrinkLevel) setShrinkLevel = nullptr;
    decltype(&BinaryenModuleOptimize) moduleOptimize = nullptr;
    decltype(&BinaryenModuleAllocateAndWrite) moduleAllocateAndWrite = nullptr;

    BinaryenApi() {
        // Binaryen's Arch package exports its own LLVM symbols; linking it
        // into the main executable can overwrite ORC JIT's LLVM ABI. Keep
        // libbinaryen out of DT_NEEDED and favor its own dependencies here.
        library = ::dlopen("libbinaryen.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (!library) return;
        const auto symbol = [this](auto& entry, const char* name) {
            entry = reinterpret_cast<std::remove_reference_t<decltype(entry)>>(
                ::dlsym(library, name));
            return entry != nullptr;
        };
        const bool ok =
            symbol(moduleRead, "BinaryenModuleRead") &&
            symbol(moduleDispose, "BinaryenModuleDispose") &&
            symbol(moduleValidate, "BinaryenModuleValidate") &&
            symbol(getOptimizeLevel, "BinaryenGetOptimizeLevel") &&
            symbol(getShrinkLevel, "BinaryenGetShrinkLevel") &&
            symbol(setOptimizeLevel, "BinaryenSetOptimizeLevel") &&
            symbol(setShrinkLevel, "BinaryenSetShrinkLevel") &&
            symbol(moduleOptimize, "BinaryenModuleOptimize") &&
            symbol(moduleAllocateAndWrite, "BinaryenModuleAllocateAndWrite");
        if (!ok) {
            ::dlclose(library);
            library = nullptr;
        }
    }
    // Deliberately keep the shared library mapped for process lifetime.
    bool available() const { return library != nullptr; }
};
#else
struct BinaryenApi {
    // Direct linking remains safe on targets without Linux's ELF preemption.
    decltype(&BinaryenModuleRead) moduleRead = &BinaryenModuleRead;
    decltype(&BinaryenModuleDispose) moduleDispose = &BinaryenModuleDispose;
    decltype(&BinaryenModuleValidate) moduleValidate = &BinaryenModuleValidate;
    decltype(&BinaryenGetOptimizeLevel) getOptimizeLevel = &BinaryenGetOptimizeLevel;
    decltype(&BinaryenGetShrinkLevel) getShrinkLevel = &BinaryenGetShrinkLevel;
    decltype(&BinaryenSetOptimizeLevel) setOptimizeLevel = &BinaryenSetOptimizeLevel;
    decltype(&BinaryenSetShrinkLevel) setShrinkLevel = &BinaryenSetShrinkLevel;
    decltype(&BinaryenModuleOptimize) moduleOptimize = &BinaryenModuleOptimize;
    decltype(&BinaryenModuleAllocateAndWrite) moduleAllocateAndWrite =
        &BinaryenModuleAllocateAndWrite;
    bool available() const { return true; }
};
#endif

const BinaryenApi& api() {
    static const BinaryenApi instance;
    return instance;
}
} // namespace
#endif

bool hasBinaryenCAPI() {
#if defined(XYO_HAVE_BINARYEN_C)
    return api().available();
#else
    return false;
#endif
}

bool optimizeWithBinaryenCAPI(const std::vector<uint8_t>& input, int level,
                             std::vector<uint8_t>& output, std::string& error) {
#if !defined(XYO_HAVE_BINARYEN_C)
    (void)input;
    (void)level;
    (void)output;
    error = "in-process Binaryen unavailable: install the Binaryen development "
            "library and reconfigure CMake (XYO_BINARYEN_C_API=ON)";
    return false;
#else
    if (!hasBinaryenCAPI()) {
        error = "Binaryen C API could not be loaded (libbinaryen.so or symbols missing)";
        return false;
    }
    // Binaryen optimization flags are process-global. Serialize all calls
    // and restore settings before another compilation can begin.
    static std::mutex binaryenMutex;
    const std::lock_guard lock(binaryenMutex);
    const BinaryenApi& b = api();
    BinaryenModuleRef module = b.moduleRead(
        reinterpret_cast<char*>(const_cast<uint8_t*>(input.data())),
        input.size());
    if (!module) {
        error = "BinaryenModuleRead failed";
        return false;
    }
    const std::unique_ptr<std::remove_pointer_t<BinaryenModuleRef>,
                          decltype(b.moduleDispose)>
        owned(module, b.moduleDispose);

    if (!b.moduleValidate(module)) {
        error = "Binaryen rejected the input WebAssembly module";
        return false;
    }

    const int oldOptimize = b.getOptimizeLevel();
    const int oldShrink = b.getShrinkLevel();
    b.setOptimizeLevel(level);
    b.setShrinkLevel(0);
    b.moduleOptimize(module);
    b.setOptimizeLevel(oldOptimize);
    b.setShrinkLevel(oldShrink);

    if (!b.moduleValidate(module)) {
        error = "Binaryen generated an invalid module";
        return false;
    }
    const auto serialized = b.moduleAllocateAndWrite(module, nullptr);
    const std::unique_ptr<void, decltype(&std::free)>
        binary(serialized.binary, &std::free);
    const std::unique_ptr<void, decltype(&std::free)>
        sourceMap(serialized.sourceMap, &std::free);
    if (!binary || serialized.binaryBytes < 8 ||
        serialized.binaryBytes > 32u * 1024u * 1024u) {
        error = "Binaryen emitted an empty or oversized module";
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(binary.get());
    if (std::memcmp(data, "\0asm", 4) != 0) {
        error = "Binaryen output has no WebAssembly magic";
        return false;
    }
    output.assign(data, data + serialized.binaryBytes);
    return true;
#endif
}
} // namespace xyo::wasm
