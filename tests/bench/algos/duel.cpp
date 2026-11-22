// xyo-cpp vs C++ 対決: Scratch と 1:1 対応の素朴実装 (faithful 部門).
// - ソート列は double 配列 (Scratch の値は f64), LCG データは gen と同一.
// - 最適化でループが畳まれないよう、反復カウンタに空バリアを置く
//   (命令は増えない。Google Benchmark の DoNotOptimize と同趣旨).
// - 計測はアルゴリズム本体のみ。データ生成・チェックサムは計測外.
// 使い方: duel <algo> <n> <reps>   ->  stdout に "check=..." と "times: ..." を出す.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

inline void opaque(long long& x) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+r"(x));
#else
    volatile long long* p = &x;
    (void)*p;
#endif
}

const long long LCG_A = 25173, LCG_B = 13849, LCG_M = 65536;
const int SEED0 = 12345;

std::vector<double> lcg(int n, int seed = SEED0) {
    std::vector<double> out(n);
    long long s = seed;
    for (int i = 0; i < n; ++i) {
        s = (s * LCG_A + LCG_B) % LCG_M;
        out[i] = (double)s;
    }
    return out;
}

// ---- quicksort: Hoare, 中央ピボット, py_quicksort と同形 ----
void qs(std::vector<double>& a, long lo, long hi) {
    if (lo < hi) {
        double pivot = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) ++i;
            while (a[j] > pivot) --j;
            if (i <= j) {
                double t = a[i];
                a[i] = a[j];
                a[j] = t;
                ++i;
                --j;
            }
        }
        if (lo < j) qs(a, lo, j);
        if (i < hi) qs(a, i, hi);
    }
}

// ---- mergesort: aux 付き, py_mergesort と同形 ----
void ms_rec(std::vector<double>& a, std::vector<double>& aux, long lo,
            long hi) {
    if (lo < hi) {
        long mid = (lo + hi) / 2;
        ms_rec(a, aux, lo, mid);
        ms_rec(a, aux, mid + 1, hi);
        long i = lo, j = mid + 1, k = lo;
        while (k <= hi) {
            if (i > mid) {
                aux[k] = a[j];
                ++j;
            } else if (j > hi) {
                aux[k] = a[i];
                ++i;
            } else if (a[i] <= a[j]) {
                aux[k] = a[i];
                ++i;
            } else {
                aux[k] = a[j];
                ++j;
            }
            ++k;
        }
        for (long k2 = lo; k2 <= hi; ++k2) a[k2] = aux[k2];
    }
}

// ---- bubblesort: py_bubblesort と同形 (swapped 早期脱出あり) ----
void bubble(std::vector<double>& a) {
    long n = (long)a.size();
    long i = 0;
    while (i < n) {
        bool swapped = false;
        long j = 0;
        while (j < n - 1 - i) {
            if (a[j] > a[j + 1]) {
                double t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
                swapped = true;
            }
            ++j;
        }
        if (!swapped) break;
        ++i;
    }
}

std::string check_sorted(const std::vector<double>& a) {
    long long s = 0;
    for (double v : a) s += (long long)v;
    long long h = 0;
    for (double v : a) h = (h * 31 + (long long)v) % 1000000007LL;
    bool ok = true;
    for (size_t k = 0; k + 1 < a.size(); ++k)
        if (a[k] > a[k + 1]) {
            ok = false;
            break;
        }
    char buf[96];
    snprintf(buf, sizeof buf, "%lld:%lld:%d", s, h, ok ? 1 : 0);
    return buf;
}

// ---- sieve: py_sieve と同形 ----
std::string run_sieve(int n, double& ms_out) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<char> fl(n + 1, 1);
    fl[0] = 0;
    if (n >= 1) fl[1] = 0;
    int i = 2;
    while ((long long)i * i <= n) {
        if (fl[i]) {
            int start = i * i, step = i;
            for (int j = start; j <= n; j += step) fl[j] = 0;
        }
        ++i;
    }
    auto t1 = std::chrono::steady_clock::now();
    ms_out =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    long long count = 0;
    for (char c : fl) count += c;
    long long h = 0;
    for (int k = 1; k <= n; ++k)
        if (fl[k]) h = (h * 31 + k) % 1000000007LL;
    char buf[96];
    snprintf(buf, sizeof buf, "%lld:%lld:1", count, h);
    return buf;
}

long long fib_rec(long long n) {
    if (n < 2) return n;
    return fib_rec(n - 1) + fib_rec(n - 2);
}

// ---- sha256: FIPS 180-4, py_sha256_faithful と同形 ----
static inline uint32_t rotr(uint32_t x, int r) {
    return (x >> r) | (x << (32 - r));
}

std::string run_sha256(int n, double& ms_out) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    // メッセージ生成は計測外 (xyo も生成は計測外)
    std::vector<uint8_t> msg;
    msg.reserve(n + 72);
    {
        long long s = 42;
        for (int k = 0; k < n; ++k) {
            s = (s * LCG_A + LCG_B) % LCG_M;
            msg.push_back((uint8_t)(s % 256));
        }
    }
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 7; k >= 0; --k) msg.push_back((uint8_t)(bits >> (k * 8)));

    auto t0 = std::chrono::steady_clock::now();
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t W[64];
        for (int k = 0; k < 16; ++k)
            W[k] = ((uint32_t)msg[off + k * 4] << 24) |
                   ((uint32_t)msg[off + k * 4 + 1] << 16) |
                   ((uint32_t)msg[off + k * 4 + 2] << 8) |
                   (uint32_t)msg[off + k * 4 + 3];
        for (int t = 16; t < 64; ++t) {
            uint32_t s1 = rotr(W[t - 2], 17) ^ rotr(W[t - 2], 19) ^
                          (W[t - 2] >> 10);
            uint32_t s0 = rotr(W[t - 15], 7) ^ rotr(W[t - 15], 18) ^
                          (W[t - 15] >> 3);
            W[t] = W[t - 16] + s0 + W[t - 7] + s1;
        }
        uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4],
                 f = H[5], g = H[6], h = H[7];
        for (int t = 0; t < 64; ++t) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[t] + W[t];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        H[0] += a;
        H[1] += b;
        H[2] += c;
        H[3] += d;
        H[4] += e;
        H[5] += f;
        H[6] += g;
        H[7] += h;
    }
    auto t1 = std::chrono::steady_clock::now();
    ms_out =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    char buf[256];
    snprintf(buf, sizeof buf, "%u:%u:%u:%u:%u:%u:%u:%u", H[0], H[1], H[2],
             H[3], H[4], H[5], H[6], H[7]);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: duel <algo> <n> <reps>\n");
        return 2;
    }
    std::string algo = argv[1];
    long long n = std::atoll(argv[2]);
    int reps = std::atoi(argv[3]);
    std::string check;
    std::vector<double> times;
    // 1 回ウォームアップ (計測外)
    auto warmup = [&](auto fn) { fn(); };
    (void)warmup;

    if (algo == "quicksort" || algo == "mergesort" ||
        algo == "bubblesort") {
        for (int r = -1; r < reps; ++r) {
            std::vector<double> data = lcg((int)n);
            auto t0 = std::chrono::steady_clock::now();
            if (algo == "quicksort") qs(data, 0, (long)data.size() - 1);
            if (algo == "mergesort") {
                std::vector<double> aux(data.size(), 0.0);
                ms_rec(data, aux, 0, (long)data.size() - 1);
            }
            if (algo == "bubblesort") bubble(data);
            auto t1 = std::chrono::steady_clock::now();
            double ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (r >= 0) {
                times.push_back(ms);
                check = check_sorted(data);
            } else {
                check = check_sorted(data);
            }
        }
    } else if (algo == "sieve") {
        for (int r = -1; r < reps; ++r) {
            double ms = 0;
            std::string c = run_sieve((int)n, ms);
            if (r >= 0) {
                times.push_back(ms);
                check = c;
            } else {
                check = c;
            }
        }
    } else if (algo == "fib") {
        for (int r = -1; r < reps; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            long long v = fib_rec(n);
            auto t1 = std::chrono::steady_clock::now();
            if (r >= 0) {
                times.push_back(std::chrono::duration<double, std::milli>(
                                    t1 - t0)
                                    .count());
                char buf[64];
                snprintf(buf, sizeof buf, "%lld:1", v);
                check = buf;
            }
        }
    } else if (algo == "sha256") {
        for (int r = -1; r < reps; ++r) {
            double ms = 0;
            std::string c = run_sha256((int)n, ms);
            if (r >= 0) {
                times.push_back(ms);
                check = c;
            } else {
                check = c;
            }
        }
    } else if (algo == "micro_loop") {
        long long warm = 0;
        for (int r = -1; r < reps; ++r) {
            long long i = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (long long k = 0; k < n; ++k) {
                i += 1;
                opaque(i);
            }
            auto t1 = std::chrono::steady_clock::now();
            if (r >= 0) {
                times.push_back(std::chrono::duration<double, std::milli>(
                                    t1 - t0)
                                    .count());
                warm = i;
            }
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", warm);
        check = buf;
    } else if (algo == "micro_read") {
        std::vector<double> L(1000, 7.0);
        double sw = 0;
        for (int r = -1; r < reps; ++r) {
            long long i = 0;
            double s = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (long long k = 0; k < n; ++k) {
                i += 1;
                if (i > 1000) i = 1;
                s += L[i - 1];
                opaque(i);
            }
            auto t1 = std::chrono::steady_clock::now();
            if (r >= 0) {
                times.push_back(std::chrono::duration<double, std::milli>(
                                    t1 - t0)
                                    .count());
                sw = s;
            }
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", (long long)sw);
        check = buf;
    } else if (algo == "micro_write") {
        std::vector<double> L(1000, 0.0);
        double sw = 0;
        for (int r = -1; r < reps; ++r) {
            std::fill(L.begin(), L.end(), 0.0);
            long long i = 0, k = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (long long q = 0; q < n; ++q) {
                i += 1;
                if (i > 1000) i = 1;
                k += 1;
                L[i - 1] = (double)k;
                opaque(i);
            }
            auto t1 = std::chrono::steady_clock::now();
            if (r >= 0) {
                times.push_back(std::chrono::duration<double, std::milli>(
                                    t1 - t0)
                                    .count());
                double s = 0;
                for (double v : L) s += v;
                sw = s;
            }
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", (long long)sw);
        check = buf;
    } else if (algo == "micro_append") {
        long long ln = 0;
        for (int r = -1; r < reps; ++r) {
            std::vector<double> L;
            L.reserve(n);
            long long k = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (long long q = 0; q < n; ++q) {
                k += 1;
                L.push_back((double)k);
            }
            auto t1 = std::chrono::steady_clock::now();
            if (r >= 0) {
                times.push_back(std::chrono::duration<double, std::milli>(
                                    t1 - t0)
                                    .count());
                ln = (long long)L.size();
            }
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", ln);
        check = buf;
    } else {
        std::fprintf(stderr, "unknown algo: %s\n", algo.c_str());
        return 2;
    }
    std::printf("check=%s\n", check.c_str());
    std::printf("times:");
    for (double t : times) std::printf(" %.3f", t);
    std::printf("\n");
    return 0;
}
