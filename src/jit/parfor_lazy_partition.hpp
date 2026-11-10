#pragma once

// Internal LLVM ORC partition policy. Public declarations only so the
// safety proof can be exercised against synthetic LLVM IR in unit tests.
#include <llvm/ExecutionEngine/Orc/IRPartitionLayer.h>

namespace llvm {
class Module;
}

namespace xyo {

// Include address-taken par-for callbacks in the requested caller partition.
// A nullopt requests conservative whole-module compilation.
std::optional<llvm::orc::IRPartitionLayer::GlobalValueSet>
partitionParForCallbacks(llvm::orc::IRPartitionLayer::GlobalValueSet requested);

// Allow lazy partitioning only when all par-for callback pointers are
// direct, private, module-local, and have no additional consumers.
bool supportsLazyParForPartition(const llvm::Module& module);

} // namespace xyo
