// Validates ASM's ECMAScript scientific-to-decimal layout against the exact
// legacy C++ policy over synthetic shortest-significand and exponent ranges.
// Assembly accepts only positive finite shortest decimal digits (1..17).
#include <charconv>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

extern "C" size_t xyo_ecma_format_asm(
    const char* digits, uint32_t k, int32_t n, char* output);
extern "C" size_t xyo_ecma_from_scientific_asm(
    const char* scientific, uint32_t length, char* output);

static std::string legacy(std::string_view digits, int n) {
    const int k = static_cast<int>(digits.size());
    std::string out;
    if (k <= n && n <= 21) {
        out.append(digits).append(n-k, '0');
    } else if (0 < n && n <= 21) {
        out.append(digits.substr(0,n)).push_back('.');
        out.append(digits.substr(n));
    } else if (-6 < n && n <= 0) {
        out = "0.";
        out.append(-n,'0').append(digits);
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out.append(digits.substr(1));
        }
        out.push_back('e');
        int ex = n-1;
        out.push_back(ex < 0 ? '-' : '+');
        char eb[16];
        auto [end, err] = std::to_chars(eb,eb+sizeof(eb),ex<0?-ex:ex);
        if (err != std::errc()) std::abort();
        out.append(eb,end);
    }
    return out;
}
static void check(std::string_view digits,int n) {
    alignas(16) char got[80];
    std::memset(got,0x5a,sizeof(got));
    size_t len=xyo_ecma_format_asm(
        digits.data(),static_cast<uint32_t>(digits.size()),n,got);
    const auto expected=legacy(digits,n);
    if (len!=expected.size() || memcmp(got,expected.data(),len)!=0 ||
        static_cast<unsigned char>(got[64]) != 0x5a) {
        fprintf(stderr,"ASM mismatch for [%.*s], n=%d: got=[%.*s] expected=[%s] len=%zu\n",
                int(digits.size()),digits.data(),n,int(len),got,expected.c_str(),len);
        exit(1);
    }
    // Also test the direct scientific text -> ECMAScript path, including
    // two-digit padded exponent input and negative zero-like exponents.
    const int exponent = n - 1;
    std::string scientific;
    scientific.push_back(digits[0]);
    if (digits.size() > 1) {
        scientific.push_back('.');
        scientific.append(digits.substr(1));
    }
    scientific.push_back('e');
    scientific.push_back(exponent < 0 ? '-' : '+');
    std::string exponentDigits = std::to_string(
        exponent < 0 ? -exponent : exponent);
    if (n % 2 == 0 && exponentDigits.size() < 2)
        exponentDigits.insert(0, 2 - exponentDigits.size(), '0');
    scientific.append(exponentDigits);

    std::memset(got, 0x5a, sizeof(got));
    len = xyo_ecma_from_scientific_asm(
        scientific.data(), uint32_t(scientific.size()), got);
    if (len != expected.size() ||
        std::memcmp(got, expected.data(), len) != 0 ||
        static_cast<unsigned char>(got[64]) != 0x5a) {
        fprintf(stderr, "Scientific->ECMA mismatch [%s]: got [%.*s] expected [%s]\n",
                scientific.c_str(), int(len), got, expected.c_str());
        exit(1);
    }
}
int main() {
    // The relevant ECMAScript thresholds are n=-5/-6 and n=21/22.
    // Exponent magnitude may be 324 for subnormals.
    std::mt19937_64 rng(0xC4B582197Aull);
    for(int n=-327;n<=310;n++) {
        for(int k=1;k<=17;k++) {
            for(int variant=0;variant<20;variant++) {
                char digits[18];
                digits[0]=char('1'+(rng()%9));
                for(int i=1;i<k;i++) digits[i]=char('0'+(rng()%10));
                check(std::string_view(digits,k),n);
            }
        }
    }
    // 216,920 systematic cases, plus 500,000 randomized cases.
    for(int i=0;i<500000;i++) {
        int n = -327 + int(rng()%638);
        int k = 1 + int(rng()%17);
        char digits[18];
        digits[0]=char('1'+(rng()%9));
        for(int j=1;j<k;j++) digits[j]=char('0'+(rng()%10));
        check(std::string_view(digits,k),n);
    }
    puts("PASS 716,920 ECMAScript layout inputs");
}
