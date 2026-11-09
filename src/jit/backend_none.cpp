// LLVM-free build configuration. A future Binaryen backend can replace
// this file while preserving the backend-neutral JitEngine interface.
#include "backend.hpp"
#include "jit.hpp"
namespace xyo {
class NullJitBackend final : public JitBackend {
public:
    bool ok() const override { return false; }
    std::unique_ptr<CompiledScript> compile(
        Thread&, Runtime&, const Block*, uint64_t,
        const ir::ScratchCompilationUnit&) override { return nullptr; }
    bool addObjectFile(const void*, size_t) override { return false; }
    void* lookupSymbol(const std::string&) override { return nullptr; }
};
std::unique_ptr<JitBackend> createJitBackend() {
    return std::make_unique<NullJitBackend>();
}
} // namespace xyo
