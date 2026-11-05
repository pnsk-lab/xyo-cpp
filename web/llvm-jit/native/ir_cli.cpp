// Diagnostic CLI for the vendored C++ browser IR API.
// Native compilation is only for inspection: its ABI metadata is NOT wasm32.
#include "browser_ir_api.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: xyo-browser-ir-diagnostic file.sb3\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::cerr << "cannot open " << argv[1] << '\n'; return 2; }
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto* result = xyo_browser_ir_compile(bytes.data(), bytes.size());
    if (!xyo_browser_ir_ok(result)) {
        std::cerr << xyo_browser_ir_error(result) << '\n';
        xyo_browser_ir_destroy(result);
        return 1;
    }
    std::cout << xyo_browser_ir_text(result);
    std::cerr << xyo_browser_ir_metadata(result) << '\n';
    xyo_browser_ir_destroy(result);
}
