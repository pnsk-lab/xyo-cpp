#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
extern "C" size_t xyo_i64_to_ascii_asm(int64_t, char*);

static void check(int64_t value) {
    char got[40], ref[40];
    std::memset(got, 0x5a, sizeof(got));
    auto n = xyo_i64_to_ascii_asm(value, got);
    auto [end, err] = std::to_chars(ref, ref + sizeof(ref), value);
    auto expected = size_t(end - ref);
    if (err != std::errc{} || n != expected ||
        std::memcmp(got, ref, expected) != 0 ||
        got[32] != char(0x5a)) {
        std::fprintf(stderr, "mismatch for %lld: asm[%.*s] ref[%.*s], sizes %zu/%zu\n",
                     (long long)value, int(n), got,
                     int(expected), ref, n, expected);
        std::exit(1);
    }
}
int main() {
    for (auto v : std::array<int64_t, 25>{
         INT64_MIN, INT64_MIN+1, INT64_MAX, INT64_MAX-1,
         -9007199254740992ll,-9007199254740991ll,
         -1000000000000000ll,-100000000000000ll,-1000000000000ll,
         -1000000000ll,-10000ll,-100ll,-99ll,-11ll,-10ll,-9ll,-1ll,
         0ll,1ll,9ll,10ll,11ll,99ll,100ll,9007199254740992ll})
        check(v);
    std::mt19937_64 gen(0x6CDEA123907ull);
    for (int i=0;i<1000000;i++)
        check(static_cast<int64_t>(gen()));
    // Exercise the reciprocal /100 path heavily: generic uniform 64-bit
    // sampling rarely lands in the [0, 2^53] exact-double subset.
    constexpr uint64_t kExactMax = 9007199254740992ull;
    for (int i=0; i<2000000; ++i) {
        int64_t magnitude=int64_t(gen() % (kExactMax + 1));
        check(i % 2 ? -magnitude : magnitude);
    }
    std::puts("PASS 3,000,025 i64 integers match to_chars");
}
