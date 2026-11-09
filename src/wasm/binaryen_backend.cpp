// Binaryen optimization of xyo's LLVM-independent WebAssembly output.
#include "binaryen_backend.hpp"
#include "binaryen_c_api.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#endif

namespace xyo::wasm {
namespace {

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
struct TempFile {
    std::string path;
    int fd = -1;
    ~TempFile() {
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::unlink(path.c_str());
    }
    bool open(std::string& error) {
        const char* dir = std::getenv("TMPDIR");
        if (!dir || !*dir) dir = "/tmp";
        auto create = [&](const std::string& base) {
            const std::string pattern = base + "/xyo-binaryen-XXXXXX";
            std::vector<char> writable(pattern.begin(), pattern.end());
            writable.push_back('\0');
            fd = ::mkstemp(writable.data());
            if (fd >= 0) path = writable.data();
            return fd >= 0;
        };
        if (!create(dir)) {
            const int originalError = errno;
            // Some restricted runners mount /tmp read-only. A writable
            // working directory is a valid fallback only for the default
            // temporary path, not for a caller-specified TMPDIR.
            const bool explicitTmp = std::getenv("TMPDIR") &&
                *std::getenv("TMPDIR");
            if (explicitTmp ||
                (originalError != EROFS && originalError != EACCES) ||
                !create(".")) {
                error = "mkstemp: " + std::string(std::strerror(errno));
                return false;
            }
        }
        return true;
    }
    bool writeAll(const std::vector<uint8_t>& input, std::string& error) {
        size_t at = 0;
        while (at < input.size()) {
            const ssize_t n = ::write(fd, input.data() + at, input.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                error = "temporary WASM write failed";
                return false;
            }
            at += size_t(n);
        }
        return true;
    }
};

bool runBinaryen(const std::string& tool, const std::string& input,
                 const std::string& output, int level, std::string& error) {
    const std::string flag = "-O" + std::to_string(level);
    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        const char* argv[] = {
            tool.c_str(), input.c_str(), flag.c_str(), "-o", output.c_str(), nullptr
        };
        ::execvp(tool.c_str(), const_cast<char* const*>(argv));
        ::_exit(127);
    }
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    int status = 0;
    for (;;) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0) {
            if (errno == EINTR) continue;
            error = "waitpid failed";
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        if (Clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            error = "Binaryen optimization timed out after 30 seconds";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "wasm-opt exited unsuccessfully (exit " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) +
            "). Install Binaryen or set XYO_WASM_OPT to a working wasm-opt path";
        return false;
    }
    return true;
}
#endif

} // namespace

HotWasmBackendSelection selectHotWasmBackend(
    const char* requested, bool hasCAPI, bool hasHostOptimizer) {
    HotWasmBackendSelection selection;
    const std::string mode = requested && *requested ? requested : "auto";
    if (mode != "auto" && mode != "builtin" && mode != "binaryen") {
        selection.error = "XYO_WASM_HOT_EMITTER must be auto|builtin|binaryen";
        return selection;
    }
    if (mode == "builtin") return selection;
    if (hasCAPI) {
        selection.emitter = WasmBackend::Binaryen;
    } else if (hasHostOptimizer) {
        selection.hostOptimize = true;
    } else if (mode == "binaryen") {
        selection.error = "Binaryen requested, but neither the C API nor "
                          "a host optimizer is available";
    }
    return selection;
}

WasmCompileResult compileWithBackend(
    const Program& program, WasmBackend backend, int optimizationLevel) {
    auto input = compileProgram(program);
    if (input.empty()) {
        return {{}, "WebAssembly lowering failed"};
    }
    return optimizeWasmWithBackend(
        std::move(input), backend, optimizationLevel);
}

WasmCompileResult optimizeWasmWithBackend(
    std::vector<uint8_t> input, WasmBackend backend, int optimizationLevel) {
    WasmCompileResult out;
    if (optimizationLevel < 0 || optimizationLevel > 3) {
        out.error = "invalid optimization level (expected 0..3)";
        return out;
    }
    if (backend == WasmBackend::Builtin) {
        out.bytes = std::move(input);
        return out;
    }
    if (backend != WasmBackend::Binaryen) {
        out.error = "unknown WebAssembly backend";
        return out;
    }
    // "auto" prefers the linked C API and skips filesystem and process
    // startup entirely. "external" is retained for compatibility and
    // side-by-side benchmarks. "library" requires the in-process backend.
    const char* configured = std::getenv("XYO_BINARYEN_ENGINE");
    const std::string engine = configured && *configured ? configured : "auto";
    if (engine != "auto" && engine != "library" && engine != "external") {
        out.error = "XYO_BINARYEN_ENGINE must be auto|library|external";
        return out;
    }
    if (engine == "library" ||
        (engine == "auto" && hasBinaryenCAPI())) {
        if (!optimizeWithBinaryenCAPI(
                input, optimizationLevel, out.bytes, out.error)) {
            out.bytes.clear();
        }
        return out;
    }
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    out.error = "Binaryen command-line backend is currently available on POSIX hosts only";
#else
    // Do not silently substitute unoptimized output when Binaryen fails.
    if (input.size() > 32u * 1024u * 1024u) {
        out.error = "WASM module exceeds Binaryen input limit (32 MiB)";
        return out;
    }
    TempFile in, tmpout;
    if (!in.open(out.error) || !tmpout.open(out.error) ||
        !in.writeAll(input, out.error)) return out;
    ::close(in.fd); in.fd = -1;
    ::close(tmpout.fd); tmpout.fd = -1;
    const char* envTool = std::getenv("XYO_WASM_OPT");
    const std::string tool = envTool && *envTool ? envTool : "wasm-opt";
    if (!runBinaryen(tool, in.path, tmpout.path, optimizationLevel, out.error))
        return out;

    std::ifstream stream(tmpout.path, std::ios::binary | std::ios::ate);
    if (!stream) {
        out.error = "cannot open Binaryen output";
        return out;
    }
    const auto size = stream.tellg();
    if (size <= 8 || size > 32 * 1024 * 1024) {
        out.error = "invalid Binaryen output size";
        return out;
    }
    stream.seekg(0);
    out.bytes.resize(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(out.bytes.data()), size) ||
        out.bytes[0] != 0 || out.bytes[1] != 'a' ||
        out.bytes[2] != 's' || out.bytes[3] != 'm') {
        out.bytes.clear();
        out.error = "invalid Binaryen output binary";
    }
#endif
    return out;
}

} // namespace xyo::wasm
