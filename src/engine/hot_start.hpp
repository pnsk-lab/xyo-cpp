// Portable, frame-boundary Scratch runtime checkpoint (version 1).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace xyo {
class Runtime;

// Returns false with a descriptive reason instead of writing a partial save.
// JIT/fiber continuations are deliberately unsupported: use --jit=off.
bool saveHotStart(Runtime& runtime, const std::string& path,
                  uint64_t projectFingerprint, std::string* reason = nullptr);
bool loadHotStart(Runtime& runtime, const std::string& path,
                  uint64_t projectFingerprint, std::string* reason = nullptr);

uint64_t hotStartFingerprint(const uint8_t* bytes, size_t length);
} // namespace xyo
