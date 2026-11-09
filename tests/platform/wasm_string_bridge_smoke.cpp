// Native semantics regression for the WASM host's string-to-number ABI.
// Real Emscripten imports additionally require a browser integration test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/wasm/string_number_bridge.hpp"
#include "../../src/core/sstr.hpp"
#include "../../src/wasm/wasm_compile.hpp"
#include <fstream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

int main(int argc, char** argv) {
    using namespace xyo;
    struct Case { std::string_view text; double expected; };
    for (const Case& test : {
        Case{"42.5", 42.5}, Case{"  -12  ", -12},
        Case{"0x10", 16}, Case{"nonsense", 0},
        Case{"0", 0}, Case{"  ", 0},
        Case{"Infinity", std::numeric_limits<double>::infinity()}
    }) {
        const Value input = Value::string(SStr::fromUtf8(test.text));
        uint64_t staged = input.bits;
        assert(wasm::convertHeapStringNumberSlot(&staged));
        double converted = 0;
        std::memcpy(&converted, &staged, sizeof(converted));
        assert(converted == test.expected);
        assert(staged == Value::num(converted).bits);
    }
    for (uint64_t raw : {Value::num(123).bits, val::kTrue,
                         val::kEmptyStr, val::kUndefined,
                         val::kTagColor | uint64_t{42}}) {
        const uint64_t original = raw;
        assert(!wasm::convertHeapStringNumberSlot(&raw));
        assert(raw == original);
    }
    assert(!wasm::convertHeapStringNumberSlot(nullptr));

    // Optional integration fixture generator: both modes compile the same
    // Scratch statement; the host-enabled mode may import env.h.
    if (argc == 2) {
        wasm::Program program;
        program.slotIds = {"v_x"};
        program.nSlots = 1;
        const uint32_t literal = program.exprNum(5);
        wasm::Stmt change;
        change.k = wasm::SK::ChangeVar;
        change.var = 0;
        change.expr = literal;
        program.main = {program.stmt(change)};
        for (const bool host : {false, true}) {
            program.useHostMath = host;
            const auto bytes = wasm::compileProgram(program);
            assert(!bytes.empty());
            const std::string filename = std::string(argv[1]) +
                (host ? "/string-host.wasm" : "/string-standalone.wasm");
            std::ofstream file(filename, std::ios::binary);
            assert(file);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            assert(file);
        }
    } else assert(argc == 1);
    return 0;
}
