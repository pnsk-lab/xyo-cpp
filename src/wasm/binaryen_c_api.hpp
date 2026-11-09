// Optional in-process Binaryen optimization. The C API implementation
// includes <binaryen-c.h> only when CMake discovers libbinaryen.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xyo::wasm {
bool hasBinaryenCAPI();
bool optimizeWithBinaryenCAPI(const std::vector<uint8_t>& input, int level,
                             std::vector<uint8_t>& output, std::string& error);
} // namespace xyo::wasm
