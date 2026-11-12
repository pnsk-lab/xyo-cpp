// The optional yyjson reader must produce exactly the same JsonValue DOM as
// the self-contained recursive-descent parser, including duplicate-key
// behavior and fallback for legacy malformed Unicode.
#include "core/json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool setBackend(const char* value) {
#ifdef _WIN32
    return _putenv_s("XYO_JSON_YYJSON", value) == 0;
#else
    return setenv("XYO_JSON_YYJSON", value, 1) == 0;
#endif
}

bool compare(std::string_view source) {
    if (!setBackend("0")) return false;
    const xyo::JsonValue legacy = xyo::jsonParse(source);
    if (!setBackend("1")) return false;
    const xyo::JsonValue fast = xyo::jsonParse(source);
    if (legacy.isNull() != fast.isNull()) return false;
    if (legacy.isBool() != fast.isBool()) return false;
    if (legacy.isNumber() != fast.isNumber()) return false;
    if (legacy.isString() != fast.isString()) return false;
    if (legacy.isArray() != fast.isArray()) return false;
    if (legacy.isObject() != fast.isObject()) return false;
    return xyo::jsonStringify(legacy) == xyo::jsonStringify(fast);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> cases = {
        "null", "true", "false", "0", "-0", "-0.0", "-0e0",
        "-0E+10", "-0e-400", "-1.23e-4",
        "18446744073709551615", "-9223372036854775809",
        "9007199254740993", "1.2345678901234567",
        "1e-300", "1e-320", "-5e-324", "1e500",
        "[1,true,null,2.0,[],{}]",
        R"({"z":[{"id":"r_1","f":[5,"日本語"]}],"a":{"inner":null}})",
        R"({"emoji":"\uD83D\uDE80","unicode":"\u3053\u3093\u306B\u3061\u306F"})",
        R"({"slash":"a\\b","quote":"a\"b","line":"\n","tab":"\t"})",
        R"({"same":1,"same":2,"other":{"k":1,"k":2}})",
        R"({"lists":[[1,2],[2,3]],"m":{"x":-0.0,"y":1000000}})",
        R"({"bad":"\uD800"})", // yyjson may reject; legacy fallback
        R"({"bad":"\uDC00"})",
        R"({"bad":"\x"})",      // both reject
        R"({"bad":[1,]})",      // both reject
        R"({"bad":+1})"         // nonstandard, handled by legacy fallback
    };

    size_t checks = 0;
    for (const auto& input : cases) {
        if (!compare(input)) {
            std::cerr << "JSON backend mismatch for: " << input << '\n';
            return 1;
        }
        ++checks;
    }
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) { std::cerr << "Cannot read fixture\n"; return 2; }
        const std::string bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        if (!compare(bytes)) {
            std::cerr << "Full project.json DOM mismatch\n";
            return 1;
        }
        ++checks;
    }
    std::cout << "PASS: legacy / yyjson DOM parity (" << checks << " cases)\n";
}
