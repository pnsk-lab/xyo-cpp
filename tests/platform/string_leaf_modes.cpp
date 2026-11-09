// Compact Latin-1 leaf representation preserves the Scratch UTF-16 ABI.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

using namespace xyo;
namespace {
int failures = 0;
void check(bool v, const char* message) {
    if (!v) {
        ++failures;
        std::fprintf(stderr, "FAIL string-leaf: %s\n", message);
    }
}
uint32_t payloadBytes(const SStr* s) {
    return (reinterpret_cast<const GcHeader*>(s) - 1)->size;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    bool compact = std::strcmp(argv[1], "latin1") == 0;
    check(stringLeafMode() == (compact ? StringLeafMode::Latin1
                                       : StringLeafMode::Utf16),
          "environment chooses leaf representation");
    check(!setStringLeafMode("invalid"), "invalid mode is rejected");
    Gc::DeferCollect defer;
    SStr* latin = SStr::create(u"Caf\u00e9\u00ff");
    check(latin->isLatin1() == compact, "Latin-1 compression selection");
    check(latin->body == nullptr, "unmaterialized leaf");
    check(latin->len == 5, "Latin-1 length in code units");
    check(latin->unitAt(3) == 0xE9 && latin->unitAt(4) == 0xFF,
          "one-byte random access and unsigned extension");
    check(latin->body == nullptr, "unitAt doesn't widen leaf");
    check(payloadBytes(latin) ==
              sizeof(SStr) + (latin->len + 1) * (compact ? 1 : 2),
          "GC payload uses actual byte width");
    check(latin->view() == u"Caf\u00e9\u00ff", "UTF-16 contiguous view");
    if (compact) {
        check(latin->body != nullptr, "view widens leaf once");
        check(latin->view().data() == latin->body,
              "widening returns cached buffer");
    }

    SStr* unicode = SStr::create(u"\u0100\xD83D\xDE00");
    check(!unicode->isLatin1(), "non-Latin-1 code unit falls back to UTF-16");
    check(unicode->len == 3 && unicode->unitAt(1) == 0xD83D &&
              unicode->unitAt(2) == 0xDE00, "surrogate pair code units");
    check(unicode->view() == u"\u0100\xD83D\xDE00", "wide view parity");

    SStr* ascii = sstrFromAscii("123");
    check(ascii->isLatin1() == compact, "ASCII number conversion leaf");
    check(cast::toNumber(Value::string(ascii)) == 123.0,
          "numeric cast with compact leaf");
    check(cast::toNumber(Value::string(SStr::create(u"-7"))) == -7.0,
          "single-digit numeric fast path");
    check(cast::compare(Value::string(SStr::create(u"ABC")),
                        Value::string(SStr::create(u"abc"))) == 0,
          "case-insensitive equality");
    {
        SStr* upper = SStr::create(u"HELLOabc");
        SStr* lower = SStr::create(u"helloABC");
        SStr* needle = SStr::create(u"oA");
        check(compareStringsCaseInsensitive(upper, lower) == 0,
              "compact ASCII comparison");
        check(sstrContains(upper, needle), "compact ASCII contains");
        check(!sstrContains(upper, SStr::create(u"nope")),
              "compact ASCII contains miss");
        check(sstrContains(upper, SStr::create(u"lloAB")),
              "long compact ASCII BMH hit");
        check(!sstrContains(upper, SStr::create(u"zzzxyz")),
              "long compact ASCII BMH miss");
        if (compact)
            check(upper->body == nullptr && lower->body == nullptr &&
                  needle->body == nullptr,
                  "ASCII comparison/search does not widen compact leaves");
    }

    SStr* joined = sstrConcat(u"caf\u00e9", u"!");
    check(joined->isLatin1() == compact, "compact flat concatenation");
    check(joined->view() == u"caf\u00e9!", "compact concat view");
    {
        SStr* lhs = SStr::create(u"caf\u00e9");
        SStr* rhs = SStr::create(u"1234");
        SStr* fast = sstrConcatStrings(lhs, rhs);
        check(fast->view() == u"caf\u00e91234", "direct leaf concatenation");
        if (compact) {
            check(lhs->body == nullptr && rhs->body == nullptr,
                  "concatenation must not widen either compact input");
            check(fast->isLatin1(), "direct concat returns compact output");
        }
        SStr* fastAscii = sstrConcatStringAscii(lhs, "5678");
        check(fastAscii->view() == u"caf\u00e95678", "direct ASCII suffix");
        if (compact)
            check(lhs->body == nullptr, "ASCII suffix must not widen its prefix");
    }
    SStr* mixed = sstrConcat(u"caf\u00e9", u"\u0100");
    check(!mixed->isLatin1() && mixed->view() == u"caf\u00e9\u0100",
          "mixed UTF-16 fallback");
    SStr* asciiSuffix = sstrConcatAscii(u"abc", "42");
    check(asciiSuffix->isLatin1() == compact &&
              asciiSuffix->view() == u"abc42", "ascii concat");

    SStr* root = sstrCons(SStr::create(u"caf\u00e9"),
                          SStr::create(u"\u00ff\u0100"));
    check(root->len == 6 && root->unitAt(3) == 0xE9 &&
              root->unitAt(4) == 0xFF && root->unitAt(5) == 0x100,
          "mixed rope indexing");
    check(root->body == nullptr, "rope indexing is lazy");
    check(root->view() == u"caf\u00e9\u00ff\u0100",
          "mixed rope flattening");

    {
        SStr* current = SStr::create(u"abcdef1234567890");
        for (unsigned i = 0; i < 256; ++i)
            current = sstrCons(current, SStr::create(u"\u00e9"));
        check(current->len == 272, "deep rope length");
        check(current->unitAt(271) == 0xE9, "deep rope unitAt");
        check(current->view().back() == 0xE9, "deep rope contents");
    }

    {
        const SStr* shared = SStr::create(u"\u00e9\u00ffABC123");
        std::atomic<bool> ok{true};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t)
            readers.emplace_back([&, shared] {
                for (int i = 0; i < 1000; ++i) {
                    if (shared->view() != u"\u00e9\u00ffABC123" ||
                        shared->unitAt(1) != 0xFF) ok.store(false);
                }
            });
        for (auto& th : readers) th.join();
        check(ok.load(), "concurrent widening is consistent");
    }

    check(setStringLeafMode(compact ? "utf16" : "latin1"),
          "changing representation mode succeeds");
    SStr* switched = SStr::create(u"caf\u00e9");
    check(switched->isLatin1() == !compact, "new objects use new mode");
    check(latin->view() == u"Caf\u00e9\u00ff", "previous leaves remain valid");

    std::printf("string leaf %s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
