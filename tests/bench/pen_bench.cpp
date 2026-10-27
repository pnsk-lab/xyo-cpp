// xyo-cpp: ペン描画ベンチマーク。
//
// 目的: ペン層の「ストローク投入」「フレーム表示」を分離して計測し、
// CPU ラスタ / GPU(OpenGL) ラスタ / 新バックエンドを同一ワークロードで
// 比較できるようにする。描画品質は CPU ラスタ経路の PPM を参照として
// SSIM と画素誤差で比較する。
//
// 使い方:
//   xyo-pen-bench [--workload NAME] [--frames N] [--reps N] [--scale S]
//                 [--seed N] [--shot out.ppm] [--trace file] [--list]
//   xyo-pen-bench --colorloop N [--palette M]   # 色変換のみ（1 色/回）
//   xyo-pen-bench --vmloop N [--vmcolor 0|1] [--palette M] [--vmreal]
//       # ペンブロック経路（setColor→setXY→hook→penLine）を VM 抜きで。
//       # --vmreal は実 SdlRenderer（描画バックエンド込み）で測る。
//   xyo-pen-bench --project 3D2.sb3 [--frames 30] [--scale 2] [--shot out.ppm]
//       # Startup clicks + 10 active warmup frames, then unpaced active frames.
//       # Linux x86_64 only: XYO_BENCH_SAMPLE=/tmp/pen [XYO_BENCH_SAMPLE_US=2000]
//       # writes .pcs/.maps/.json for main-thread CPU PCs, timed loop only.
//       # Sampling perturbs timing: use unsampled runs for A/B comparisons.
//   xyo-pen-bench --compare ref.ppm cand.ppm
//
// ワークロード: sparse_dots / dense_dots / scanline / tri_fill /
//               dense_alpha / opaque_overdraw / random_lines / real_trace /
//               short_strokes / wide_brush
//
// 内部内訳（flush/readback/convert/upload/present）は XYO_PENPROF=1 を
// 付けて実行すると終了時に stderr へも出る。本ベンチは penStats() の
// 差分を直接表示するので環境変数は不要。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <SDL.h>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif

#include "core/fileio.hpp"
#include "blocks/pen_api.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "render/sdl_renderer.hpp"
#include "pc_sampler.hpp"

using namespace xyo;

// 旧リビジョン（penStats 未実装）との A/B 用: -DXYO_BENCH_NO_STATS で
// 統計をゼロ固定にする。時間計測はベンチ自身の wall clock なので比較可能。
#if defined(XYO_BENCH_NO_STATS)
namespace xyo {
struct PenStats {
    uint64_t strokes = 0, rasterPx = 0, quads = 0, flushes = 0, readbacks = 0,
             convPx = 0, uploads = 0, frames = 0, records = 0;
    double strokeMs = 0, rasterMs = 0, quadMs = 0, flushMs = 0,
           readbackMs = 0, convertMs = 0, uploadMs = 0, copyMs = 0,
           drawMs = 0, presentMs = 0;
};
static inline PenStats penStats() { return PenStats{}; }
static inline void penStatsReset() {}
} // namespace xyo
#endif

namespace {

struct Stroke {
    float rgba[4] = {0, 0, 0, 1};
    double d = 1, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool dot() const { return x0 == x1 && y0 == y1; }
};

uint64_t g_rng = 0x2545F4914F6CDD1Dull;
inline uint32_t xr() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 32);
}
inline double rnd(double lo, double hi) {
    return lo + (hi - lo) * (double(xr()) / 4294967296.0);
}
inline float rnd01() { return float(xr()) / 4294967296.0f; }

void setColor(Stroke& s, float a) {
    // Scratch の HSV レンジに近い彩度の高い色（0..1 の RGB）
    s.rgba[0] = 0.15f + 0.8f * rnd01();
    s.rgba[1] = 0.15f + 0.8f * rnd01();
    s.rgba[2] = 0.15f + 0.8f * rnd01();
    s.rgba[3] = a;
}

// Scratch ステージ座標（中心原点）で x∈[-240,240], y∈[-180,180]
void genWorkload(const std::string& name, std::vector<Stroke>& out,
                 uint32_t seed) {
    g_rng = 0x2545F4914F6CDD1Dull ^ (uint64_t(seed) * 0x9E3779B97F4A7C15ull);
    out.clear();
    if (name == "sparse_dots") {
        const int n = 40000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = rnd(1, 3);
            s.x0 = s.x1 = rnd(-240, 240);
            s.y0 = s.y1 = rnd(-180, 180);
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "dense_dots") {
        const int n = 40000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 2;
            s.x0 = s.x1 = rnd(-30, 30);
            s.y0 = s.y1 = rnd(-30, 30);
            setColor(s, 0.5f);
            out.push_back(s);
        }
    } else if (name == "scanline") {
        const int n = 12000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 1;
            const double y = rnd(-180, 180);
            s.x0 = -240; s.x1 = 240; s.y0 = y; s.y1 = y;
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "tri_fill") {
        // 三角形塗りつぶし（スクラッチの tri fill engine 相当）:
        // 行ごとに走査線セグメントを引く。不透明・大量オーバードロー。
        const int rows = 900;
        out.reserve(size_t(rows) * 3);
        for (int r = 0; r < rows; ++r) {
            const double t = double(r) / (rows - 1);
            const double y = 170.0 - 340.0 * t;
            const double half = 220.0 * (1.0 - std::fabs(2.0 * t - 1.0));
            Stroke s;
            s.d = 1;
            s.x0 = -half; s.x1 = half; s.y0 = y; s.y1 = y;
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "dense_alpha") {
        const int n = 60000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 4;
            s.x0 = rnd(-60, 60); s.y0 = rnd(-45, 45);
            s.x1 = s.x0 + rnd(-8, 8); s.y1 = s.y0 + rnd(-8, 8);
            setColor(s, 0.2f);
            out.push_back(s);
        }
    } else if (name == "opaque_overdraw") {
        const int n = 60000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 6;
            s.x0 = rnd(-60, 60); s.y0 = rnd(-45, 45);
            s.x1 = s.x0 + rnd(-10, 10); s.y1 = s.y0 + rnd(-10, 10);
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "random_lines") {
        const int n = 20000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = rnd(1, 4);
            s.x0 = rnd(-240, 240); s.y0 = rnd(-180, 180);
            s.x1 = s.x0 + rnd(-40, 40); s.y1 = s.y0 + rnd(-40, 40);
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "short_strokes") {
        // Scratch の「ペンを下ろして動かす」典型: 短い連続セグメント
        const int n = 60000;
        out.reserve(n);
        double x = 0, y = 0;
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 2;
            s.x0 = x; s.y0 = y;
            x += rnd(-3, 3);
            y += rnd(-3, 3);
            x = std::clamp(x, -230.0, 230.0);
            y = std::clamp(y, -170.0, 170.0);
            s.x1 = x; s.y1 = y;
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "overdraw_heavy") {
        // 極端なオーバードロー: 100 万本の不透明ストロークを狭い領域へ重ねる
        const int n = 1000000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 6;
            s.x0 = rnd(-20, 20); s.y0 = rnd(-20, 20);
            s.x1 = s.x0 + rnd(-6, 6); s.y1 = s.y0 + rnd(-6, 6);
            setColor(s, 1.0f);
            out.push_back(s);
        }
    } else if (name == "alpha_heavy") {
        // 極端な半透明オーバードロー: 100 万本 α=0.15
        const int n = 1000000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 6;
            s.x0 = rnd(-20, 20); s.y0 = rnd(-20, 20);
            s.x1 = s.x0 + rnd(-6, 6); s.y1 = s.y0 + rnd(-6, 6);
            setColor(s, 0.15f);
            out.push_back(s);
        }
    } else if (name == "wide_brush") {
        const int n = 4000;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Stroke s;
            s.d = 40;
            s.x0 = rnd(-240, 240); s.y0 = rnd(-180, 180);
            s.x1 = s.x0 + rnd(-60, 60); s.y1 = s.y0 + rnd(-60, 60);
            setColor(s, 0.6f);
            out.push_back(s);
        }
    } else {
        std::fprintf(stderr, "unknown workload: %s\n", name.c_str());
        std::exit(2);
    }
}

bool loadTrace(const char* path, std::vector<Stroke>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        Stroke s;
        float r = 0, g = 0, b = 0, a = 1;
        if (std::sscanf(line, "%lf %lf %lf %lf %lf %f %f %f %f", &s.d,
                        &s.x0, &s.y0, &s.x1, &s.y1, &r, &g, &b, &a) == 9) {
            s.rgba[0] = r; s.rgba[1] = g; s.rgba[2] = b; s.rgba[3] = a;
            out.push_back(s);
        }
    }
    std::fclose(f);
    return true;
}

// ---------------- PPM / SSIM ----------------
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGB
};

bool readPPM(const char* path, Image& img) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    if (std::fscanf(f, "%2s", magic) != 1 || std::strcmp(magic, "P6") != 0) {
        std::fclose(f);
        return false;
    }
    // コメント・空白を飛ばしてヘッダを読む
    auto nextInt = [&](int& v) -> bool {
        int c;
        do {
            c = std::fgetc(f);
            if (c == '#') {
                while (c != '\n' && c != EOF) c = std::fgetc(f);
            }
        } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (c == EOF) return false;
        std::ungetc(c, f);
        return std::fscanf(f, "%d", &v) == 1;
    };
    if (!nextInt(w) || !nextInt(h) || !nextInt(maxv) || w <= 0 || h <= 0) {
        std::fclose(f);
        return false;
    }
    std::fgetc(f);   // ヘッダ終端の 1 バイト
    img.w = w; img.h = h;
    img.px.resize(size_t(w) * h * 3);
    const size_t got = std::fread(img.px.data(), 1, img.px.size(), f);
    std::fclose(f);
    return got == img.px.size();
}

/// グレースケール SSIM（8x8 ブロック平均）+ 画素誤差統計
int compareImages(const char* refPath, const char* candPath) {
    Image a, b;
    if (!readPPM(refPath, a)) {
        std::fprintf(stderr, "cannot read %s\n", refPath);
        return 2;
    }
    if (!readPPM(candPath, b)) {
        std::fprintf(stderr, "cannot read %s\n", candPath);
        return 2;
    }
    if (a.w != b.w || a.h != b.h) {
        std::fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", a.w, a.h, b.w,
                     b.h);
        return 2;
    }
    auto gray = [](const Image& im, int x, int y) {
        const uint8_t* p = &im.px[(size_t(y) * im.w + x) * 3];
        return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    };
    double maxErr = 0, sumErr = 0;
    size_t n = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const uint8_t* pa = &a.px[(size_t(y) * a.w + x) * 3];
            const uint8_t* pb = &b.px[(size_t(y) * b.w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                const double e = std::fabs(double(pa[c]) - double(pb[c]));
                maxErr = std::max(maxErr, e);
                sumErr += e;
                ++n;
            }
        }
    const double meanErr = n ? sumErr / n : 0;

    const double C1 = 6.5025, C2 = 58.5225;
    double ssimSum = 0;
    int blocks = 0;
    const int B = 8;
    for (int by = 0; by + B <= a.h; by += B)
        for (int bx = 0; bx + B <= a.w; bx += B) {
            double ma = 0, mb = 0;
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    ma += gray(a, bx + x, by + y);
                    mb += gray(b, bx + x, by + y);
                }
            ma /= B * B; mb /= B * B;
            double va = 0, vb = 0, cov = 0;
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    const double da = gray(a, bx + x, by + y) - ma;
                    const double db = gray(b, bx + x, by + y) - mb;
                    va += da * da; vb += db * db; cov += da * db;
                }
            va /= B * B - 1; vb /= B * B - 1; cov /= B * B - 1;
            ssimSum += ((2 * ma * mb + C1) * (2 * cov + C2)) /
                       ((ma * ma + mb * mb + C1) * (va + vb + C2));
            ++blocks;
        }
    std::printf("SSIM=%.6f meanErr=%.4f maxErr=%.1f blocks=%d\n",
                blocks ? ssimSum / blocks : 0.0, meanErr, maxErr, blocks);
    return 0;
}

void listWorkloads() {
    std::printf(
        "sparse_dots dense_dots scanline tri_fill dense_alpha "
        "opaque_overdraw random_lines short_strokes wide_brush overdraw_heavy alpha_heavy real_trace\n");
}

// Runtime::step invokes renderer->draw(). Measure that existing callback rather
// than drawing a second time, and keep these frame-level timers off stroke paths.
int benchProject(const std::string& path, int frames, int scale,
                 const std::string& shot) {
#if defined(XYO_BENCH_NO_STATS)
    std::fprintf(stderr, "--project requires penStats for active-frame detection\n");
    return 2;
#else
    std::ifstream input(pathFromUtf8(path), std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open project: %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    Runtime rt;
    const auto loaded = loadSb3File(rt, bytes.data(), bytes.size());
    if (!loaded.ok) {
        std::fprintf(stderr, "project load failed: %s\n", loaded.error.c_str());
        return 2;
    }
    struct ProjectRenderer : SdlRenderer {
        double drawMs = 0;
        uint64_t draws = 0;
        void draw() override {
            const auto start = std::chrono::steady_clock::now();
            SdlRenderer::draw();
            drawMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            ++draws;
        }
    } ren;
    if (!ren.init(rt, scale)) {
        std::fprintf(stderr, "project renderer init failed\n");
        return 2;
    }
    rt.renderer = &ren;
    rt.greenFlag();
    // Match the player's XYO_SET hook for resolution/scene parity checks.
    if (const char* spec = std::getenv("XYO_SET")) {
        std::string settings = spec;
        for (size_t begin = 0; begin <= settings.size();) {
            size_t end = settings.find(';', begin);
            std::string setting = settings.substr(begin,
                end == std::string::npos ? end : end - begin);
            begin = end == std::string::npos ? settings.size() + 1 : end + 1;
            size_t eq = setting.find('=');
            if (eq == std::string::npos) continue;
            std::string name = setting.substr(0, eq), raw = setting.substr(eq + 1);
            char* tail = nullptr;
            double number = std::strtod(raw.c_str(), &tail);
            Value value = tail != raw.c_str() && *tail == 0
                ? Value::num(number) : Value::string(SStr::fromUtf8(raw));
            for (Target* t : rt.targets)
                for (auto& [id, variable] : t->variables)
                    if (id == name || SStr::toUtf8(variable.name) == name)
                        variable.value = value;
        }
    }
    uint64_t activeThreshold = 100000;
    if (const char* value = std::getenv("XYO_BENCH_ACTIVE_STROKES"))
        activeThreshold = std::strtoull(value, nullptr, 10);
    int warmupFrames = 10;
    if (const char* value = std::getenv("XYO_BENCH_WARMUP_FRAMES"))
        warmupFrames = std::max(1, std::atoi(value));
    // Two clicks match the established 3D2 startup injection, scaled from
    // (480,360) at scale=2. Pace startup so real-time fades can finish.
    int activeWarmup = 0, startupFrames = 0;
    for (int f = 0; f < 1000 && activeWarmup < warmupFrames; ++f) {
        const auto start = std::chrono::steady_clock::now();
        if (!ren.pollEvents()) return 2;
        if (f == 20 || f == 21 || f == 60 || f == 61) {
            SDL_Event ev{};
            ev.type = (f == 20 || f == 60) ? SDL_MOUSEBUTTONDOWN
                                          : SDL_MOUSEBUTTONUP;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = 240 * scale;
            ev.button.y = 180 * scale;
            ren.handleEvent(ev);
        }
        const uint64_t before = penStats().strokes;
        rt.step();
        if (penStats().strokes - before > activeThreshold) ++activeWarmup;
        ++startupFrames;
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed < 33.0) SDL_Delay(Uint32(33.0 - elapsed));
    }
    if (activeWarmup < warmupFrames) {
        std::fprintf(stderr,
            "project startup timed out: steps=%d active_warmup=%d (need %d frames with >%llu strokes)\n",
            startupFrames, activeWarmup, warmupFrames,
            (unsigned long long)activeThreshold);
        return 2;
    }
    // Opt-in benchmark control, after driver threads/startup/warmup exist.
    // This changes only the calling thread's affinity, never another process.
    if (const char* cpuText = std::getenv("XYO_BENCH_CPU")) {
#if defined(__linux__)
        char* end = nullptr;
        const long cpu = std::strtol(cpuText, &end, 10);
        cpu_set_t allowed, selected;
        if (end == cpuText || *end || cpu < 0 || cpu >= CPU_SETSIZE ||
            sched_getaffinity(0, sizeof(allowed), &allowed) != 0 ||
            !CPU_ISSET(int(cpu), &allowed)) {
            std::fprintf(stderr, "invalid/unavailable XYO_BENCH_CPU=%s\n", cpuText);
            return 2;
        }
        CPU_ZERO(&selected);
        CPU_SET(int(cpu), &selected);
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0) {
            std::perror("sched_setaffinity");
            return 2;
        }
        std::printf("PROJECT pinned_main_cpu=%ld\n", cpu);
#else
        std::fprintf(stderr, "XYO_BENCH_CPU is only supported on Linux\n");
        return 2;
#endif
    }
    penStatsReset();
    ren.drawMs = 0;
    ren.draws = 0;
    int active = 0, steps = 0;
    const auto parBefore = rt.sequencer->parallelStats();
    double stepMs = 0;
    xyo::bench::PcSampler sampler;
    sampler.start(); // Opt-in setup/prefault cost precedes both time baselines.
    const std::clock_t cpuStart = std::clock();
    const auto start = std::chrono::steady_clock::now();
    // Bound inactive steps too: an input/scene change must not hang a run.
    while (active < frames && steps < frames + 1000) {
        if (!ren.pollEvents()) return 2;
        const uint64_t before = penStats().strokes;
        const auto stepStart = std::chrono::steady_clock::now();
        rt.step();
        stepMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stepStart).count();
        if (penStats().strokes - before > activeThreshold) ++active;
        ++steps;
    }
    const double totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const double cpuMs = 1000.0 * double(std::clock() - cpuStart) / CLOCKS_PER_SEC;
    sampler.stop(); // Keep teardown and all profile writes outside bench timers.
    sampler.write();
    const PenStats stats = penStats();
    std::printf("PROJECT process_cpu=%.3fms (all process threads)\n", cpuMs);
    std::printf(
        "PROJECT path=%s startup_steps=%d warmup_active=%d active=%d steps=%d draws=%llu\n",
        path.c_str(), startupFrames, activeWarmup, active, steps,
        (unsigned long long)ren.draws);
    std::printf(
        "PROJECT total=%.3fms step_including_draw=%.3fms step_excluding_draw=%.3fms draw=%.3fms active_fps=%.3f\n",
        totalMs, stepMs, stepMs - ren.drawMs, ren.drawMs,
        totalMs > 0 ? active * 1000.0 / totalMs : 0.0);
    std::printf(
        "PROJECT strokes=%llu quads=%llu frames=%llu flushes=%llu readbacks=%llu uploads=%llu\n",
        (unsigned long long)stats.strokes, (unsigned long long)stats.quads,
        (unsigned long long)stats.frames, (unsigned long long)stats.flushes,
        (unsigned long long)stats.readbacks, (unsigned long long)stats.uploads);
    const auto parAfter = rt.sequencer->parallelStats();
    std::printf("PROJECT par_batches=%llu dispatches=%llu threads=%llu groups=%llu\n",
        (unsigned long long)(parAfter.batches - parBefore.batches),
        (unsigned long long)(parAfter.dispatches - parBefore.dispatches),
        (unsigned long long)(parAfter.threads - parBefore.threads),
        (unsigned long long)(parAfter.groups - parBefore.groups));
    // Readback/screenshot cost is deliberately outside the measured interval.
    // Optional presentation-only normalization of a wall-clock monitor. Do not
    // change the underlying Scratch variable or execute another Runtime step:
    // the final scene/pen buffer must remain exactly as measured. A second
    // renderer draw updates only the snapshot, after all timing/stats capture.
    if (!shot.empty()) {
        if (const char* name = std::getenv("XYO_BENCH_SHOT_MONITOR")) {
            unsigned matches = 0;
            for (auto& [id, monitor] : rt.monitorState.records) {
                auto it = monitor.params.find("VARIABLE");
                if (monitor.visible && monitor.opcode == "data_variable" &&
                    (id == name || (it != monitor.params.end() &&
                                   SStr::toUtf8(it->second) == name))) {
                    monitor.value = Value::num(0);
                    ++matches;
                }
            }
            std::printf("PROJECT snapshot_monitor_matches=%u\n", matches);
            if (matches != 1) {
                std::fprintf(stderr, "snapshot monitor must match exactly one visible variable\n");
                return 2;
            }
            ren.SdlRenderer::draw();
        }
        if (!ren.savePPM(shot.c_str())) return 2;
    }
    if (active != frames) {
        std::fprintf(stderr, "project timed interval exhausted inactive-step bound\n");
        return 2;
    }
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string workload = "sparse_dots";
    std::string trace, shot, project;
    size_t limit = 0;
    int frames = 30, reps = 1, scale = 2, warmupMs = 0;
    uint32_t seed = 1;
    bool interleave = false;
    long colorLoop = 0;
    long vmLoop = 0, vmFence = 0;
    int vmColor = 1, vmReal = 0, vmMove = 1;
    int palette = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto val = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (!std::strcmp(a, "--project")) project = val();
        else if (!std::strcmp(a, "--workload")) workload = val();
        else if (!std::strcmp(a, "--frames")) frames = std::atoi(val());
        else if (!std::strcmp(a, "--reps")) reps = std::atoi(val());
        else if (!std::strcmp(a, "--scale")) scale = std::atoi(val());
        else if (!std::strcmp(a, "--seed"))
            seed = uint32_t(std::strtoul(val(), nullptr, 10));
        else if (!std::strcmp(a, "--shot")) shot = val();
        else if (!std::strcmp(a, "--trace")) trace = val();
        else if (!std::strcmp(a, "--limit"))
            limit = size_t(std::strtoul(val(), nullptr, 10));
        else if (!std::strcmp(a, "--interleave")) interleave = true;
        else if (!std::strcmp(a, "--colorloop"))
            colorLoop = std::strtol(val(), nullptr, 10);
        else if (!std::strcmp(a, "--vmloop"))
            vmLoop = std::strtol(val(), nullptr, 10);
        else if (!std::strcmp(a, "--vmcolor"))
            vmColor = std::atoi(val());
        else if (!std::strcmp(a, "--vmmove"))
            vmMove = std::atoi(val());
        else if (!std::strcmp(a, "--vmreal"))
            vmReal = 1;
        else if (!std::strcmp(a, "--vmfence"))
            vmFence = std::strtol(val(), nullptr, 10);
        else if (!std::strcmp(a, "--palette"))
            palette = std::atoi(val());
        else if (!std::strcmp(a, "--warmup-ms")) warmupMs = std::atoi(val());
        else if (!std::strcmp(a, "--list")) { listWorkloads(); return 0; }
        else if (!std::strcmp(a, "--compare")) {
            const char* p1 = val();
            const char* p2 = val();
            return compareImages(p1, p2);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a);
            return 2;
        }
    }
    if (frames < 1) frames = 1;
    if (reps < 1) reps = 1;
    if (!project.empty()) return benchProject(project, frames, scale, shot);

    // ペンブロック経路: setPenColorToColor → setXY → penOnTargetMoved →
    // syncPenColor → penLine。VM（ブロック実行）と描画バックエンドを
    // 除いた「ペン 1 ストローク分の CPU」を決定論的に測る。--vmreal は
    // 実 SdlRenderer を使い、フェンス・GL レコード追加まで含める。
    if (vmFence > 0) {
        // getFencedPosition 単体（実レンダラ）。--vmfence N
        static const char kProj[] = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
     "currentCostume": 0, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {},
     "costumes": [{"assetId":"a","name":"a","dataFormat":"svg",
       "md5ext":"a.svg","bitmapResolution":1,"rotationCenterX":48,
       "rotationCenterY":48}],
     "sounds": [], "currentCostume": 0, "x": 0, "y": 0, "size": 100,
     "direction": 90, "rotationStyle": "all around", "visible": true,
     "layerOrder": 1}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";
        Runtime rt;
        if (!loadSb3(rt, kProj, nullptr).ok) return 2;
        static const char kSvg[] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" "
            "height=\"96\"><rect width=\"96\" height=\"96\" "
            "fill=\"#ff0000\"/></svg>";
        Target* t = rt.targets.size() > 1 ? rt.targets[1] : nullptr;
        if (!t) return 2;
        t->sprite->costumes[0].data.assign(kSvg, kSvg + sizeof(kSvg) - 1);
        auto* ren = new SdlRenderer();
        if (!ren->init(rt, 1)) return 2;
        rt.renderer = ren;
        const int id = t->drawableID;
        const auto f0 = std::chrono::steady_clock::now();
        double x = 0, y = 0;
        // 実プロジェクト同様、1px ずつ右へ動かし端でクランプさせる
        for (long i = 0; i < vmFence; ++i) {
            x = -240.0 + double(i % 2000) * 0.5;
            ren->getFencedPosition(id, x, y, true);
        }
        const auto f1 = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(f1 - f0).count();
        std::printf("FENCE n=%ld total=%.2fms per_call=%.2fns\n", vmFence,
                    ns / 1e6, ns / double(vmFence));
        return 0;
    }

    if (vmLoop > 0) {
        struct FakeRen : RendererAPI {
            long lines = 0;
            int createPenSkin() override { return 7; }
            void penLine(int, const float*, double, double, double, double,
                         double) override { ++lines; }
            void penPoint(int, const float*, double, double, double) override {}
        } fake;
        Runtime rt;
        Target* tp = nullptr;
        if (vmReal) {
            static const char kProj[] = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
     "currentCostume": 0, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {},
     "costumes": [{"assetId":"a","name":"a","dataFormat":"svg",
       "md5ext":"a.svg","bitmapResolution":1,"rotationCenterX":48,
       "rotationCenterY":48}],
     "sounds": [], "currentCostume": 0, "x": -240, "y": 0, "size": 100,
     "direction": 90, "rotationStyle": "all around", "visible": true,
     "layerOrder": 1}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";
            if (!loadSb3(rt, kProj, nullptr).ok) {
                std::fprintf(stderr, "project load failed\n");
                return 2;
            }
            static const char kSvg[] =
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" "
                "height=\"96\"><rect width=\"96\" height=\"96\" "
                "fill=\"#ff0000\"/></svg>";
            tp = rt.targets.size() > 1 ? rt.targets[1] : nullptr;
            if (!tp) return 2;
            tp->sprite->costumes[0].data.assign(kSvg,
                                               kSvg + sizeof(kSvg) - 1);
            auto* realRen = new SdlRenderer();   // ベンチ終了まで生かす
            if (!realRen->init(rt, 1)) {
                std::fprintf(stderr, "renderer init failed\n");
                return 2;
            }
            rt.renderer = realRen;
        } else {
            rt.renderer = &fake;
            tp = new Target(&rt);
            tp->visible = true;
            tp->x = -240;
            tp->y = 0;
        }
        Target& t = *tp;
        penrt::down(&t, &rt);
        std::vector<Value> pal;
        if (palette > 0) {
            uint32_t x = 12345u;
            for (int i = 0; i < palette; ++i) {
                x = x * 1664525u + 1013904223u;
                pal.push_back(Value::num(double(x >> 8)));
            }
        }
        const auto v0 = std::chrono::steady_clock::now();
        double x = -240;
        long lines = 0;
        for (long i = 0; i < vmLoop; ++i) {
            if (vmColor) {
                if (!pal.empty())
                    penrt::setColorToColor(&t, pal[size_t(i) % pal.size()]);
                else
                    penrt::setColorToColor(
                        &t, Value::num(double(i * 7919 % 16777216)));
            }
            if (vmMove) {
                x += 1;
                t.setXY(x, 0);
            }
        }
        const auto v1 = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(v1 - v0).count();
        if (!vmReal) lines = fake.lines;
        std::printf(
            "VMLOOP n=%ld color=%d move=%d real=%d palette=%d total=%.2fms "
            "per_iter=%.2fns lines=%ld x=%.0f\n",
            vmLoop, vmColor, vmMove, vmReal, palette, ns / 1e6,
            ns / double(vmLoop), lines, x);
        return 0;
    }

    // 色変換（pen_setPenColorToColor の rgbToHsv→hsvToRgb 往復）だけを
    // 回すマイクロベンチ。palette>0 で同じ色を巡回（往復キャッシュの
    // ヒット率を模す）。
    if (colorLoop > 0) {
        PenState ps;
        std::vector<Value> colors;
        colors.reserve(palette > 0 ? size_t(palette) : 1);
        if (palette > 0) {
            uint32_t x = 12345u;
            for (int i = 0; i < palette; ++i) {
                x = x * 1664525u + 1013904223u;
                colors.push_back(Value::num(double(x >> 8)));
            }
        }
        const auto c0 = std::chrono::steady_clock::now();
        for (long i = 0; i < colorLoop; ++i) {
            Value v;
            if (palette > 0)
                v = colors[size_t(i) % colors.size()];
            else
                v = Value::num(double(i * 7919 % 16777216));
            penrt::setColorToColorState(ps, v);
        }
        const auto c1 = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(c1 - c0).count();
        std::printf(
            "COLORLOOP n=%ld palette=%d total=%.2fms per_color=%.2fns "
            "c=%.3f\n",
            colorLoop, palette, ns / 1e6, ns / double(colorLoop), ps.color);
        return 0;
    }

    std::vector<Stroke> strokes;
    if (workload == "real_trace") {
        if (trace.empty() || !loadTrace(trace.c_str(), strokes)) {
            std::fprintf(stderr, "real_trace requires --trace <file>\n");
            return 2;
        }
    } else {
        genWorkload(workload, strokes, seed);
    }

    if (limit && strokes.size() > limit) strokes.resize(limit);

    Runtime rt;
    SdlRenderer ren;
    if (!ren.init(rt, scale)) {
        std::fprintf(stderr, "renderer init failed\n");
        return 2;
    }
    const int skin = ren.createPenSkin();

    // ウォームアップ（シェーダコンパイル・初回確保を計測から除く）
    {
        static const float kWhite[4] = {1, 1, 1, 1};
        ren.penClear(skin);
        ren.penPoint(skin, kWhite, 2, 0, 0);
        ren.draw();
    }

    // GPU をブースト状態へ持ち上げる（短い計測だとアイドル clock のまま
    // 測定してしまう）。指定時間ぶん同じワークロードを回してから計測する。
    if (warmupMs > 0) {
        const auto w0 = std::chrono::steady_clock::now();
        do {
            ren.penClear(skin);
            for (const Stroke& s : strokes) {
                if (s.dot()) ren.penPoint(skin, s.rgba, s.d, s.x0, s.y0);
                else
                    ren.penLine(skin, s.rgba, s.d, s.x0, s.y0, s.x1, s.y1);
            }
            for (int f = 0; f < 8; ++f) ren.draw();
        } while (std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - w0).count() <
                 warmupMs);
    }
    double feedMs = 0, drawMs = 0;
    PenStats before = penStats();
    penStatsReset();

    const auto t0 = std::chrono::steady_clock::now();
    if (interleave) {
        // 実プロジェクト相当: 毎フレーム「ストローク投入 → draw()」を繰り返す。
        // 表示経路（readback/変換/upload/present）が毎フレーム走る。
        const size_t n = strokes.size();
        for (int f = 0; f < frames; ++f) {
            const size_t lo = n * size_t(f) / size_t(frames);
            const size_t hi = n * size_t(f + 1) / size_t(frames);
            const auto feedStart = std::chrono::steady_clock::now();
            ren.penClear(skin);
            for (size_t i = lo; i < hi; ++i) {
                const Stroke& s = strokes[i];
                if (s.dot())
                    ren.penPoint(skin, s.rgba, s.d, s.x0, s.y0);
                else
                    ren.penLine(skin, s.rgba, s.d, s.x0, s.y0, s.x1, s.y1);
            }
            const auto drawStart = std::chrono::steady_clock::now();
            feedMs += std::chrono::duration<double, std::milli>(
                drawStart - feedStart).count();
            ren.draw();
            drawMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - drawStart).count();
        }
    } else {
        const auto tf0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) {
            ren.penClear(skin);
            for (const Stroke& s : strokes) {
                if (s.dot())
                    ren.penPoint(skin, s.rgba, s.d, s.x0, s.y0);
                else
                    ren.penLine(skin, s.rgba, s.d, s.x0, s.y0, s.x1, s.y1);
            }
        }
        feedMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - tf0).count();
        const auto td0 = std::chrono::steady_clock::now();
        for (int f = 0; f < frames; ++f) ren.draw();
        drawMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - td0).count();
    }
    const auto t2 = std::chrono::steady_clock::now();
    (void)before;

    const PenStats st = penStats();
    const size_t n = strokes.size() * size_t(reps);
    const double totalMs = std::chrono::duration<double, std::milli>(t2 - t0)
                               .count();
    std::printf(
        "BENCH workload=%s backend=%s strokes=%zu frames=%d reps=%d%s\n",
        workload.c_str(),
        st.quads > 0 || st.flushes > 0 ? "gl" : "cpu", n, frames, reps,
        interleave ? " interleave" : "");
    std::printf(
        "BENCH feed=%.1fms draw=%.1fms total=%.1fms frame=%.2fms "
        "stroke_rate=%.0f/s fps=%.1f\n",
        feedMs, drawMs, totalMs, totalMs / frames,
        totalMs > 0 ? n / (totalMs / 1000.0) : 0.0,
        totalMs > 0 ? frames / (totalMs / 1000.0) : 0.0);
    std::printf(
        "BENCH stages stroke=%.1f raster=%.1f quad=%.1f flush=%.1f "
        "readback=%.1f convert=%.1f upload=%.1f copy=%.1f present=%.1f\n",
        st.strokeMs, st.rasterMs, st.quadMs, st.flushMs, st.readbackMs,
        st.convertMs, st.uploadMs, st.copyMs, st.presentMs);
    std::printf(
        "BENCH counts rasterPx=%llu quads=%llu flushes=%llu readbacks=%llu "
        "convPx=%llu uploads=%llu\n",
        (unsigned long long)st.rasterPx, (unsigned long long)st.quads,
        (unsigned long long)st.flushes, (unsigned long long)st.readbacks,
        (unsigned long long)st.convPx, (unsigned long long)st.uploads);
    if (!shot.empty()) {
        if (!ren.savePPM(shot.c_str()))
            std::fprintf(stderr, "savePPM failed: %s\n", shot.c_str());
        else
            std::printf("BENCH shot=%s\n", shot.c_str());
    }
    return 0;
}
