#pragma once

// Internal LLVM ORC partition policy. Public declarations only so the
// safety proof can be exercised against synthetic LLVM IR in unit tests.
#include <llvm/Config/llvm-config.h>
#include <optional>

// LLVM 18 exposes its on-demand partition type on CompileOnDemandLayer;
// LLVM 20+ split the IR partitioning policy into IRPartitionLayer.
// Keep the opt-in policy's function signature identical to the LLVM version
// that actually builds LLLazyJIT, including the static LLVM 18 CI build.
#if LLVM_VERSION_MAJOR >= 20
#include <llvm/ExecutionEngine/Orc/IRPartitionLayer.h>
#else
#include <llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h>
#endif

namespace llvm {
class Module;
}

namespace xyo {

// Include address-taken par-for callbacks in the requested caller partition.
// A nullopt requests conservative whole-module compilation.
#if LLVM_VERSION_MAJOR >= 20
using ParForPartitionValueSet = llvm::orc::IRPartitionLayer::GlobalValueSet;
#else
using ParForPartitionValueSet = llvm::orc::CompileOnDemandLayer::GlobalValueSet;
#endif

std::optional<ParForPartitionValueSet>
partitionParForCallbacks(ParForPartitionValueSet requested);

// Allow lazy partitioning only when all par-for callback pointers are
// direct, private, module-local, and have no additional consumers.
bool supportsLazyParForPartition(const llvm::Module& module);

} // namespace xyo
