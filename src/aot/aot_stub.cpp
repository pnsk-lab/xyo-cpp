// AOT is LLVM-only until another backend implements its object format.
#include "aot.hpp"
namespace xyo::aot {
bool compileProject(Runtime&, uint64_t, BuildResult&, std::string* why,
                    const ProgressCallback&) {
    if (why) *why = "AOT requires the LLVM backend";
    return false;
}
uint64_t hashBytes(const void* p, size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(p);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
bool writeObjectFile(const std::string&, const BuildResult&, std::string* why) {
    if (why) *why = "AOT requires the LLVM backend";
    return false;
}
bool writeExecutable(const std::string&, const void*, size_t,
                     const BuildResult&, uint64_t, std::string* why) {
    if (why) *why = "AOT requires the LLVM backend";
    return false;
}
bool readSelf(std::vector<uint8_t>&, std::vector<uint8_t>&, uint64_t*) {
    return false;
}
} // namespace xyo::aot
