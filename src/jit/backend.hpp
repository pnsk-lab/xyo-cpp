// JIT backend contract. No LLVM headers or LLVM types are exposed here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xyo {
class Thread;
class Runtime;
struct Block;
struct CompiledScript;
namespace ir { struct ScratchNumericKernel; struct ScratchCompilationUnit; }

// Callable shared-CFG numeric kernel. The backend owns executable code for
// the JitEngine lifetime; a thread owns only its independent continuation.
struct NumericJitCode {
    std::shared_ptr<const ir::ScratchNumericKernel> kernel;
    std::function<uint64_t(uint64_t*, uint64_t*)> run;
};

// Prepared native code is independent of mutable Scratch state. Its
// finish() method may run on a compilation worker, while the interpreter
// continues on the main thread.
class PendingJitCompilation {
public:
    virtual ~PendingJitCompilation() = default;
    virtual std::unique_ptr<CompiledScript> finish() = 0;
};

class JitBackend {
public:
    virtual ~JitBackend() = default;
    virtual bool ok() const = 0;
    // A null result is a compilation failure: JitEngine falls back to the
    // interpreter and remembers unsupported scripts.
    virtual std::unique_ptr<CompiledScript> compile(
        Thread&, Runtime&, const Block* top, uint64_t serial,
        const ir::ScratchCompilationUnit& source) = 0;
    // The backend may emit an immutable IR snapshot on the owning thread.
    // The returned task must not access Thread, Runtime, Blocks, or GC while
    // running finish() in the worker. Unsupported backends return nullptr.
    virtual std::unique_ptr<PendingJitCompilation> prepareAsync(
        Thread&, Runtime&, const Block*, uint64_t) { return {}; }

    // Optional synchronous, opt-in shared-CFG compilation. Ordinary JIT
    // compilation remains available when this returns nullptr.
    virtual std::shared_ptr<NumericJitCode> compileNumeric(
        std::shared_ptr<const ir::ScratchNumericKernel>,
        uint64_t) { return {}; }

    // Optional native-object support for the pre-existing AOT format.
    // WebAssembly backends may reject native objects without affecting JIT.
    virtual bool addObjectFile(const void* bytes, size_t size) = 0;
    virtual void* lookupSymbol(const std::string& name) = 0;
};

std::unique_ptr<JitBackend> createJitBackend();
} // namespace xyo
