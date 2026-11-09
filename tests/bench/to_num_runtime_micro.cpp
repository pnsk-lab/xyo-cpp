// Compare xyo_to_num's direct entry with the canonical cast::toNumber path.
// Both use the same ECMAScript parser; this isolates NaN-box dispatch and
// atomic cached-string lookup. Not part of normal builds / CI.
#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

extern "C" double xyo_to_num(uint64_t bits);
extern "C" double xyo_to_num_uncached(xyo::SStr* string);

using namespace xyo;

static double reference(uint64_t bits) {
    return cast::toNumber(Value{bits});
}

using Fn = double (*)(uint64_t);

// Model the two JIT cold-string call sites, both after verifying the same
// numState flag. Keep it in separate functions to measure call overhead.
static double genericCold(uint64_t bits) {
    auto* string = reinterpret_cast<SStr*>(bits & val::kPtrMask);
    const uint32_t state = string->loadNumState();
    if (state == 0) return xyo_to_num(bits);
    return state == 2 ? 0.0 : string->loadNumCache();
}

static double specializedCold(uint64_t bits) {
    auto* string = reinterpret_cast<SStr*>(bits & val::kPtrMask);
    const uint32_t state = string->loadNumState();
    if (state == 0) return xyo_to_num_uncached(string);
    return state == 2 ? 0.0 : string->loadNumCache();
}

static void measure(const char* title, const char* name, Fn fn,
                    const std::vector<Value>& values, bool cold) {
    uint64_t checksum = 0;
    constexpr int kRounds = 3000;
    auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < kRounds; ++r) {
        for (Value v : values) {
            if (cold && v.isString() && v.asStrPtr()) {
                SStr* str = v.asStrPtr();
                std::atomic_ref<uint32_t>(str->numState).store(
                    0, std::memory_order_relaxed);
            }
            const double number = fn(v.bits);
            checksum += std::bit_cast<uint64_t>(number);
        }
    }
    auto elapsed = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("%s %s %.2f ns/op checksum=%016llx\n", title, name,
                elapsed / (kRounds * values.size()),
                static_cast<unsigned long long>(checksum));
}

int main() {
    std::vector<Value> immediates, numericStrings, digitStrings,
                       signedDigitStrings, invalidStrings, whitespaceStrings;
    for (int i = 0; i < 512; ++i) {
        const char16_t digit = char16_t(u'0' + i % 10);
        digitStrings.push_back(Value::string(SStr::create(
            std::u16string(1, digit))));
        signedDigitStrings.push_back(Value::string(SStr::create(
            std::u16string(1, i % 2 ? u'-' : u'+') +
            std::u16string(1, digit))));
        invalidStrings.push_back(Value::string(SStr::create(u"x")));
        whitespaceStrings.push_back(Value::string(SStr::create(
            std::u16string(1, i % 2 ? u' ' : u'\t'))));
        immediates.push_back(Value::num(
            (i % 3 == 0) ? i * 0.25 : -i * 1.125));
        const std::string str = std::to_string(i * 11);
        numericStrings.push_back(Value::string(
            SStr::fromUtf8(str)));
    }
    for (Value v : numericStrings) (void)reference(v.bits);

    for (const auto& [title, values, cold] :
         std::vector<std::tuple<const char*, const std::vector<Value>*, bool>>{
            {"immediate", &immediates, false},
            {"cached-string", &numericStrings, false},
            {"cold-string", &numericStrings, true},
            {"cold-digit", &digitStrings, true},
            {"cold-signed-digit", &signedDigitStrings, true},
            {"cold-invalid", &invalidStrings, true},
            {"cold-whitespace", &whitespaceStrings, true},
         }) {
        for (int round = 0; round < 3; ++round) {
            measure(title, "cast", reference, *values, cold);
            measure(title, "helper", xyo_to_num, *values, cold);
        }
    }
    for (int round = 0; round < 5; ++round) {
        measure("jit-cold", "generic", genericCold, numericStrings, true);
        measure("jit-cold", "special", specializedCold, numericStrings, true);
    }
}
