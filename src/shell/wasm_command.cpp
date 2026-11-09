// Standalone WebAssembly output for eligible Scratch scripts.
// No LLVM IR, LLVM linker or native code execution is involved.
#include "wasm_command.hpp"

#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "wasm/binaryen_backend.hpp"
#include "wasm/wasm_compile.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool readSb3(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size <= 0 || size > 128 * 1024 * 1024) return false;
    input.seekg(0);
    bytes.resize(static_cast<size_t>(size));
    return bool(input.read(reinterpret_cast<char*>(bytes.data()), size));
}

std::string escapeJson(const std::string& value) {
    const char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result.push_back('\\');
            result.push_back(char(c));
        } else if (c < 0x20) {
            result += "\\u00";
            result.push_back(digits[c >> 4]);
            result.push_back(digits[c & 15]);
        } else {
            result.push_back(char(c));
        }
    }
    return result + '"';
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return bool(f);
}

bool writeManifest(const std::string& path, const std::string& target,
                   const std::string& script, size_t targetIndex,
                   const xyo::wasm::Program& program,
                   xyo::wasm::WasmBackend backend, int opt) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n  \"version\": 1,\n  \"entry\": \"f\",\n"
           "  \"memory\": {\"module\": \"env\", \"name\": \"memory\", "
           "\"minPages\": 256},\n"
           "  \"status\": {\"complete\": 0, \"bail\": 1},\n"
        << "  \"target\": " << escapeJson(target)
        << ",\n  \"script\": " << escapeJson(script)
        << ",\n  \"targetIndex\": " << targetIndex
        << ",\n  \"backend\": \""
        << (backend == xyo::wasm::WasmBackend::Binaryen ? "binaryen" : "builtin")
        << "\",\n  \"optLevel\": " << opt
        << ",\n  \"slots\": [";
    for (size_t i = 0; i < program.slotIds.size(); ++i) {
        if (i) out << ", ";
        out << escapeJson(program.slotIds[i]);
    }
    out << "]\n}\n";
    out.close();
    return bool(out);
}

} // namespace

int cmdWasm(int argc, char** argv) {
    using xyo::wasm::WasmBackend;
    if (argc < 4) {
        std::fprintf(stderr, "usage: xyo wasm <project.sb3> -o <output.wasm> "
                             "[--script TOP_ID] [--backend binaryen|builtin] "
                             "[--opt 0|1|2|3]\n");
        return 2;
    }
    const std::string project = argv[2];
    std::string destination, scriptId, manifest;
    WasmBackend backend = WasmBackend::Binaryen;
    int optimization = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-o" || arg == "--output") && i + 1 < argc)
            destination = argv[++i];
        else if (arg == "--script" && i + 1 < argc)
            scriptId = argv[++i];
        else if (arg == "--metadata" && i + 1 < argc)
            manifest = argv[++i];
        else if (arg == "--backend" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "binaryen") backend = WasmBackend::Binaryen;
            else if (value == "builtin") backend = WasmBackend::Builtin;
            else {
                std::fprintf(stderr, "wasm: invalid backend %s\n", value.c_str());
                return 2;
            }
        } else if (arg == "--opt" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.size() != 1 || value[0] < '0' || value[0] > '3') {
                std::fprintf(stderr, "wasm: --opt must be in 0..3\n");
                return 2;
            }
            optimization = value[0] - '0';
        } else {
            std::fprintf(stderr, "wasm: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (destination.empty()) {
        std::fprintf(stderr, "wasm: output path is required (-o)\n");
        return 2;
    }
    if (manifest.empty()) manifest = destination + ".json";
    if (manifest == destination) {
        std::fprintf(stderr, "wasm: metadata must not overwrite the wasm output\n");
        return 2;
    }

    std::vector<uint8_t> bytes;
    if (!readSb3(project.c_str(), bytes)) {
        std::fprintf(stderr, "wasm: cannot read SB3 (or input is oversized)\n");
        return 1;
    }

    // Avoid executing scripts while compiling them. This also prevents
    // HotSpot counters from influencing the independent WASM backend.
    xyo::setJitMode(xyo::JitMode::Off);
    xyo::Runtime runtime;
    auto loaded = xyo::loadSb3File(runtime, bytes.data(), bytes.size());
    if (!loaded.ok) {
        std::fprintf(stderr, "wasm: %s\n", loaded.error.c_str());
        return 1;
    }

    xyo::wasm::Program selected;
    std::string selectedTarget, selectedScript;
    size_t matches = 0;
    size_t selectedTargetIndex = 0;
    for (size_t targetIndex = 0; targetIndex < runtime.targets.size(); ++targetIndex) {
        auto* target = runtime.targets[targetIndex];
        if (!target || !target->isOriginal || !target->blocks) continue;
        for (const auto& id : target->blocks->getScripts()) {
            if (!scriptId.empty() && scriptId != id) continue;
            const auto* top = target->blocks->getBlock(id);
            // No event scheduling ABI is provided: a green flag script is
            // the only supported explicit entry point for now.
            if (!top || top->opcode != "event_whenflagclicked") continue;
            xyo::wasm::Program candidate;
            if (!xyo::wasm::lowerScript(*target->blocks, top, candidate))
                continue;
            ++matches;
            if (matches == 1) {
                selected = std::move(candidate);
                selectedTarget = target->id;
                selectedTargetIndex = targetIndex;
                selectedScript = id;
            }
        }
    }
    if (matches != 1) {
        std::fprintf(stderr,
            "wasm: expected exactly one supported green-flag script, found %zu. "
            "Use --script TOP_ID or remove unsupported blocks.\n", matches);
        return 1;
    }

    auto result = xyo::wasm::compileWithBackend(selected, backend, optimization);
    if (!result.error.empty()) {
        std::fprintf(stderr, "wasm: %s\n", result.error.c_str());
        return 1;
    }
    if (!writeBytes(destination, result.bytes)) {
        std::fprintf(stderr, "wasm: cannot write %s\n", destination.c_str());
        return 1;
    }
    if (!writeManifest(manifest, selectedTarget, selectedScript,
                       selectedTargetIndex, selected, backend, optimization)) {
        std::fprintf(stderr, "wasm: cannot write metadata %s\n", manifest.c_str());
        return 1;
    }
    std::printf("wasm: %s backend=%s -O%d bytes=%zu slots=%u target=%s script=%s\n",
                destination.c_str(),
                backend == WasmBackend::Binaryen ? "binaryen" : "builtin",
                optimization, result.bytes.size(), selected.nSlots,
                selectedTarget.c_str(), selectedScript.c_str());
    return 0;
}
