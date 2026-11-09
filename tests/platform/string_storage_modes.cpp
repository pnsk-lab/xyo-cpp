// Exercise the same Scratch join path used by the interpreter and JIT helpers.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

namespace {
using namespace xyo;

int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}
Value str(std::u16string_view text) {
    return Value::string(SStr::create(text));
}
std::u16string_view contents(Value value) {
    return cast::toString(value)->view();
}

void checkShape(Value value, StringStorageMode mode, const char* context) {
    SStr* s = value.asStrPtr();
    check(s != nullptr, context);
    if (!s) return;
    if (mode == StringStorageMode::Flat) {
        check(!s->isComposite(), "flat mode must not allocate composite joins");
    } else if (mode == StringStorageMode::Rope) {
        check(s->isCons(), "rope mode must allocate cons for long joins");
    } else {
        check(s->isAppend(), "append-log mode must allocate append snapshots");
    }
}
} // namespace

int main(int argc, char** argv) {
    using namespace xyo;
    if (argc != 2) return 2;
    const StringStorageMode expected =
        std::strcmp(argv[1], "flat") == 0 ? StringStorageMode::Flat :
        std::strcmp(argv[1], "append-log") == 0 ? StringStorageMode::AppendLog :
        StringStorageMode::Rope;
    // CTest sets the environment; verify the real configuration path.
    check(stringStorageMode() == expected, "environment selects storage mode");
    check(setStringStorageMode("invalid") == false, "reject unknown storage modes");
    check(stringStorageMode() == expected, "invalid setting cannot change mode");
    check(setStringStorageMode(argv[1]), "CLI setter accepts the mode");

    {
        Value joined = rt::join(str(u"abcdefghijklmnop"), str(u"日本"));
        check(contents(joined) == u"abcdefghijklmnop日本", "UTF-16 long join");
        checkShape(joined, expected, "long join result");
        check(rt::strLength(joined) == 18.0, "UTF-16 length");
        check(contents(rt::letterOf(Value::num(17), joined)) == u"日",
              "UTF-16 indexing");
        check(rt::strContains(joined, str(u"nop日")), "case-insensitive contains");
    }
    {
        Value base = str(u"abcdefghijklmnop");
        Value a = rt::join(base, str(u"A"));
        Value b = rt::join(a, str(u"B"));
        Value fork = rt::join(a, str(u"C"));
        checkShape(a, expected, "base snapshot");
        checkShape(b, expected, "extended snapshot");
        checkShape(fork, expected, "forked snapshot");
        check(contents(a) == u"abcdefghijklmnopA", "snapshot remains immutable");
        check(contents(b) == u"abcdefghijklmnopAB", "extended snapshot");
        check(contents(fork) == u"abcdefghijklmnopAC", "fork does not mutate parent");
        check(a.asStrPtr()->unitAt(16) == u'A', "unitAt on snapshot");
    }
    {
        Value numeric = rt::join(str(u"abcdefghijklmnop"), Value::num(42));
        check(contents(numeric) == u"abcdefghijklmnop42", "numeric suffix");
        checkShape(numeric, expected, "numeric suffix representation");
        Value prefixed = rt::join(Value::num(42), str(u"abcdefghijklmnop"));
        check(contents(prefixed) == u"42abcdefghijklmnop", "numeric prefix");
        checkShape(prefixed, expected, "numeric prefix representation");
        check(contents(rt::join(Value::num(-0.0), str(u"x"))) == u"0x",
              "negative zero");
        check(contents(rt::join(str(u""), Value::num(3.5))) == u"3.5",
              "empty prefix conversion");
    }
    {
        Value joined = rt::join(str(u"abcdefghijklmnop"), str(u"\U0001F600"));
        check(contents(joined).size() == 18, "surrogate pair is two UTF-16 units");
        check(joined.asStrPtr()->unitAt(16) == 0xD83D, "surrogate high");
        check(joined.asStrPtr()->unitAt(17) == 0xDE00, "surrogate low");
    }
    {
        std::u16string largeSuffix(300, u'x');
        Value joined = rt::join(str(u"abcdefghijklmnop"), str(largeSuffix));
        check(contents(joined).size() == 316, "oversized suffix contents");
        if (expected == StringStorageMode::AppendLog)
            check(joined.asStrPtr()->isCons(), "oversized append falls back to rope");
        if (expected == StringStorageMode::Flat)
            check(!joined.asStrPtr()->isComposite(), "oversized flat stays flat");
    }
    {
        Value accum = str(u"abcdefghijklmnop");
        std::u16string reference = u"abcdefghijklmnop";
        for (int i = 0; i < 512; ++i) {
            const auto part = i % 7 == 0 ? u"語" : u"ab";
            accum = rt::join(accum, str(part));
            reference += part;
            if (i % 41 == 0)
                check(contents(accum) == reference, "repeated append parity");
        }
        check(contents(accum) == reference, "long append parity");
        check(accum.asStrPtr()->unitAt(static_cast<uint32_t>(reference.size() - 1))
                  == reference.back(), "last code unit");
    }
    {
        // Changing the policy does not invalidate strings allocated previously.
        Value rope;
        check(setStringStorageMode("rope"), "switch to rope");
        rope = rt::join(str(u"abcdefghijklmnop"), str(u"XYZ"));
        check(rope.asStrPtr()->isCons(), "switch produces rope");
        check(setStringStorageMode("flat"), "switch to flat");
        Value flat = rt::join(rope, str(u"日本"));
        check(!flat.asStrPtr()->isComposite(), "switch produces flat");
        check(contents(rope) == u"abcdefghijklmnopXYZ", "old rope survives switch");
        check(contents(flat) == u"abcdefghijklmnopXYZ日本", "new flat is correct");
    }
    std::printf("string storage %s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
