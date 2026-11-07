// xyo-cpp: ランチャー GUI（SDL2）。CLI を使えないユーザー向けに
// 「プロジェクトを選んで実行」と「AOT 単体実行ファイルの書き出し」を
// マウス/キーだけで完結させる。
//
//  - `xyo`（引数なし）または `xyo gui [dir]` で起動
//  - プロジェクトはステージの実サムネイル付きカードグリッドで表示する
//    （背景 + 表示中スプライトを sb3 から遅延デコードし、1 フレーム 1 枚ずつ生成）
//  - クリックで選択、ダブルクリック or Enter で実行、ディレクトリは開く
//  - `/` または Ctrl+F、検索欄クリックでインクリメンタル絞り込み
//  - [開く] で再生、[編集] で内蔵エディタ、[AOT 実行]/[AOT 書き出し] は
//    事前コンパイル経路（JIT 無しビルドでは書き出し不可）
//  - フォントが無い環境でも操作できるよう、文字は矩形フォールバック付き
//
// 自動検証用: XYO_GUI_ACTIONS="down,enter,export" をカンマ区切りで
// 1 フレームに 1 つ実行する（down/up/home/end/enter/back/export/
// aotrun/run/quit）。XYO_GUI_DEBUG=1 で選択・書き出しを stderr に出す。
// XYO_GUI_EXIT_FRAMES=N で N フレーム後に終了、XYO_GUI_SHOT=path.ppm で
// 最終フレームを保存する（レイアウト回帰用）。
#include "gui.hpp"

#ifdef XYO_HAVE_SDL2

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif
#include <string>
#include <unordered_map>
#include <vector>

#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"   // utf8ToU16
#include "render/text.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "model/variable.hpp"
#include "../../third_party/stb_image.h"
#include "../../third_party/nanosvg.h"
#include "../../third_party/nanosvgrast.h"
#include "editor.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#include "jit/jit.hpp"
#endif

/// main.cpp の再生経路（SDL ウィンドウで実行）。
/// aotObjPath は事前コンパイル済みオブジェクト（nullptr 可）。
int xyoShellPlay(const char* path, const char* aotObjPath, int scale,
                 bool turbo, int fps, int gpuMode);

namespace xyo {
namespace {

namespace fs = std::filesystem;

struct Entry {
    std::string name;   // 表示名
    std::string path;   // フルパス（".." は親）
    bool isDir = false;
    uintmax_t size = 0;
    long long mtime = 0;
};

struct NavState {
    std::string dir;
    std::string selection;
    int scrollRow = 0;
};

std::string recentHistoryPath();

struct GuiRunOptions {
    int scale = 2;
    int fps = -1;       // Project-defined FPS (normally 30)
    int jitMode = 0;    // 0 = HotSpot, 1 = Eager, 2 = interpreter
    int lazyMode = 0;   // 0 = auto, 1 = on, 2 = off
    int gpuMode = -1;   // -1 = normal, 0 = ray, 1 = path
    bool turbo = false;
    bool danger = false;
};

std::string settingsFilePath() {
    const std::string history = recentHistoryPath();
    if (history.empty()) return {};
    return (fs::path(history).parent_path() / "gui-settings.txt").string();
}

GuiRunOptions readRunOptions() {
    GuiRunOptions out;
    std::ifstream file(settingsFilePath());
    std::string line;
    while (std::getline(file, line)) {
        const size_t sep = line.find('=');
        if (sep == std::string::npos) continue;
        const std::string key = line.substr(0, sep);
        const std::string value = line.substr(sep + 1);
        const auto number = [&]() -> int {
            if (value.empty()) return -9999;
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != 0 ||
                n < -9999 || n > 9999) return -9999;
            return int(n);
        };
        const int n = number();
        if (key == "scale" && n >= 1 && n <= 4)
            out.scale = n;
        else if (key == "fps" &&
                 (n == -1 || n == 30 || n == 60 || n == 120))
            out.fps = n;
        else if (key == "jit" && n >= 0 && n <= 2)
            out.jitMode = n;
        else if (key == "lazy" && n >= 0 && n <= 2)
            out.lazyMode = n;
        else if (key == "gpu" && n >= -1 && n <= 1)
            out.gpuMode = n;
        else if (key == "turbo" && (n == 0 || n == 1))
            out.turbo = n == 1;
        // Do not persist "danger" on purpose; it always starts disabled.
    }
    return out;
}

void writeRunOptions(const GuiRunOptions& o) {
    const std::string file = settingsFilePath();
    if (file.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    if (ec) return;
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << "scale=" << o.scale << '\n'
            << "fps=" << o.fps << '\n'
            << "jit=" << o.jitMode << '\n'
            << "lazy=" << o.lazyMode << '\n'
            << "gpu=" << o.gpuMode << '\n'
            << "turbo=" << int(o.turbo) << '\n';
        if (!out) return;
    }
    fs::rename(tmp, file, ec);
#ifdef _WIN32
    if (ec) {
        fs::remove(file, ec);
        ec.clear();
        fs::rename(tmp, file, ec);
    }
#endif
}


struct Tex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

struct Thumb {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    long long mtime = -1;
    bool failed = false;
    bool requested = false;   // 生成をワーカーへ依頼済み
};

struct Color {
    uint8_t r, g, b, a;
};

// Launcher: quiet workspace surfaces and a high-contrast navigation rail.
constexpr Color kBg{0xF4, 0xF6, 0xFB, 255};
constexpr Color kPanel{0xFF, 0xFF, 0xFF, 255};
constexpr Color kFg{0x57, 0x5E, 0x75, 255};
constexpr Color kStrong{0x2B, 0x32, 0x47, 255};
constexpr Color kDim{0x86, 0x91, 0xA7, 255};
constexpr Color kMuted{0xA8, 0xB1, 0xC2, 255};
constexpr Color kBorder{0x00, 0x00, 0x00, 0x26};
constexpr Color kField{0xF1, 0xF4, 0xF9, 255};
constexpr Color kAccent{0x3A, 0x78, 0xD8, 255};
constexpr Color kAccentHot{0x29, 0x64, 0xBF, 255};
constexpr Color kFolder{0xF2, 0xC4, 0x6D, 255};
constexpr Color kFolderDark{0xE0, 0xAE, 0x4F, 255};
constexpr Color kFolderLight{0xF8, 0xD9, 0x96, 255};
constexpr Color kGreen{0x3F, 0xB2, 0x6B, 255};
constexpr Color kWarn{0xE0, 0x5B, 0x52, 255};
constexpr Color kShadow{0x57, 0x5E, 0x75, 0x15};
constexpr Color kSidebar{0xED, 0xF0, 0xF6, 255};
constexpr Color kSidebarHover{0xDB, 0xE2, 0xF3, 255};
constexpr Color kSidebarText{0x2E, 0x38, 0x50, 255};
constexpr Color kSidebarMuted{0x7A, 0x85, 0x9D, 255};

bool inside(const SDL_Rect& r, int x, int y) {
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < r.x + r.w &&
           y >= r.y && y < r.y + r.h;
}

std::string userHome() {
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE"); home && *home)
        return home;
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    return drive && path ? std::string(drive) + path : std::string();
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return home;
    // Applications launched outside a shell can lack HOME.
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir)
        return user->pw_dir;
    return {};
#endif
}

std::string recentHistoryPath() {
    const char* state = std::getenv("XDG_STATE_HOME");
#ifdef _WIN32
    if (!state || !*state) state = std::getenv("APPDATA");
#endif
    fs::path base;
    if (state && *state) base = state;
    else if (!userHome().empty()) {
#ifdef __APPLE__
        base = fs::path(userHome()) / "Library" / "Application Support";
#else
        base = fs::path(userHome()) / ".local" / "state";
#endif
    }
    if (base.empty()) return {};
    return (base / "xyo" / "recent-projects.txt").string();
}

std::string sampleDirectory() {
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    if (ec) return {};
    // A source checkout works regardless of whether xyo is launched from
    // the root or its build directory.
    for (int i = 0; i < 3; ++i) {
        if (fs::is_directory(here / "examples", ec))
            return (here / "examples").string();
        if (!here.has_parent_path()) break;
        here = here.parent_path();
    }
    return {};
}

bool hasProject(const std::string& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext == ".sb3" || ext == ".sb2") return true;
    }
    return false;
}

/// 表示幅に収まるよう、左側を省略記号で詰める。
std::string ellipsizeLeft(std::string s, size_t keep) {
    if (keep == 0 || s.size() <= keep) return s;
    std::string out = "...";
    out += s.substr(s.size() - keep);
    return out;
}

void popUtf8(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.resize(i);
}

std::string lowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
        out.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

std::string humanSize(uintmax_t n) {
    char buf[64];
    if (n >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      double(n) / (1024.0 * 1024.0));
    else if (n >= 1024)
        std::snprintf(buf, sizeof(buf), "%.0f KB", double(n) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    return buf;
}

std::string formatDate(long long t) {
    if (t <= 0) return {};
    std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

/// コスチューム（PNG / SVG）を RGBA8 へデコードする。サムネイル用。
bool decodeCostumeRGBA(const Costume& c, std::vector<uint8_t>& out, int& w,
                       int& h, double* logicalW = nullptr,
                       double* logicalH = nullptr, int maxDim = 640) {
    if (c.data.empty()) return false;
    if (c.dataFormat == "svg" || c.dataFormat == "svg+xml") {
        std::string xml(reinterpret_cast<const char*>(c.data.data()),
                        c.data.size());
        NSVGimage* img = nsvgParse(xml.data(), "px", 96.0f);
        if (!img) return false;
        const float longest = std::max(img->width, img->height);
        if (!(longest > 0.f) || !std::isfinite(longest)) {
            nsvgDelete(img);
            return false;
        }
        const float scale =
            std::min(1.0f, float(maxDim) / longest);
        if (logicalW) *logicalW = double(img->width);
        if (logicalH) *logicalH = double(img->height);
        w = int(std::ceil(img->width * scale));
        h = int(std::ceil(img->height * scale));
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            nsvgDelete(img);
            return false;
        }
        out.assign(size_t(w) * size_t(h) * 4, 0);
        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (!rast) {
            nsvgDelete(img);
            return false;
        }
        nsvgRasterize(rast, img, 0, 0, scale, out.data(), w, h, w * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(img);
        return true;
    }
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(
        c.data.data(), int(c.data.size()), &w, &h, &comp, 4);
    if (!px) return false;
    out.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    const double br = c.bitmapResolution > 0 ? c.bitmapResolution : 1.0;
    if (logicalW) *logicalW = double(w) / br;
    if (logicalH) *logicalH = double(h) / br;
    return w > 0 && h > 0;
}


// ---------------------------------------------------------------------------
// サムネイル生成（ワーカースレッド + ディスクキャッシュ）
//
// Runtime の構築と SVG のラスタ化で 1 枚 2〜3ms かかるため UI スレッドでは
// 生成しない。パス・更新時刻・サイズをキーにディスクへキャッシュし、
// 2 回目以降は数十マイクロ秒で済む。
// ---------------------------------------------------------------------------

struct ThumbJob {
    std::string path;
    long long mtime = 0;
    uintmax_t size = 0;
    int w = 0, h = 0;
};

struct ThumbOut {
    std::string path;
    long long mtime = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok = false;
};

uint64_t fnv1aHash(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string thumbCacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string base;
    if (xdg && *xdg)
        base = xdg;
    else if (home && *home)
        base = std::string(home) + "/.cache";
    if (base.empty()) return {};
    const std::string dir = base + "/xyo/thumbs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? std::string() : dir;
}

std::string thumbCacheFile(const std::string& dir, const ThumbJob& j) {
    if (dir.empty()) return {};
    char key[512];
    std::snprintf(key, sizeof(key), "%s|%lld|%llu|%d|%d", j.path.c_str(),
                  (long long)j.mtime, (unsigned long long)j.size, j.w, j.h);
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llx.xyo-thumb",
                  (unsigned long long)fnv1aHash(key));
    return dir + name;
}

bool readThumbCache(const std::string& file, std::vector<uint8_t>& out, int& w,
                    int& h) {
    if (file.empty()) return false;
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    int32_t ww = 0, hh = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && magic[0] == 'X' &&
              magic[1] == 'Y' && magic[2] == 'T' && magic[3] == 'H' &&
              std::fread(&ww, 4, 1, f) == 1 && std::fread(&hh, 4, 1, f) == 1 &&
              ww > 0 && hh > 0 && ww <= 4096 && hh <= 4096;
    if (ok) {
        out.resize(size_t(ww) * size_t(hh) * 4);
        ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        if (ok) {
            w = ww;
            h = hh;
        }
    }
    std::fclose(f);
    return ok;
}

void writeThumbCache(const std::string& file, const std::vector<uint8_t>& rgba,
                     int w, int h) {
    if (file.empty()) return;
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) return;
    int32_t ww = w, hh = h;
    std::fwrite("XYTH", 1, 4, f);
    std::fwrite(&ww, 4, 1, f);
    std::fwrite(&hh, 4, 1, f);
    std::fwrite(rgba.data(), 1, rgba.size(), f);
    std::fclose(f);
}

/// アルファ合成しながら拡大縮小コピーする（バイリニア）。
void blendSpriteScaled(std::vector<uint8_t>& dst, int dw, int dh,
                       const std::vector<uint8_t>& src, int sw, int sh,
                       double dx, double dy, double dwid, double dhei) {
    if (sw <= 0 || sh <= 0 || dwid <= 0 || dhei <= 0) return;
    const int x0 = std::max(0, int(std::floor(dx)));
    const int y0 = std::max(0, int(std::floor(dy)));
    const int x1 = std::min(dw, int(std::ceil(dx + dwid)));
    const int y1 = std::min(dh, int(std::ceil(dy + dhei)));
    for (int y = y0; y < y1; ++y) {
        const double v = (double(y) + 0.5 - dy) / dhei * double(sh) - 0.5;
        const int vy0 = int(std::floor(v));
        const double ty = v - vy0;
        for (int x = x0; x < x1; ++x) {
            const double u = (double(x) + 0.5 - dx) / dwid * double(sw) - 0.5;
            const int ux0 = int(std::floor(u));
            const double tx = u - ux0;
            double acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                const int sy = std::clamp(vy0 + j, 0, sh - 1);
                const double wy = j ? ty : 1.0 - ty;
                for (int i = 0; i < 2; ++i) {
                    const int sx = std::clamp(ux0 + i, 0, sw - 1);
                    const double wx = i ? tx : 1.0 - tx;
                    const double wgt = wx * wy;
                    const uint8_t* px = &src[(size_t(sy) * size_t(sw) +
                                              size_t(sx)) * 4];
                    acc[0] += px[0] * wgt;
                    acc[1] += px[1] * wgt;
                    acc[2] += px[2] * wgt;
                    acc[3] += px[3] * wgt;
                }
            }
            const int a = std::clamp(int(std::lround(acc[3])), 0, 255);
            if (a == 0) continue;
            uint8_t* q = &dst[(size_t(y) * size_t(dw) + size_t(x)) * 4];
            if (a >= 255) {
                q[0] = uint8_t(std::clamp(int(std::lround(acc[0])), 0, 255));
                q[1] = uint8_t(std::clamp(int(std::lround(acc[1])), 0, 255));
                q[2] = uint8_t(std::clamp(int(std::lround(acc[2])), 0, 255));
                q[3] = 255;
                continue;
            }
            const int ia = 255 - a;
            q[0] = uint8_t(std::clamp(
                int(std::lround(acc[0] + q[0] * ia / 255.0)), 0, 255));
            q[1] = uint8_t(std::clamp(
                int(std::lround(acc[1] + q[1] * ia / 255.0)), 0, 255));
            q[2] = uint8_t(std::clamp(
                int(std::lround(acc[2] + q[2] * ia / 255.0)), 0, 255));
            q[3] = 255;
        }
    }
}

/// sb3 を読み、背景とスプライトを合成した RGBA を返す（ワーカー用、SDL 非依存）。
bool renderThumbRGBA(const std::string& path, int tw, int th,
                     std::vector<uint8_t>& out) {
    std::vector<uint8_t> data;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        data.resize(size_t(n));
        if (std::fread(data.data(), 1, data.size(), f) != data.size())
            data.clear();
    }
    std::fclose(f);
    if (data.empty()) return false;

    out.assign(size_t(tw) * size_t(th) * 4, 0xF2);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;

    Runtime rt;
    if (!loadSb3File(rt, data.data(), data.size()).ok) return false;
    const double stageW = std::max(1, rt.stageWidth);
    const double stageH = std::max(1, rt.stageHeight);
    const double scale = std::min(double(tw) / stageW, double(th) / stageH);
    const double ox = (tw - stageW * scale) / 2.0;
    const double oy = (th - stageH * scale) / 2.0;

    Target* stage = nullptr;
    for (Target* tp : rt.targets)
        if (tp && tp->isStage) {
            stage = tp;
            break;
        }
    if (!stage || !stage->sprite) return false;

    bool drew = false;
    if (!stage->sprite->costumes.empty()) {
        const int ci = std::clamp(stage->currentCostume, 0,
                                  int(stage->sprite->costumes.size()) - 1);
        std::vector<uint8_t> rgba;
        int iw = 0, ih = 0;
        double lw = 0, lh = 0;
        if (decodeCostumeRGBA(stage->sprite->costumes[size_t(ci)], rgba, iw,
                              ih, &lw, &lh, std::max(tw, th) * 2)) {
            blendSpriteScaled(out, tw, th, rgba, iw, ih, ox, oy,
                              stageW * scale, stageH * scale);
            drew = true;
        }
    }
    int drawn = 0;
    for (Target* tp : rt.targets) {
        if (!tp || tp->isStage || !tp->visible || !tp->sprite ||
            tp->sprite->costumes.empty())
            continue;
        if (drawn++ >= 10) break;
        const int ci = std::clamp(tp->currentCostume, 0,
                                  int(tp->sprite->costumes.size()) - 1);
        std::vector<uint8_t> rgba;
        int iw = 0, ih = 0;
        double lw = 0, lh = 0;
        if (!decodeCostumeRGBA(tp->sprite->costumes[size_t(ci)], rgba, iw, ih,
                               &lw, &lh, std::max(tw, th) * 2))
            continue;
        const double sz = (tp->size > 0 ? tp->size : 100.0) / 100.0;
        const double wpx = std::max(1.0, lw * sz * scale);
        const double hpx = std::max(1.0, lh * sz * scale);
        // Scratch 座標: y は上向き、原点はステージ中央。
        const double cx = ox + (stageW * 0.5 + tp->x) * scale;
        const double cy = oy + (stageH * 0.5 - tp->y) * scale;
        if (wpx < tw * 4 && hpx < th * 4)
            blendSpriteScaled(out, tw, th, rgba, iw, ih, cx - wpx / 2,
                              cy - hpx / 2, wpx, hpx);
        drew = true;
    }
    return drew;
}

class Launcher {
public:
    bool open();
    void close();
    /// 1 フレーム描画してイベントを処理。
    /// 0 = 継続 / 1 = 再生 / 2 = AOT 再生 / 3 = 終了 / 4 = 編集
    int step();
    const std::string& pendingPath() const { return pending_; }
    bool pendingAot() const { return pendingAot_; }
    bool pendingEdit() const { return pendingEdit_; }
    GuiRunOptions options() const { return options_; }
    void setOptions(GuiRunOptions opts) { options_ = opts; }
    void setStatus(const std::string& s, bool err);
    void openDir(const std::string& d) { navigatePath(d, false); }
    void setActions(std::vector<std::string> a) {
        actions_ = std::move(a);
        actIdx_ = 0;
    }
    /// 検証用: ループ終了時にウィンドウ全体を PPM で保存する。
    void setShot(const std::string& p) { shotPath_ = p; }
    /// 現在のフレームバッファを書き出す（終了直前の状態を残す）。
    void captureNow();

private:
    void loadDir(const std::string& dir);
    bool navigatePath(const std::string& value, bool record = true);
    void goBack();
    void goForward();
    NavState currentNav() const;
    void restoreNav(const NavState& nav);
    void goUp();
    void selectPath(const std::string& path);
    void loadRecent();
    void rememberRecent(const std::string& path);
    void setPathActive(bool on);
    void finishPathInput();
    void setListView(bool enabled);
    void toggleSettings();
    bool clickSettings(int x, int y);
    void drawSettings(int W, int H);
    void optionsChanged();
    void rebuildView();
    void clampSel();
    void clampScroll();
    int rowCount() const;
    void layoutGrid(int W, int H);
    void revealSelection();
    void scrollGrid(int notches);
    void activate();
    int cardAt(int x, int y) const;
    void exportAot();
    void moveSelection(int delta);
    void moveSelectionRow(int dir);
    void setSearchActive(bool on);
    void startThumbs();
    void stopThumbs();
    void workerLoop();
    void requestThumb(const Entry& e, int tw, int th);
    void pumpThumbs();
    void updateThumbs(int tw, int th);
    void draw();
    void drawFinderList(int W, int H);
    void drawFinderPreview(int W, int H);
    void drawText(int x, int y, const std::string& utf8, int px, Color c,
                  int* outW = nullptr, bool bold = false);
    void drawTextClipped(int x, int y, const std::string& utf8, int px,
                         Color c, int maxW, bool bold = false);
    void fill(int x, int y, int w, int h, Color c);
    void roundRect(int x, int y, int w, int h, int r, Color c);
    void ringRect(int x, int y, int w, int h, int r, Color c, int t,
                  Color bg);
    void fillCircle(int cx, int cy, int r, Color c);
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c);
    void button(const SDL_Rect& r, const std::string& label, bool hot,
                bool accent, bool enabled = true);
    SDL_Texture* textTexture(const std::string& utf8, int px, Color c,
                             int* w, int* h, bool bold = false);

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    std::unordered_map<std::string, Tex> cache_;
    std::unordered_map<std::string, Thumb> thumbs_;
    std::thread thumbWorker_;
    std::mutex thumbMu_;
    std::condition_variable thumbCv_;
    std::deque<ThumbJob> thumbQueue_;
    std::vector<ThumbOut> thumbDone_;
    std::atomic<bool> thumbStop_{false};
    std::string dir_;
    std::vector<Entry> entries_;
    std::vector<int> view_;       // entries_ のうちフィルタ後の添字
    std::string filter_;
    bool searchActive_ = false;
    bool pathActive_ = false;
    std::string pathInput_;
    std::vector<NavState> backDirs_, forwardDirs_;
    std::vector<std::string> recent_;
    std::string examplesDir_, homeDir_;
    bool sortRecent_ = false;
    bool listView_ = true;
    bool settingsOpen_ = false;
    GuiRunOptions options_{};
    SDL_Rect bSettings_{}, bSettingsClose_{};
    SDL_Rect bScale_[4]{}, bFps_[4]{}, bJit_[3]{};
    SDL_Rect bLazy_[3]{}, bGpu_[3]{}, bTurbo_{}, bDanger_{};
    int previewW_ = 0;
    bool pathSelectAll_ = false;
    std::vector<std::pair<SDL_Rect, std::string>> breadcrumbs_;
    int sel_ = 0;
    int scrollRow_ = 0;       // 先頭に見えている行（行単位スクロール）
    bool revealSel_ = true;   // 選択が変わったときだけ見える位置へ寄せる
    int hotCard_ = -1;
    int mx_ = -1, my_ = -1;
    std::string status_;
    bool statusErr_ = false;
    size_t actIdx_ = 0;
    std::vector<std::string> actions_;
    bool pendingAot_ = false;
    bool pendingEdit_ = false;
    std::string pending_;
    std::string shotPath_;
    long frameNo_ = 0;
    SDL_Rect bRun_{}, bAotRun_{}, bExport_{}, bEdit_{}, bQuit_{};
    SDL_Rect bSearch_{}, bClear_{};
    SDL_Rect bHome_{}, bDesktop_{}, bDocuments_{};
    SDL_Rect bExamples_{}, bDownloads_{};
    SDL_Rect bBack_{}, bForward_{}, bUp_{}, bPath_{}, bSort_{};
    SDL_Rect bIcons_{}, bList_{};
    SDL_Rect bRecent_[5]{};
    int sideW_ = 218;
    // グリッドのレイアウト（draw が更新し、ヒットテストが参照する）
    int gridTop_ = 0, gridH_ = 0, cardW_ = 240, cardH_ = 210, gap_ = 18;
    int gridX_ = 20, cols_ = 1, rowsVisible_ = 1, contentH_ = 0;
};

void Launcher::fill(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(ren_, &r);
}

void Launcher::roundRect(int x, int y, int w, int h, int r, Color c) {
    if (w <= 0 || h <= 0) return;
    r = std::clamp(r, 0, std::min(w, h) / 2);
    if (r == 0) {
        fill(x, y, w, h, c);
        return;
    }
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_Rect mid{x, y + r, w, h - 2 * r};
    SDL_Rect top{x + r, y, w - 2 * r, r};
    SDL_Rect bot{x + r, y + h - r, w - 2 * r, r};
    SDL_RenderFillRect(ren_, &mid);
    SDL_RenderFillRect(ren_, &top);
    SDL_RenderFillRect(ren_, &bot);
    for (int i = 0; i <= r; ++i) {
        const double dy = double(r - i);
        const int dx = int(std::lround(double(r) -
                                       std::sqrt(double(r * r) - dy * dy)));
        SDL_Rect a{x + dx, y + i, w - 2 * dx, 1};
        SDL_Rect b{x + dx, y + h - 1 - i, w - 2 * dx, 1};
        SDL_RenderFillRect(ren_, &a);
        SDL_RenderFillRect(ren_, &b);
    }
}

void Launcher::ringRect(int x, int y, int w, int h, int r, Color c, int t,
                        Color bg) {
    roundRect(x, y, w, h, r, c);
    roundRect(x + t, y + t, w - 2 * t, h - 2 * t, std::max(0, r - t), bg);
}

void Launcher::fillCircle(int cx, int cy, int r, Color c) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    for (int dy = -r; dy <= r; ++dy) {
        const int dx = int(std::lround(std::sqrt(double(r * r - dy * dy))));
        SDL_Rect row{cx - dx, cy + dy, 2 * dx + 1, 1};
        SDL_RenderFillRect(ren_, &row);
    }
}

void Launcher::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                            Color c) {
    const int minY = std::min({y0, y1, y2});
    const int maxY = std::max({y0, y1, y2});
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    const int xs[3][2] = {{x0, y0}, {x1, y1}, {x2, y2}};
    for (int y = minY; y <= maxY; ++y) {
        int lo = 0, hi = -1;
        for (int e = 0; e < 3; ++e) {
            const int ax = xs[e][0], ay = xs[e][1];
            const int bx = xs[(e + 1) % 3][0], by = xs[(e + 1) % 3][1];
            if (ay == by) continue;
            if (y < std::min(ay, by) || y > std::max(ay, by)) continue;
            const double tt = double(y - ay) / double(by - ay);
            const int x = int(std::lround(ax + tt * (bx - ax)));
            if (hi < lo) {
                lo = hi = x;
            } else {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
        if (lo <= hi) {
            SDL_Rect row{lo, y, hi - lo + 1, 1};
            SDL_RenderFillRect(ren_, &row);
        }
    }
}

SDL_Texture* Launcher::textTexture(const std::string& utf8, int px,
                                   Color c, int* w, int* h, bool bold) {
    char suffix[48];
    std::snprintf(suffix, sizeof(suffix), "|%d|%02x%02x%02x|%d", px, c.r,
                  c.g, c.b, bold ? 1 : 0);
    const std::string key = utf8 + suffix;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        if (w) *w = it->second.w;
        if (h) *h = it->second.h;
        return it->second.tex;
    }
    std::u16string u = utf8ToU16(utf8);
    std::vector<uint8_t> bmp;
    int tw = 0, th = 0;
    FontRenderer* fr = FontRenderer::get();
    if (!fr->render(u, px, c.r, c.g, c.b, bmp, tw, th, bold)) return nullptr;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        bmp.data(), tw, th, 32, tw * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    cache_[key] = Tex{tex, tw, th};
    if (w) *w = tw;
    if (h) *h = th;
    return tex;
}

void Launcher::drawText(int x, int y, const std::string& utf8, int px,
                        Color c, int* outW, bool bold) {
    int w = 0, h = 0;
    SDL_Texture* tex = textTexture(utf8, px, c, &w, &h, bold);
    if (!tex) {
        // フォントが無い環境向けフォールバック: 1 文字 = 6px の箱
        const int n = int(utf8ToU16(utf8).size());
        fill(x, y + 3, n * (px / 2), std::max(2, px / 3), c);
        if (outW) *outW = n * (px / 2);
        return;
    }
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(ren_, tex, nullptr, &dst);
    if (outW) *outW = w;
}

void Launcher::drawTextClipped(int x, int y, const std::string& utf8, int px,
                               Color c, int maxW, bool bold) {
    if (maxW <= 4) return;
    int w = 0;
    textTexture(utf8, px, c, &w, nullptr, bold);
    std::string shown = utf8;
    if (w > maxW) {
        int ww = 0;
        textTexture("...", px, c, &ww, nullptr, bold);
        const int room = std::max(0, maxW - ww);
        size_t keep = w > 0 ? size_t(double(shown.size()) * room / w) : 0;
        while (keep > 0 && keep < shown.size() &&
               (static_cast<unsigned char>(shown[keep]) & 0xC0) == 0x80)
            --keep;
        shown = shown.substr(0, keep) + "...";
    }
    SDL_Rect clip{x, y - 2, maxW, px + 6};
    SDL_RenderSetClipRect(ren_, &clip);
    drawText(x, y, shown, px, c, nullptr, bold);
    SDL_RenderSetClipRect(ren_, nullptr);
}

void Launcher::loadRecent() {
    recent_.clear();
    const std::string filename = recentHistoryPath();
    if (filename.empty()) return;
    std::ifstream stream(filename);
    std::string path;
    while (recent_.size() < 5 && std::getline(stream, path)) {
        if (path.empty() || path.size() > 4096) continue;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        const std::string ext = lowerAscii(fs::path(path).extension().string());
        if (ext != ".sb3" && ext != ".sb2") continue;
        if (std::find(recent_.begin(), recent_.end(), path) == recent_.end())
            recent_.push_back(path);
    }
}

void Launcher::rememberRecent(const std::string& path) {
    if (path.empty() || path.find('\n') != std::string::npos ||
        path.find('\r') != std::string::npos) return;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), path),
                  recent_.end());
    recent_.insert(recent_.begin(), path);
    if (recent_.size() > 5) recent_.resize(5);
    const std::string filename = recentHistoryPath();
    if (filename.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(filename).parent_path(), ec);
    if (ec) return;
    const std::string tmp = filename + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        for (const std::string& item : recent_) f << item << '\n';
        if (!f) return;
    }
    // A same-directory temporary file avoids partially written history.
    fs::rename(tmp, filename, ec);
#ifdef _WIN32
    if (ec) {
        fs::remove(filename, ec);
        ec.clear();
        fs::rename(tmp, filename, ec);
    }
#endif
}

bool Launcher::navigatePath(const std::string& raw, bool record) {
    std::string path = raw;
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);
    if (path == "~" && !userHome().empty()) path = userHome();
    else if (path.size() >= 2 && path[0] == '~' &&
             (path[1] == '/' || path[1] == '\\') && !userHome().empty())
        path = userHome() + path.substr(1);
    if (path.empty()) {
        setStatus("開くフォルダのパスを入力してください", true);
        return false;
    }
    std::error_code ec;
    fs::path target(path);
    if (target.is_relative()) target = fs::path(dir_) / target;
    target = fs::absolute(target, ec).lexically_normal();
    if (ec) {
        setStatus("パスを解決できません: " + path, true);
        return false;
    }
    const bool isFile = fs::is_regular_file(target, ec);
    if (isFile) {
        const std::string ext = lowerAscii(target.extension().string());
        if (ext != ".sb3" && ext != ".sb2") {
            setStatus(".sb3 / .sb2 ファイルを選んでください", true);
            return false;
        }
    } else if (!fs::is_directory(target, ec)) {
        setStatus("フォルダが見つかりません: " + path, true);
        return false;
    }
    const std::string nextDir =
        (isFile ? target.parent_path() : target).string();
    if (record && !dir_.empty() && dir_ != nextDir) {
        backDirs_.push_back(currentNav());
        if (backDirs_.size() > 64) backDirs_.erase(backDirs_.begin());
        forwardDirs_.clear();
    }
    if (dir_ != nextDir || entries_.empty()) loadDir(nextDir);
    if (isFile) selectPath(target.string());
    setPathActive(false);
    return true;
}

NavState Launcher::currentNav() const {
    NavState nav{dir_, {}, scrollRow_};
    if (sel_ >= 0 && sel_ < int(entries_.size()))
        nav.selection = entries_[size_t(sel_)].path;
    return nav;
}

void Launcher::restoreNav(const NavState& nav) {
    loadDir(nav.dir);
    if (!nav.selection.empty()) selectPath(nav.selection);
    scrollRow_ = nav.scrollRow;
    clampScroll();
    revealSel_ = false;
}

void Launcher::goBack() {
    if (backDirs_.empty()) return;
    NavState nav = std::move(backDirs_.back());
    backDirs_.pop_back();
    if (!dir_.empty()) forwardDirs_.push_back(currentNav());
    restoreNav(nav);
}

void Launcher::goForward() {
    if (forwardDirs_.empty()) return;
    NavState nav = std::move(forwardDirs_.back());
    forwardDirs_.pop_back();
    if (!dir_.empty()) backDirs_.push_back(currentNav());
    restoreNav(nav);
}

void Launcher::goUp() {
    const fs::path parent = fs::path(dir_).parent_path();
    if (!parent.empty() && parent != fs::path(dir_))
        navigatePath(parent.string());
}

void Launcher::selectPath(const std::string& path) {
    for (int i : view_) {
        if (entries_[size_t(i)].path == path) {
            sel_ = i;
            revealSelection();
            return;
        }
    }
    // A recent project or a dropped file must remain visible even when a
    // previous search filter excluded it.
    filter_.clear();
    rebuildView();
    for (int i : view_)
        if (entries_[size_t(i)].path == path) {
            sel_ = i;
            revealSelection();
            return;
        }
}

void Launcher::setPathActive(bool on) {
    if (pathActive_ == on) return;
    if (on) {
        setSearchActive(false);
        pathInput_ = dir_;
        pathSelectAll_ = true;
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
    pathActive_ = on;
}

void Launcher::finishPathInput() {
    const std::string value = pathInput_;
    if (navigatePath(value)) setPathActive(false);
}

void Launcher::loadDir(const std::string& dir) {
    // 絶対パスに正規化してから列挙する（相対パスの二重解決を防ぐ）
    std::error_code ec;
    const std::string abs = fs::absolute(dir, ec).lexically_normal().string();
    if (ec || !fs::is_directory(abs, ec)) {
        setStatus("フォルダを開けません: " + dir, true);
        return;
    }
    dir_ = abs;
    entries_.clear();
    filter_.clear();
    setSearchActive(false);
    sel_ = 0;
    scrollRow_ = 0;
    status_.clear();
    for (auto& [k, t] : thumbs_)
        if (t.tex) SDL_DestroyTexture(t.tex);
    thumbs_.clear();
    std::vector<Entry> dirs, files;
    fs::directory_iterator it(dir_, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(e2)) {
            Entry e;
            e.name = name;
            e.path = it->path().string();
            e.isDir = true;
            dirs.push_back(std::move(e));
        } else if (it->is_regular_file(e2)) {
            const std::string ext = it->path().extension().string();
            if (ext != ".sb3" && ext != ".sb2") continue;
            Entry e;
            e.name = name;
            e.path = it->path().string();
            std::error_code es;
            e.size = fs::file_size(it->path(), es);
            if (es) e.size = 0;
            std::error_code et;
            const auto ft = fs::last_write_time(it->path(), et);
            if (!et) {
                const auto sys = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                e.mtime = std::chrono::system_clock::to_time_t(sys);
            }
            files.push_back(std::move(e));
        }
    }
    auto byName = [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    // Finder-style navigation uses the toolbar, not a synthetic ".." card.
    for (auto& e : dirs) entries_.push_back(std::move(e));
    for (auto& e : files) entries_.push_back(std::move(e));
    if (ec) setStatus("フォルダの内容を読み取れませんでした", true);
    rebuildView();
    sel_ = -1;
    // Prefer a project over a folder on first open.
    for (const Entry& e : entries_)
        if (!e.isDir) { selectPath(e.path); break; }
    if (sel_ == -1)
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].name != "..") {
                sel_ = int(i);
                break;
            }
    clampSel();
    revealSelection();
    if (std::getenv("XYO_GUI_DEBUG"))
        std::fprintf(stderr, "gui: dir=%s entries=%zu\n", dir_.c_str(),
                     entries_.size());
}

void Launcher::rebuildView() {
    view_.clear();
    const std::string needle = lowerAscii(filter_);
    for (int i = 0; i < int(entries_.size()); ++i) {
        if (needle.empty() ||
            lowerAscii(entries_[size_t(i)].name).find(needle) !=
                std::string::npos)
            view_.push_back(i);
    }
    if (sortRecent_) {
        std::stable_sort(view_.begin(), view_.end(), [this](int ia, int ib) {
            const Entry& a = entries_[size_t(ia)];
            const Entry& b = entries_[size_t(ib)];
            if (a.name == ".." || b.name == "..") return a.name == "..";
            if (a.isDir != b.isDir) return a.isDir;
            if (!a.isDir && a.mtime != b.mtime) return a.mtime > b.mtime;
            return lowerAscii(a.name) < lowerAscii(b.name);
        });
    }
}

void Launcher::clampSel() {
    if (view_.empty()) {
        sel_ = -1;
        return;
    }
    if (std::find(view_.begin(), view_.end(), sel_) == view_.end())
        sel_ = view_.front();
}

int Launcher::rowCount() const {
    if (view_.empty() || cols_ <= 0) return 0;
    return int((view_.size() + size_t(cols_) - 1) / size_t(cols_));
}

void Launcher::clampScroll() {
    const int maxRow = std::max(0, rowCount() - rowsVisible_);
    scrollRow_ = std::clamp(scrollRow_, 0, maxRow);
}

/// グリッドの寸法を更新する。描画前だけでなく入力処理の前にも呼ぶことで、
/// ホイールやフィルタ変更の直後から正しい範囲でクランプできる。
void Launcher::setListView(bool enabled) {
    if (listView_ == enabled) return;
    listView_ = enabled;
    scrollRow_ = 0;
    revealSelection();
    // Re-query the grid geometry immediately so hit-testing is correct.
    if (win_) {
        int w = 0, h = 0;
        SDL_GetWindowSize(win_, &w, &h);
        layoutGrid(w, h);
    }
}

void Launcher::layoutGrid(int W, int H) {
    sideW_ = W < 880 ? 186 : 218;
    gridX_ = sideW_ + 24;
    previewW_ = listView_ && W - gridX_ >= 720
                    ? std::clamp((W - gridX_) / 3, 230, 290) : 0;
    gridTop_ = listView_ ? 209 : 185;
    gridH_ = std::max(80, H - gridTop_ - 96 - 12);
    const int avail = std::max(180, W - gridX_ - 24);
    if (listView_) {
        gap_ = 0;
        cols_ = 1;
        cardW_ = std::max(120, avail - previewW_ -
                                    (previewW_ ? 14 : 0));
        cardH_ = 40;
    } else {
        gap_ = 16;
        cols_ = std::clamp((avail + gap_) / (192 + gap_), 1, 5);
        cardW_ = (avail - (cols_ - 1) * gap_) / cols_;
        cardH_ = int(std::lround(cardW_ * 0.75)) - 12 + 54;
    }
    // Whole rows: compatible SDL renderers may ignore clip rectangles.
    rowsVisible_ = std::max(1, (gridH_ + gap_) / (cardH_ + gap_));
    contentH_ = std::max(0, rowCount() * (cardH_ + gap_) - gap_);
    clampSel();
    clampScroll();
}

/// 次の描画で選択カードを画面内へ寄せる（キーボード移動・検索時のみ）。
void Launcher::revealSelection() { revealSel_ = true; }

/// ホイール 1 ノッチ = 64px。ユーザー操作なので選択追従は止める。
void Launcher::scrollGrid(int notches) {
    int W = 0, H = 0;
    SDL_GetWindowSize(win_, &W, &H);
    layoutGrid(W, H);
    // notches: wheel delta (positive = wheel up / scroll towards the start)
    scrollRow_ -= listView_ ? notches * 3 : notches;
    revealSel_ = false;
    clampScroll();
}

void Launcher::moveSelection(int delta) {
    if (view_.empty()) return;
    int pos = 0;
    const auto it = std::find(view_.begin(), view_.end(), sel_);
    if (it != view_.end()) pos = int(it - view_.begin());
    pos = std::clamp(pos + delta, 0, int(view_.size()) - 1);
    sel_ = view_[size_t(pos)];
    revealSelection();
}

void Launcher::moveSelectionRow(int dir) {
    if (view_.empty()) return;
    int pos = 0;
    const auto it = std::find(view_.begin(), view_.end(), sel_);
    if (it != view_.end()) pos = int(it - view_.begin());
    pos = std::clamp(pos + dir * std::max(1, cols_), 0,
                     int(view_.size()) - 1);
    sel_ = view_[size_t(pos)];
    revealSelection();
}

void Launcher::setSearchActive(bool on) {
    if (searchActive_ == on) return;
    if (on) {
        setPathActive(false);
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
    searchActive_ = on;
}

bool Launcher::open() {
    // SDL_main を使わないので明示的に main ready を立てる（Windows で必須）
    SDL_SetMainReady();
    // 最初のクリックがフォーカス取得だけで捨てられないようにする。
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "gui: SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    win_ = SDL_CreateWindow("xyo — Project library",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            1120, 760, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win_) return false;
    SDL_SetWindowMinimumSize(win_, 760, 560);
#if defined(__APPLE__) && defined(XYO_PEN_GL)
    // The SDL dummy video driver used in headless CI has no OpenGL renderer.
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    if (!std::getenv("XYO_PEN_CPU") && videoDriver &&
        SDL_strcmp(videoDriver, "dummy") != 0)
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    ren_ = SDL_CreateRenderer(
        win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
    if (!ren_) return false;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    // Retina and other high-DPI displays: draw in window (point) units.
    // SDL scales the logical scene to the physical renderer output.
    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(win_, &logicalW, &logicalH);
    SDL_RenderSetLogicalSize(ren_, logicalW, logicalH);
    homeDir_ = userHome();
    examplesDir_ = sampleDirectory();
    loadRecent();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    startThumbs();
    return true;
}

void Launcher::close() {
    setSearchActive(false);
    setPathActive(false);
    stopThumbs();
    for (auto& [k, t] : cache_)
        if (t.tex) SDL_DestroyTexture(t.tex);
    cache_.clear();
    for (auto& [k, t] : thumbs_)
        if (t.tex) SDL_DestroyTexture(t.tex);
    thumbs_.clear();
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    ren_ = nullptr;
    win_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Launcher::setStatus(const std::string& s, bool err) {
    status_ = s;
    statusErr_ = err;
    if (std::getenv("XYO_GUI_DEBUG") || err)
        std::fprintf(stderr, "gui: %s\n", s.c_str());
}

/// 選択項目を開く。ディレクトリなら移動、プロジェクトなら実行予約。
void Launcher::activate() {
    clampSel();
    if (sel_ < 0 || sel_ >= int(entries_.size())) return;
    const Entry& e = entries_[size_t(sel_)];
    if (e.isDir) {
        navigatePath(e.path);
        return;
    }
    pending_ = e.path;
    pendingAot_ = false;
    rememberRecent(e.path);
}

/// カードグリッド上の座標 → view_ の添字（無ければ -1）。
int Launcher::cardAt(int x, int y) const {
    if (y < gridTop_ || y >= gridTop_ + gridH_) return -1;
    if (x < gridX_ || x >= gridX_ + cols_ * (cardW_ + gap_)) return -1;
    const int relX = x - gridX_;
    const int col = relX / (cardW_ + gap_);
    if (col >= cols_ || relX % (cardW_ + gap_) >= cardW_) return -1;
    const int localY = y - gridTop_;
    if (localY % (cardH_ + gap_) >= cardH_) return -1;
    const int row = scrollRow_ + localY / (cardH_ + gap_);
    if (row >= scrollRow_ + rowsVisible_) return -1;
    const int pos = row * cols_ + col;
    return pos >= 0 && pos < int(view_.size()) ? pos : -1;
}

/// サムネイル（ステージの最初の背景）を遅延生成してキャッシュする。
void Launcher::startThumbs() {
    thumbStop_.store(false);
    thumbWorker_ = std::thread([this] { workerLoop(); });
}

void Launcher::stopThumbs() {
    thumbStop_.store(true);
    thumbCv_.notify_all();
    if (thumbWorker_.joinable()) thumbWorker_.join();
    std::lock_guard<std::mutex> lk(thumbMu_);
    thumbQueue_.clear();
    thumbDone_.clear();
}

void Launcher::workerLoop() {
    const std::string dir = thumbCacheDir();
    for (;;) {
        ThumbJob job;
        {
            std::unique_lock<std::mutex> lk(thumbMu_);
            thumbCv_.wait(lk, [this] {
                return thumbStop_.load() || !thumbQueue_.empty();
            });
            if (thumbStop_.load()) return;
            job = std::move(thumbQueue_.front());
            thumbQueue_.pop_front();
        }
        ThumbOut res;
        res.path = job.path;
        res.mtime = job.mtime;
        res.w = job.w;
        res.h = job.h;
        const std::string cache = thumbCacheFile(dir, job);
        if (readThumbCache(cache, res.rgba, res.w, res.h)) {
            res.ok = true;
        } else if (renderThumbRGBA(job.path, job.w, job.h, res.rgba)) {
            res.ok = true;
            writeThumbCache(cache, res.rgba, res.w, res.h);
        }
        {
            std::lock_guard<std::mutex> lk(thumbMu_);
            thumbDone_.push_back(std::move(res));
        }
    }
}

/// サムネイル生成を依頼する（同じカードは 1 回だけ）。
void Launcher::requestThumb(const Entry& e, int tw, int th) {
    if (e.isDir || tw <= 0 || th <= 0) return;
    Thumb& t = thumbs_[e.path];
    if (t.tex && t.w == tw && t.h == th && t.mtime == e.mtime) return;
    if (t.requested) return;
    if (t.failed && t.w == tw && t.h == th && t.mtime == e.mtime) return;
    t.failed = false;
    if (t.tex) {
        SDL_DestroyTexture(t.tex);
        t.tex = nullptr;
    }
    t.requested = true;
    t.mtime = e.mtime;
    t.w = tw;
    t.h = th;
    ThumbJob job;
    job.path = e.path;
    job.mtime = e.mtime;
    job.size = e.size;
    job.w = tw;
    job.h = th;
    {
        std::lock_guard<std::mutex> lk(thumbMu_);
        thumbQueue_.push_back(std::move(job));
    }
    thumbCv_.notify_one();
}

/// 生成済みサムネイルをテクスチャ化する（1 フレーム最大 8 枚）。
void Launcher::pumpThumbs() {
    std::vector<ThumbOut> done;
    {
        std::lock_guard<std::mutex> lk(thumbMu_);
        done.swap(thumbDone_);
    }
    int uploaded = 0;
    std::vector<ThumbOut> carry;
    for (ThumbOut& r : done) {
        auto it = thumbs_.find(r.path);
        if (it == thumbs_.end()) continue;   // 表示中のフォルダが変わった
        Thumb& t = it->second;
        if (t.mtime != r.mtime) continue;
        if (!r.ok) {
            t.failed = true;
            t.requested = false;
            continue;
        }
        if (uploaded >= 8) {
            carry.push_back(std::move(r));
            continue;
        }
        SDL_Texture* tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, r.w,
                                             r.h);
        if (tex &&
            SDL_UpdateTexture(tex, nullptr, r.rgba.data(), r.w * 4) == 0) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            if (t.tex) SDL_DestroyTexture(t.tex);
            t.tex = tex;
            t.w = r.w;
            t.h = r.h;
            t.failed = false;
            t.requested = false;
            ++uploaded;
        } else {
            if (tex) SDL_DestroyTexture(tex);
            t.failed = true;
            t.requested = false;
        }
    }
    if (!carry.empty()) {
        std::lock_guard<std::mutex> lk(thumbMu_);
        for (ThumbOut& r : carry) thumbDone_.push_back(std::move(r));
    }
}

/// 可視行（+1 行先読み）のサムネイルを要求し、完成分を取り込む。
void Launcher::updateThumbs(int tw, int th) {
    pumpThumbs();
    if (!thumbWorker_.joinable()) return;
    const int lastRow =
        std::min(rowCount() - 1, scrollRow_ + rowsVisible_);
    for (int row = scrollRow_; row <= lastRow; ++row) {
        for (int col = 0; col < cols_; ++col) {
            const int pos = row * cols_ + col;
            if (pos >= int(view_.size())) break;
            requestThumb(entries_[size_t(view_[size_t(pos)])], tw, th);
        }
    }
}

/// AOT 事前コンパイルして単体実行ファイルを書き出す。
void Launcher::exportAot() {
#ifndef XYO_JIT
    setStatus("このビルドは JIT 無しのため AOT できません", true);
#else
    clampSel();
    if (sel_ < 0 || sel_ >= int(entries_.size()) ||
        entries_[size_t(sel_)].isDir) {
        setStatus("プロジェクトファイルを選んでください", true);
        return;
    }
    const Entry e = entries_[size_t(sel_)];
    // 書き出し中の表示を先に出す
    setStatus("AOT 書き出し中... " + e.name, false);
    draw();
    SDL_RenderPresent(ren_);

    std::vector<uint8_t> data;
    {
        std::FILE* f = std::fopen(e.path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                data.resize(size_t(n));
                if (std::fread(data.data(), 1, data.size(), f) !=
                    data.size())
                    data.clear();
            }
            std::fclose(f);
        }
    }
    if (data.empty()) {
        setStatus("読み込みに失敗しました: " + e.name, true);
        return;
    }
    const uint64_t hash = aot::hashBytes(data.data(), data.size());
    Runtime rt;
    if (!loadSb3File(rt, data.data(), data.size()).ok) {
        setStatus("プロジェクトのロードに失敗しました", true);
        return;
    }
    aot::BuildResult br;
    std::string why;
    if (!aot::compileProject(rt, hash, br, &why)) {
        setStatus("AOT コンパイル不可: " + why, true);
        return;
    }
    const fs::path src(e.path);
    fs::path out = src.parent_path() / src.stem();
    std::error_code ec;
    if (fs::exists(out, ec)) {
        out = src.parent_path() / (src.stem().string() + "-aot");
    }
    if (!aot::writeExecutable(out.string(), data.data(), data.size(), br,
                              hash, &why)) {
        setStatus("書き出し失敗: " + why, true);
        return;
    }
    setStatus("書き出しました: " + out.string() + "（" +
                  std::to_string(br.scripts) + " scripts / " +
                  std::to_string(br.slots) + " slots）",
              false);
#endif
}

void Launcher::button(const SDL_Rect& r, const std::string& label, bool hot,
                      bool accent, bool enabled) {
    const Color bg = !enabled ? Color{0xF4, 0xF5, 0xF8, 255}
                   : accent ? (hot ? kAccentHot : kAccent)
                   : hot ? Color{0xF0, 0xEE, 0xFF, 255} : kPanel;
    const Color fg = !enabled ? kMuted
                   : accent ? Color{255, 255, 255, 255} : kFg;
    roundRect(r.x, r.y, r.w, r.h, r.h / 2, bg);
    if (!accent || !enabled)
        ringRect(r.x, r.y, r.w, r.h, r.h / 2,
                 enabled && hot ? Color{0xC9, 0xB8, 0xF0, 255}
                                : Color{0xDF, 0xE4, 0xEF, 255}, 1, bg);
    int tw = 0, th = 0;
    textTexture(label, 14, fg, &tw, &th, accent && enabled);
    drawText(r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, label, 14,
             fg, nullptr, accent && enabled);
}

void Launcher::drawFinderPreview(int W, int H) {
    if (!listView_ || previewW_ == 0) return;
    const int x = gridX_ + cardW_ + 13;
    const int y = gridTop_ - 28;
    const int w = std::max(40, W - x - 24);
    const int h = H - 96 - y - 11;
    fill(x, y, w, h, kPanel);
    fill(x, y, 1, h, Color{0xE3, 0xE7, 0xF0, 255});
    drawText(x + 18, y + 14, "プレビュー", 14, kStrong, nullptr, true);
    if (sel_ < 0 || sel_ >= int(entries_.size())) {
        drawTextClipped(x + 18, y + 66, "ファイルを選択してください",
                        12, kDim, w - 36);
        return;
    }
    const Entry& selected = entries_[size_t(sel_)];
    const int pw = std::max(24, std::min(w - 36, 236));
    const int ph = std::min(pw * 3 / 4, 180);
    const int px = x + (w - pw) / 2;
    const int py = y + 55;
    roundRect(px, py, pw, ph, 10, kField);
    if (selected.isDir) {
        const int cx = px + pw / 2, cy = py + ph / 2;
        roundRect(cx - 37, cy - 22, 74, 48, 8, kFolder);
        roundRect(cx - 37, cy - 31, 30, 18, 6, kFolderDark);
        roundRect(cx - 35, cy - 18, 70, 42, 7, kFolderLight);
    } else {
        auto it = thumbs_.find(selected.path);
        if (it != thumbs_.end() && it->second.tex) {
            SDL_Rect dst{px, py, pw, ph};
            SDL_RenderCopy(ren_, it->second.tex, nullptr, &dst);
        } else {
            drawTextClipped(px + 12, py + ph / 2 - 8, "読み込み中...", 12,
                            kDim, pw - 24);
        }
    }
    const int infoY = py + ph + 22;
    const std::string name = selected.isDir ? selected.name :
                             fs::path(selected.name).stem().string();
    drawTextClipped(x + 18, infoY, name, 16, kStrong, w - 36, true);
    drawTextClipped(x + 18, infoY + 29, selected.isDir ? "フォルダ" :
                    "Scratch プロジェクト", 12, kDim, w - 36);
    fill(x + 18, infoY + 58, w - 36, 1,
         Color{0xE4, 0xE8, 0xF0, 255});
    drawText(x + 18, infoY + 74, "情報", 12, kStrong, nullptr, true);
    drawTextClipped(x + 18, infoY + 100, selected.isDir ? "種類  フォルダ" :
                    "種類  " + lowerAscii(fs::path(selected.name).extension().string()),
                    12, kDim, w - 36);
    if (!selected.isDir) {
        drawTextClipped(x + 18, infoY + 123, "サイズ  " + humanSize(selected.size),
                        12, kDim, w - 36);
        const std::string date = formatDate(selected.mtime);
        if (!date.empty())
            drawTextClipped(x + 18, infoY + 146, "変更日  " + date,
                            12, kDim, w - 36);
    }
}

void Launcher::drawFinderList(int W, int H) {
    const int x = gridX_, top = gridTop_ - 29;
    fill(x, top, cardW_, H - 96 - top - 10, kPanel);
    fill(x, top + 28, cardW_, 1, Color{0xE5, 0xE9, 0xF2, 255});
    drawText(x + 15, top + 7, "名前", 12, kDim);
    if (cardW_ >= 420) {
        drawText(x + cardW_ - 180, top + 7, "変更日", 12, kDim);
        drawText(x + cardW_ - 77, top + 7, "サイズ", 12, kDim);
    } else {
        drawText(x + cardW_ - 102, top + 7, "種類", 12, kDim);
    }
    for (int row = scrollRow_; row < scrollRow_ + rowsVisible_; ++row) {
        if (row >= rowCount()) break;
        const int pos = row;
        const Entry& e = entries_[size_t(view_[size_t(pos)])];
        const int y = gridTop_ + (row - scrollRow_) * cardH_;
        const bool selected = view_[size_t(pos)] == sel_;
        const bool hover = hotCard_ == pos;
        if (selected) {
            fill(x + 4, y + 2, cardW_ - 8, cardH_ - 4,
                 Color{0xDA, 0xE6, 0xFF, 255});
        } else if (hover) {
            fill(x + 4, y + 2, cardW_ - 8, cardH_ - 4,
                 Color{0xF1, 0xF4, 0xFC, 255});
        } else if ((row & 1) != 0) {
            fill(x, y, cardW_, cardH_, Color{0xFA, 0xFB, 0xFE, 255});
        }
        const int ix = x + 12;
        const int iy = y + 10;
        if (e.isDir) {
            roundRect(ix + 2, iy + 5, 26, 20, 4, kFolder);
            roundRect(ix + 2, iy + 2, 13, 9, 3, kFolderDark);
            roundRect(ix + 3, iy + 8, 24, 15, 3, kFolderLight);
        } else {
            auto it = thumbs_.find(e.path);
            if (it != thumbs_.end() && it->second.tex) {
                const SDL_Rect dst{ix + 2, iy, 30, 24};
                SDL_RenderCopy(ren_, it->second.tex, nullptr, &dst);
            } else {
                roundRect(ix + 3, iy, 27, 25, 4,
                          Color{0xD7, 0xD1, 0xF2, 255});
                drawText(ix + 10, iy + 5, "S", 12, kAccent, nullptr, true);
            }
        }
        const std::string name = e.isDir ? e.name :
                                 fs::path(e.name).stem().string();
        drawTextClipped(x + 53, y + 10, name, 13,
                        selected ? kStrong : kFg,
                        std::max(16, cardW_ - (cardW_ >= 420 ? 242 : 168)),
                        selected);
        if (cardW_ >= 420) {
            const std::string date = e.isDir ? "—" : formatDate(e.mtime);
            drawTextClipped(x + cardW_ - 180, y + 12,
                            date, 11, kDim, 95);
            const std::string size = e.isDir ? "—" : humanSize(e.size);
            drawTextClipped(x + cardW_ - 77, y + 12,
                            size, 11, kDim, 69);
        } else {
            drawTextClipped(x + cardW_ - 102, y + 12,
                            e.isDir ? "フォルダ" : "Scratch",
                            11, kDim, 89);
        }
        fill(x, y + cardH_ - 1, cardW_, 1,
             Color{0xF0, 0xF2, 0xF7, 255});
    }
    drawFinderPreview(W, H);
}

void Launcher::toggleSettings() {
    if (!settingsOpen_) {
        setSearchActive(false);
        setPathActive(false);
    }
    settingsOpen_ = !settingsOpen_;
}

void Launcher::optionsChanged() {
    writeRunOptions(options_);
}

void Launcher::drawSettings(int W, int H) {
    fill(0, 0, W, H, Color{0x16, 0x24, 0x3B, 0xA2});
    const int panelW = std::min(676, W - 42);
    const int panelH = std::min(505, H - 32);
    const int x = (W - panelW) / 2;
    const int y = (H - panelH) / 2;
    roundRect(x + 4, y + 7, panelW, panelH, 18, kShadow);
    roundRect(x, y, panelW, panelH, 18, kPanel);
    ringRect(x, y, panelW, panelH, 18,
             Color{0xD9, 0xE0, 0xEC, 255}, 1, kPanel);
    drawText(x + 24, y + 18, "実行設定", 21, kStrong, nullptr, true);
    drawText(x + 24, y + 48,
             "選んだプロジェクトを実行するときに使用するフラグ", 12, kDim);
    bSettingsClose_ = {x + panelW - 49, y + 16, 32, 32};
    button(bSettingsClose_, "×", inside(bSettingsClose_, mx_, my_), false);

    const int groupX = x + 191;
    const int groupWidth = panelW - 217;
    const auto choices = [&](int index, const std::string& label,
                             SDL_Rect* items, const char* const* labels,
                             int count, int active, bool enabled = true) {
        const int yy = y + 83 + index * 61;
        drawText(x + 26, yy + 9, label, 13, kFg, nullptr, true);
        const int spacing = 6;
        const int itemW = (groupWidth - spacing * (count - 1)) / count;
        for (int i = 0; i < count; ++i) {
            items[i] = {groupX + i * (itemW + spacing), yy,
                        itemW, 35};
            button(items[i], labels[i],
                   inside(items[i], mx_, my_), i == active && enabled,
                   enabled);
        }
    };
    const char* const scales[] = {"1×", "2×", "3×", "4×"};
    const char* const fps[] = {"自動", "30", "60", "120"};
    const char* const jit[] = {"HotSpot", "Eager", "OFF"};
    const char* const lazy[] = {"自動", "ON", "OFF"};
    const char* const gpu[] = {"通常", "Ray", "Path"};
    choices(0, "画面倍率", bScale_, scales, 4, options_.scale - 1);
    const int fpsIndex = options_.fps == -1 ? 0 :
                         options_.fps == 30 ? 1 :
                         options_.fps == 60 ? 2 : 3;
    choices(1, "フレームレート", bFps_, fps, 4, fpsIndex);
#ifdef XYO_JIT
    choices(2, "JIT 実行方式", bJit_, jit, 3, options_.jitMode);
    choices(3, "遅延 JIT", bLazy_, lazy, 3, options_.lazyMode,
            options_.jitMode != 2);
#else
    choices(2, "JIT 実行方式", bJit_, jit, 3, 2, false);
    choices(3, "遅延 JIT", bLazy_, lazy, 3, 2, false);
#endif
    choices(4, "GPU 描画 (実験的)", bGpu_, gpu, 3,
            options_.gpuMode + 1);

    bTurbo_ = {x + 26, y + 394, 185, 36};
    bDanger_ = {x + 220, y + 394, 226, 36};
    button(bTurbo_, options_.turbo ? "✓ Turbo 強制 ON" : "Turbo 自動",
           inside(bTurbo_, mx_, my_), options_.turbo);
    button(bDanger_, options_.danger ? "✓ 危険モード有効" : "危険モード無効",
           inside(bDanger_, mx_, my_), options_.danger);
    drawTextClipped(x + 26, y + 446,
                    options_.danger
                        ? "注意: スクリプトの実行時間制限が解除されます"
                        : "設定は保存されます。危険モードだけは毎回無効に戻ります。",
                    12, options_.danger ? kWarn : kDim, panelW - 52);
}

bool Launcher::clickSettings(int x, int y) {
    if (inside(bSettingsClose_, x, y)) {
        settingsOpen_ = false;
        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if (inside(bScale_[i], x, y)) {
            options_.scale = i + 1;
            optionsChanged();
            return true;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (inside(bGpu_[i], x, y)) {
            options_.gpuMode = i - 1;
            optionsChanged();
            return true;
        }
#ifdef XYO_JIT
        if (inside(bJit_[i], x, y)) {
            options_.jitMode = i;
            optionsChanged();
            return true;
        }
        if (inside(bLazy_[i], x, y) && options_.jitMode != 2) {
            options_.lazyMode = i;
            optionsChanged();
            return true;
        }
#endif
    }
    const int frameRates[] = {-1, 30, 60, 120};
    for (int i = 0; i < 4; ++i) {
        if (inside(bFps_[i], x, y)) {
            options_.fps = frameRates[i];
            optionsChanged();
            return true;
        }
    }
    if (inside(bTurbo_, x, y)) {
        options_.turbo = !options_.turbo;
        optionsChanged();
    } else if (inside(bDanger_, x, y)) {
        options_.danger = !options_.danger;
    } else {
        // Modal outside click dismisses the sheet without triggering
        // controls underneath.
        settingsOpen_ = false;
    }
    return true;
}

void Launcher::draw() {
    int W = 1120, H = 760;
    SDL_GetWindowSize(win_, &W, &H);
    layoutGrid(W, H);
    const int footerH = 96;
    if (revealSel_) {
        if (sel_ >= 0) {
            const auto it = std::find(view_.begin(), view_.end(), sel_);
            if (it != view_.end()) {
                const int row = int(it - view_.begin()) / cols_;
                if (row < scrollRow_) scrollRow_ = row;
                if (row >= scrollRow_ + rowsVisible_)
                    scrollRow_ = row - rowsVisible_ + 1;
            }
        }
        revealSel_ = false;
        clampScroll();
    }

    SDL_SetRenderDrawColor(ren_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(ren_);

    // ---- navigation rail
    fill(0, 0, sideW_, H, kSidebar);
    roundRect(20, 19, 40, 40, 12, kAccent);
    drawText(33, 27, "X", 20, Color{255, 255, 255, 255}, nullptr, true);
    drawText(72, 18, "xyo", 23, kSidebarText, nullptr, true);
    drawText(73, 47, "PROJECT STUDIO", 10, kSidebarMuted);
    fill(20, 78, sideW_ - 40, 1, Color{0xD5, 0xDB, 0xE7, 255});
    drawText(22, 95, "よく使う項目", 12, kSidebarMuted, nullptr, true);

    const auto navItem = [&](SDL_Rect& area, int y, const std::string& glyph,
                             const std::string& label,
                             const std::string& target) {
        const bool available = !target.empty();
        area = available ? SDL_Rect{12, y, sideW_ - 24, 38} : SDL_Rect{};
        if (!available) return;
        const bool selected = fs::path(target).lexically_normal() ==
                              fs::path(dir_).lexically_normal();
        const bool hover = inside(area, mx_, my_);
        if (selected || hover)
            roundRect(area.x, area.y, area.w, area.h, 10,
                      selected ? Color{0xD3, 0xE3, 0xFA, 255}
                               : kSidebarHover);
        roundRect(area.x + 9, area.y + 8, 22, 22, 6,
                  selected ? Color{0xBB, 0xD5, 0xF6, 255}
                           : Color{0xDE, 0xE4, 0xF1, 255});
        drawText(area.x + 15, area.y + 10, glyph, 14, kAccent,
                 nullptr, true);
        drawTextClipped(area.x + 41, area.y + 10, label, 13,
                        selected ? kStrong : kSidebarText,
                        area.w - 51, selected);
    };
    const auto availableFolder = [](const fs::path& p) -> std::string {
        std::error_code ec;
        return fs::is_directory(p, ec) ? p.string() : std::string();
    };
    navItem(bHome_, 120, "H", "ホーム", homeDir_);
    navItem(bDesktop_, 163, "D", "デスクトップ",
            homeDir_.empty() ? "" :
            availableFolder(fs::path(homeDir_) / "Desktop"));
    navItem(bDocuments_, 206, "F", "書類",
            homeDir_.empty() ? "" :
            availableFolder(fs::path(homeDir_) / "Documents"));
    navItem(bDownloads_, 249, "↓", "ダウンロード",
            homeDir_.empty() ? "" :
            availableFolder(fs::path(homeDir_) / "Downloads"));
    navItem(bExamples_, 292, "S", "サンプル", examplesDir_);

    fill(20, 345, sideW_ - 40, 1, Color{0xD5, 0xDB, 0xE7, 255});
    drawText(22, 357, "最近開いたプロジェクト", 12, kSidebarMuted,
             nullptr, true);
    const int recentVisible = std::clamp((H - footerH - 445) / 44, 0, 5);
    for (int i = 0; i < 5; ++i) bRecent_[i] = {};
    for (int i = 0; i < std::min(int(recent_.size()), recentVisible); ++i) {
        const fs::path f(recent_[size_t(i)]);
        SDL_Rect& r = bRecent_[i];
        r = {12, 384 + i * 44, sideW_ - 24, 40};
        const bool hot = inside(r, mx_, my_);
        if (hot) roundRect(r.x, r.y, r.w, r.h, 10, kSidebarHover);
        roundRect(r.x + 9, r.y + 8, 24, 24, 7,
                  Color{0xD9, 0xD5, 0xF6, 255});
        drawText(r.x + 15, r.y + 12, "P", 12, kAccent, nullptr, true);
        drawTextClipped(r.x + 42, r.y + 3, f.stem().string(), 12,
                        kSidebarText, r.w - 50, true);
        drawTextClipped(r.x + 42, r.y + 22, f.parent_path().filename().string(),
                        10, kSidebarMuted, r.w - 50);
    }
    if (recent_.empty())
        drawTextClipped(22, 392, "実行するとここに表示", 12,
                        kSidebarMuted, sideW_ - 34);
    drawText(22, H - footerH - 49, "ファイルをドロップして開く", 11,
             kSidebarMuted);
    drawText(22, H - footerH - 29, "Ctrl / Cmd + L でパス入力", 11,
             kSidebarMuted);

    // ---- workspace header
    fill(sideW_, 0, W - sideW_, 75, kPanel);
    fill(sideW_, 74, W - sideW_, 1, Color{0xE1, 0xE7, 0xF1, 255});
    const std::string windowTitle = dir_.empty()
        ? "プロジェクト" : fs::path(dir_).filename().string();
    drawTextClipped(gridX_, 17, windowTitle.empty() ? "プロジェクト" : windowTitle,
                    20, kStrong, std::max(80, W - gridX_ - 375), true);
    drawText(gridX_, 49, "xyo  /  Scratch プロジェクト", 12, kDim);

    const int sw = std::min(326, (W - sideW_) / 2);
    bSearch_ = {W - 24 - sw, 18, sw, 40};
    const bool searchHot = inside(bSearch_, mx_, my_);
    roundRect(bSearch_.x, bSearch_.y, bSearch_.w, bSearch_.h, 12,
              searchActive_ ? Color{0xF1, 0xEF, 0xFF, 255} : kField);
    ringRect(bSearch_.x, bSearch_.y, bSearch_.w, bSearch_.h, 12,
             searchActive_ ? kAccent
                 : searchHot ? Color{0xC9, 0xB8, 0xF0, 255}
                             : Color{0xDE, 0xE4, 0xEE, 255},
             1, searchActive_ ? Color{0xF1, 0xEF, 0xFF, 255} : kField);
    const int sx = bSearch_.x + 17, sy = bSearch_.y + 20;
    ringRect(sx - 5, sy - 6, 11, 11, 6, kDim, 2, kField);
    fill(sx + 5, sy + 5, 6, 2, kDim);
    const int textX = bSearch_.x + 36, textMax = bSearch_.w - 68;
    if (filter_.empty() && !searchActive_)
        drawText(textX, bSearch_.y + 12, "プロジェクトを検索   /", 13,
                 kMuted);
    else {
        drawTextClipped(textX, bSearch_.y + 11, filter_, 14, kFg, textMax);
        int fw = 0;
        textTexture(filter_, 14, kFg, &fw, nullptr);
        if (searchActive_ && fw < textMax && (SDL_GetTicks() / 500) % 2 == 0)
            fill(textX + fw + 2, bSearch_.y + 10, 2, 20, kAccent);
    }
    bClear_ = {};
    if (!filter_.empty()) {
        bClear_ = {bSearch_.x + bSearch_.w - 33, bSearch_.y + 9, 24, 23};
        if (inside(bClear_, mx_, my_))
            roundRect(bClear_.x, bClear_.y, 24, 23, 9,
                      Color{0xE4, 0xE9, 0xF2, 255});
        drawText(bClear_.x + 6, bClear_.y + 3, "x", 15, kDim);
    }

    // ---- path and navigation
    bBack_ = {gridX_, 86, 36, 36};
    bForward_ = {gridX_ + 42, 86, 36, 36};
    bUp_ = {gridX_ + 84, 86, 36, 36};
    button(bBack_, "<", inside(bBack_, mx_, my_), false, !backDirs_.empty());
    button(bForward_, ">", inside(bForward_, mx_, my_), false,
           !forwardDirs_.empty());
    const fs::path parent = fs::path(dir_).parent_path();
    const bool canUp = !parent.empty() && parent != fs::path(dir_);
    button(bUp_, "^", inside(bUp_, mx_, my_), false, canUp);
    bPath_ = {gridX_ + 132, 86, std::max(60, W - gridX_ - 156), 36};
    const bool pathHot = inside(bPath_, mx_, my_);
    const Color pathBg = pathActive_ ? Color{0xF0, 0xED, 0xFF, 255} : kPanel;
    roundRect(bPath_.x, bPath_.y, bPath_.w, bPath_.h, 10, pathBg);
    ringRect(bPath_.x, bPath_.y, bPath_.w, bPath_.h, 10,
             pathActive_ ? kAccent
             : pathHot ? Color{0xC9, 0xB8, 0xF0, 255}
                       : Color{0xDF, 0xE5, 0xEE, 255}, 1, pathBg);
    drawText(bPath_.x + 12, bPath_.y + 10, "/", 14, kAccent, nullptr, true);
    const int pathTextMax = std::max(22, bPath_.w - 101);
    breadcrumbs_.clear();
    if (pathActive_) {
        const std::string display = pathSelectAll_ ? pathInput_ : pathInput_;
        drawTextClipped(bPath_.x + 30, bPath_.y + 11, display,
                        12, kFg, pathTextMax);
        if (pathSelectAll_) {
            const int selW = std::min(pathTextMax, int(display.size()) * 7);
            fill(bPath_.x + 30, bPath_.y + 30, selW, 2, kAccent);
        } else if ((SDL_GetTicks() / 500) % 2 == 0) {
            int tw = 0;
            textTexture(display, 12, kFg, &tw, nullptr);
            if (tw < pathTextMax)
                fill(bPath_.x + 32 + tw, bPath_.y + 9, 2, 18, kAccent);
        }
    } else {
        std::vector<std::pair<std::string, std::string>> parts;
        fs::path running;
        for (const fs::path& part : fs::path(dir_)) {
            running /= part;
            const std::string raw = part.string();
            if (raw.empty()) continue;
            parts.emplace_back(raw == "/" ? "ルート" : raw, running.string());
        }
        // Show the right-most path components that fit, with direct
        // click targets to jump to ancestor folders.
        int available = pathTextMax;
        size_t first = parts.size();
        for (size_t i = parts.size(); i > 0; --i) {
            int tw = 0;
            textTexture(parts[i - 1].first, 12, kFg, &tw, nullptr);
            const int consumed = tw + 25;
            if (consumed > available && first < parts.size()) break;
            available -= consumed;
            first = i - 1;
        }
        int x = bPath_.x + 30;
        if (first > 0) {
            drawText(x, bPath_.y + 11, "…", 12, kDim);
            x += 19;
        }
        for (size_t i = first; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            int tw = 0;
            textTexture(parts[i].first, 12, last ? kStrong : kDim,
                        &tw, nullptr, last);
            const int usable = std::max(0, bPath_.x + bPath_.w - 83 - x);
            if (usable < 8) break;
            const int shownW = std::min(tw, usable);
            drawTextClipped(x, bPath_.y + 11, parts[i].first, 12,
                            last ? kStrong : kDim, usable, last);
            breadcrumbs_.push_back(
                {SDL_Rect{x - 2, bPath_.y + 4, shownW + 5, 27},
                 parts[i].second});
            x += shownW + 8;
            if (!last && x < bPath_.x + bPath_.w - 85) {
                drawText(x, bPath_.y + 11, "›", 12, kMuted);
                x += 15;
            }
        }
    }
    drawText(bPath_.x + bPath_.w - 58, bPath_.y + 12, "⌘ L", 11, kMuted);

    // ---- section heading and sort
    drawText(gridX_, 144, filter_.empty() ? "このフォルダ" : "検索結果",
             18, kStrong, nullptr, true);
    const std::string count = std::to_string(view_.size()) + " 項目";
    int cw = 0;
    textTexture(count, 12, kDim, &cw, nullptr);
    if (W >= 900)
        drawText(gridX_ + 148, 151, count, 12, kDim);
    bSettings_ = {W - 24 - 96, 140, 96, 32};
    bSort_ = {bSettings_.x - 8 - 108, 140, 108, 32};
    bList_ = {bSort_.x - 10 - 64, 140, 64, 32};
    bIcons_ = {bList_.x - 6 - 76, 140, 76, 32};
    button(bIcons_, "アイコン", inside(bIcons_, mx_, my_), !listView_);
    button(bList_, "リスト", inside(bList_, mx_, my_), listView_);
    button(bSort_, sortRecent_ ? "更新日順" : "名前順",
           inside(bSort_, mx_, my_), false);
    button(bSettings_, "実行設定", inside(bSettings_, mx_, my_),
           settingsOpen_);
    // ---- Finder-style list or large thumbnail gallery
    hotCard_ = cardAt(mx_, my_);
    if (listView_) {
        updateThumbs(192, 144);
        drawFinderList(W, H);
    } else {
        updateThumbs(cardW_ - 16, int(std::lround(cardW_ * 0.75)) - 12);
        for (int row = scrollRow_; row < scrollRow_ + rowsVisible_; ++row) {
        if (row >= rowCount()) break;
        const int y = gridTop_ + (row - scrollRow_) * (cardH_ + gap_);
        for (int col = 0; col < cols_; ++col) {
            const int pos = row * cols_ + col;
            if (pos >= int(view_.size())) break;
            const int x = gridX_ + col * (cardW_ + gap_);
            const Entry& e = entries_[size_t(view_[size_t(pos)])];
            const bool selected = view_[size_t(pos)] == sel_;
            const bool hover = pos == hotCard_;
            const int lift = hover && !selected ? 2 : 0;
            const int cy = y - lift;
            // shadow + card
            roundRect(x + 2, cy + 5 + lift, cardW_, cardH_, 16,
                      hover ? Color{0x57, 0x5E, 0x75, 0x33} : kShadow);
            roundRect(x, cy, cardW_, cardH_, 16, kPanel);
            if (selected) {
                ringRect(x, cy, cardW_, cardH_, 16, kAccent, 2, kPanel);
            } else {
                ringRect(x, cy, cardW_, cardH_, 16,
                         hover ? Color{0xC9, 0xB8, 0xF0, 255}
                               : Color{0xE2, 0xE7, 0xF0, 255},
                         1, kPanel);
            }
            const int thumbW = cardW_ - 16;
            const int thumbH = int(std::lround(cardW_ * 0.75)) - 12;
            const int tx = x + 8, ty = cy + 8;
            if (e.isDir) {
                const int cx2 = x + cardW_ / 2;
                const int cy2 = ty + thumbH / 2 + 2;
                if (e.name == "..") {
                    fillCircle(cx2, cy2, 27, Color{0xEE, 0xF2, 0xF8, 255});
                    fillTriangle(cx2, cy2 - 12, cx2 - 10, cy2 + 2, cx2 + 10,
                                 cy2 + 2, kDim);
                    fill(cx2 - 4, cy2 + 2, 8, 10, kDim);
                } else {
                    roundRect(cx2 - 36, cy2 - 22, 72, 46, 9, kFolder);
                    roundRect(cx2 - 36, cy2 - 32, 30, 18, 7, kFolderDark);
                    roundRect(cx2 - 36, cy2 - 30, 72, 20, 8, kFolderDark);
                    roundRect(cx2 - 34, cy2 - 20, 68, 42, 8, kFolderLight);
                    fill(cx2 - 34, cy2 - 20, 68, 8, kFolder);
                }
            } else {
                const Thumb& th = thumbs_[e.path];
                if (th.tex && th.w == thumbW && th.h == thumbH) {
                    SDL_Rect dst{tx, ty, thumbW, thumbH};
                    SDL_RenderCopy(ren_, th.tex, nullptr, &dst);
                    // 角を丸める（画像の四隅をカード色で塗る）
                    for (int c = 0; c < 4; ++c) {
                        const int px = (c & 1) ? tx + thumbW - 1 : tx;
                        const int py = (c & 2) ? ty + thumbH - 1 : ty;
                        fillCircle(px, py, 7, kPanel);
                    }
                    fill(tx, ty, thumbW, 1, Color{0xE2, 0xE7, 0xF0, 255});
                    fill(tx, ty + thumbH - 1, thumbW, 1,
                         Color{0xE2, 0xE7, 0xF0, 255});
                    fill(tx, ty, 1, thumbH, Color{0xE2, 0xE7, 0xF0, 255});
                    fill(tx + thumbW - 1, ty, 1, thumbH,
                         Color{0xE2, 0xE7, 0xF0, 255});
                } else {
                    roundRect(tx, ty, thumbW, thumbH, 8,
                              Color{0xF2, 0xF5, 0xFA, 255});
                    const int cx2 = tx + thumbW / 2, cy2 = ty + thumbH / 2;
                    roundRect(cx2 - 18, cy2 - 14, 36, 28, 5,
                              Color{0xDD, 0xE3, 0xEE, 255});
                    fillTriangle(cx2 - 6, cy2 - 7, cx2 - 6, cy2 + 7, cx2 + 9,
                                 cy2, Color{0xB4, 0xBE, 0xD0, 255});
                }
            }
            const std::string label =
                e.isDir ? (e.name == ".." ? "親フォルダ" : e.name)
                        : fs::path(e.name).stem().string();
            drawTextClipped(x + 12, cy + thumbH + 14, label, 14, kStrong,
                            cardW_ - 24, true);
            std::string meta;
            if (e.isDir)
                meta = e.name == ".." ? "親フォルダ" : "フォルダ";
            else {
                meta = lowerAscii(fs::path(e.name).extension().string()) +
                       "  /  " + humanSize(e.size);
                const std::string d = formatDate(e.mtime);
                if (!d.empty()) meta += "  ·  " + d;
            }
            drawTextClipped(x + 12, cy + thumbH + 32, meta, 11, kDim,
                            cardW_ - 24);
        }
    }

    }
    if (view_.empty()) {
        const std::string msg = filter_.empty()
                                    ? "このフォルダにプロジェクトがありません"
                                    : "一致するプロジェクトがありません";
        int mw = 0, mh = 0;
        textTexture(msg, 15, kDim, &mw, &mh);
        drawText(gridX_ + (W - gridX_ - mw) / 2,
                 gridTop_ + gridH_ / 2 - mh, msg, 15, kDim);
        if (filter_.empty()) {
            const std::string sub =
                ".sb3 / .sb2 をこのフォルダに置いてください";
            int sw2 = 0;
            textTexture(sub, 12, kMuted, &sw2, nullptr);
            drawText(gridX_ + (W - gridX_ - sw2) / 2,
                     gridTop_ + gridH_ / 2 + 6, sub, 12, kMuted);
        }
    }

    if (rowCount() > rowsVisible_) {
        const int trackH = gridH_ - 8;
        const int thumbH = std::max(30, trackH * rowsVisible_ / rowCount());
        const int maxRow = std::max(1, rowCount() - rowsVisible_);
        const int ty = gridTop_ + 4 + (trackH - thumbH) * scrollRow_ / maxRow;
        const int scrollbarX = listView_ ? gridX_ + cardW_ - 7 : W - 12;
        roundRect(scrollbarX, gridTop_ + 4, 4, trackH, 2,
                  Color{0xD3, 0xDA, 0xE6, 255});
        roundRect(scrollbarX, ty, 4, thumbH, 2, Color{0xA8, 0xB1, 0xC2, 255});
    }

    // ---- selected project inspector
    // Use the otherwise empty area below short folders. The normal grid
    // remains full-height and scrollable for directories with many entries.
    const int detailY = gridTop_ + rowCount() * (cardH_ + gap_) + 12;
    const int detailH = H - footerH - 18 - detailY;
    if (!listView_ && detailH >= 184 &&
        sel_ >= 0 && sel_ < int(entries_.size())) {
        const Entry& picked = entries_[size_t(sel_)];
        const int detailX = gridX_, detailW = W - detailX - 24;
        roundRect(detailX + 1, detailY + 3, detailW, detailH, 18, kShadow);
        roundRect(detailX, detailY, detailW, detailH, 18, kPanel);
        ringRect(detailX, detailY, detailW, detailH, 18,
                 Color{0xE2, 0xE7, 0xF1, 255}, 1, kPanel);
        const int previewW = std::min(250, (detailH - 28) * 4 / 3);
        const int previewH = previewW * 3 / 4;
        const SDL_Rect preview{detailX + 16,
                               detailY + (detailH - previewH) / 2,
                               previewW, previewH};
        roundRect(preview.x, preview.y, preview.w, preview.h, 11, kField);
        if (picked.isDir) {
            const int cx = preview.x + preview.w / 2;
            const int cy = preview.y + preview.h / 2;
            roundRect(cx - 37, cy - 24, 74, 48, 8, kFolder);
            roundRect(cx - 37, cy - 33, 32, 18, 6, kFolderDark);
            roundRect(cx - 35, cy - 20, 70, 42, 7, kFolderLight);
        } else {
            auto it = thumbs_.find(picked.path);
            if (it != thumbs_.end() && it->second.tex) {
                SDL_RenderCopy(ren_, it->second.tex, nullptr, &preview);
            } else {
                drawText(preview.x + 18, preview.y + preview.h / 2 - 8,
                         "プレビューを読み込み中", 12, kDim);
            }
        }
        const int dx = preview.x + preview.w + 25;
        const int dw = std::max(32, detailX + detailW - dx - 20);
        drawText(dx, detailY + 21,
                 picked.isDir ? "FOLDER" : "PROJECT PREVIEW",
                 11, kAccent, nullptr, true);
        drawTextClipped(dx, detailY + 48,
                        picked.isDir ? picked.name
                                     : fs::path(picked.name).stem().string(),
                        19, kStrong, dw, true);
        if (!picked.isDir) {
            const std::string meta = lowerAscii(
                fs::path(picked.name).extension().string()) +
                "  ·  " + humanSize(picked.size);
            drawTextClipped(dx, detailY + 83, meta, 13, kDim, dw);
            const std::string date = formatDate(picked.mtime);
            if (!date.empty())
                drawTextClipped(dx, detailY + 107, "更新日  " + date,
                                12, kDim, dw);
        } else {
            drawTextClipped(dx, detailY + 83, "フォルダを開いて内容を表示",
                            12, kDim, dw);
        }
        const int hintY = detailY + detailH - 37;
        fillCircle(dx + 5, hintY + 6, 4, kGreen);
        drawTextClipped(dx + 17, hintY,
                        picked.isDir ? "Enter  フォルダを開く"
                                     : "Enter  実行   ·   E  編集",
                        12, kFg, dw - 17);
    }

    // ---- contextual action bar
    fill(sideW_, H - footerH, W - sideW_, footerH, kPanel);
    fill(sideW_, H - footerH, W - sideW_, 1,
         Color{0xE1, 0xE6, 0xF0, 255});
    const bool hasSel = sel_ >= 0 && sel_ < int(entries_.size());
    const bool projectSel = hasSel && !entries_[size_t(sel_)].isDir;
    const std::string selectedLabel = [&] {
        if (!hasSel) return std::string("プロジェクトを選んでください");
        const Entry& e = entries_[size_t(sel_)];
        if (e.isDir) return std::string("フォルダ: ") +
                            (e.name == ".." ? "親フォルダ" : e.name);
        return e.name + "  /  " + humanSize(e.size) +
               (e.mtime ? "  /  " + formatDate(e.mtime) : "");
    }();
    if (!status_.empty()) {
        fillCircle(gridX_ + 5, H - footerH + 22, 4,
                   statusErr_ ? kWarn : kGreen);
        drawTextClipped(gridX_ + 17, H - footerH + 14,
                        status_, 13, statusErr_ ? kWarn : kFg,
                        W - gridX_ - 38);
    } else {
        drawTextClipped(gridX_, H - footerH + 13, selectedLabel, 13,
                        hasSel ? kStrong : kDim, W - gridX_ - 38);
    }
    const int by = H - 49, bh = 36, space = 8;
    bQuit_ = {W - 20 - 70, by, 70, bh};
    bRun_ = {bQuit_.x - space - 118, by, 118, bh};
    bEdit_ = {bRun_.x - space - 76, by, 76, bh};
    bAotRun_ = {bEdit_.x - space - 88, by, 88, bh};
    bExport_ = {bAotRun_.x - space - 112, by, 112, bh};
#ifdef XYO_JIT
    const bool canAot = projectSel;
#else
    const bool canAot = false;
#endif
    button(bExport_, "AOT 書き出し", inside(bExport_, mx_, my_),
           false, canAot);
    button(bAotRun_, "AOT 実行", inside(bAotRun_, mx_, my_),
           false, canAot);
    button(bEdit_, "編集", inside(bEdit_, mx_, my_), false, projectSel);
    button(bRun_, projectSel ? "実行  ▶" : "フォルダを開く",
           inside(bRun_, mx_, my_), true, hasSel);
    button(bQuit_, "終了", inside(bQuit_, mx_, my_), false);
    if (settingsOpen_) drawSettings(W, H);
}

int Launcher::step() {
    ++frameNo_;
    SDL_Event ev;
    int W = 0, H = 0;
    SDL_GetWindowSize(win_, &W, &H);
    layoutGrid(W, H);
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return 3;
        if (ev.type == SDL_WINDOWEVENT &&
            (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
             ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
            int w = 0, h = 0;
            SDL_GetWindowSize(win_, &w, &h);
            SDL_RenderSetLogicalSize(ren_, w, h);
            layoutGrid(w, h);
            revealSelection();
            continue;
        }
        if (ev.type == SDL_DROPFILE) {
            if (settingsOpen_) {
                SDL_free(ev.drop.file);
                continue;
            }
            const std::string path = ev.drop.file ? ev.drop.file : "";
            SDL_free(ev.drop.file);
            if (!path.empty() && navigatePath(path))
                setStatus("選択しました: " + fs::path(path).filename().string(),
                          false);
            continue;
        }
        if (ev.type == SDL_TEXTINPUT) {
            if (settingsOpen_) continue;
            if (pathActive_) {
                if (pathSelectAll_) {
                    pathInput_.clear();
                    pathSelectAll_ = false;
                }
                if (pathInput_.size() + std::strlen(ev.text.text) < 4096)
                    pathInput_ += ev.text.text;
            } else if (searchActive_) {
                if (filter_.size() + std::strlen(ev.text.text) < 256)
                    filter_ += ev.text.text;
                rebuildView();
                clampSel();
                scrollRow_ = 0;
                revealSelection();
            }
            continue;
        }
        if (ev.type == SDL_KEYDOWN) {
            const SDL_Keycode sym = ev.key.keysym.sym;
            const SDL_Keymod mods = static_cast<SDL_Keymod>(ev.key.keysym.mod);
            const bool ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0;
            if ((ctrl && sym == SDLK_COMMA) || sym == SDLK_F4) {
                toggleSettings();
                continue;
            }
            if (settingsOpen_) {
                if (sym == SDLK_ESCAPE || sym == SDLK_RETURN ||
                    sym == SDLK_KP_ENTER) settingsOpen_ = false;
                continue;
            }
            if (pathActive_) {
                if (sym == SDLK_ESCAPE) setPathActive(false);
                else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
                    finishPathInput();
                else if (sym == SDLK_BACKSPACE) {
                    if (pathSelectAll_) pathInput_.clear();
                    else popUtf8(pathInput_);
                    pathSelectAll_ = false;
                }
                else if (ctrl && sym == SDLK_a) pathSelectAll_ = true;
                else if (ctrl && sym == SDLK_v) {
                    if (char* txt = SDL_GetClipboardText()) {
                        if (pathSelectAll_) pathInput_.clear();
                        pathSelectAll_ = false;
                        if (pathInput_.size() + std::strlen(txt) < 4096)
                            pathInput_ += txt;
                        SDL_free(txt);
                    }
                } else if (sym == SDLK_TAB) setPathActive(false);
                continue;
            }
            if (ctrl && (sym == SDLK_l || sym == SDLK_o)) {
                setPathActive(true);
                continue;
            }
            if (ctrl && sym == SDLK_f) {
                setSearchActive(true);
                continue;
            }
            if (ctrl && sym == SDLK_1) {
                setListView(false);
                continue;
            }
            if (ctrl && sym == SDLK_2) {
                setListView(true);
                continue;
            }
            if (ctrl && sym == SDLK_UP) {
                goUp();
                continue;
            }
            if (ctrl && sym == SDLK_DOWN) {
                activate();
                if (!pending_.empty()) return 1;
                continue;
            }
            if (searchActive_) {
                if (sym == SDLK_ESCAPE) {
                    filter_.clear();
                    setSearchActive(false);
                    rebuildView();
                    clampSel();
                    scrollRow_ = 0;
                    revealSelection();
                } else if (sym == SDLK_BACKSPACE || (ctrl && sym == SDLK_a) ||
                           (ctrl && sym == SDLK_v)) {
                    if (ctrl && sym == SDLK_a) filter_.clear();
                    else if (ctrl && sym == SDLK_v) {
                        if (char* txt = SDL_GetClipboardText()) {
                            if (filter_.size() + std::strlen(txt) < 256)
                                filter_ += txt;
                            SDL_free(txt);
                        }
                    } else popUtf8(filter_);
                    rebuildView();
                    clampSel();
                    scrollRow_ = 0;
                    revealSelection();
                } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                    setSearchActive(false);
                    activate();
                    if (!pending_.empty()) return 1;
                } else if (sym == SDLK_DOWN) {
                    moveSelectionRow(1);
                } else if (sym == SDLK_UP) {
                    moveSelectionRow(-1);
                } else if (sym == SDLK_TAB) {
                    setSearchActive(false);
                }
                continue;
            }
            if ((mods & KMOD_ALT) && sym == SDLK_LEFT) {
                goBack();
                continue;
            }
            if ((mods & KMOD_ALT) && sym == SDLK_RIGHT) {
                goForward();
                continue;
            }
            switch (sym) {
            case SDLK_ESCAPE: return 3;
            case SDLK_F5: loadDir(dir_); break;
            case SDLK_UP: moveSelectionRow(-1); break;
            case SDLK_DOWN: moveSelectionRow(1); break;
            case SDLK_LEFT:
                if (listView_) goUp();
                else moveSelection(-1);
                break;
            case SDLK_RIGHT:
                if (listView_ && sel_ >= 0 &&
                    entries_[size_t(sel_)].isDir) activate();
                else if (!listView_) moveSelection(1);
                break;
            case SDLK_PAGEUP:
                scrollGrid(1);
                moveSelectionRow(-rowsVisible_);
                break;
            case SDLK_PAGEDOWN:
                scrollGrid(-1);
                moveSelectionRow(rowsVisible_);
                break;
            case SDLK_HOME:
                if (!view_.empty()) {
                    sel_ = view_.front();
                    clampSel();
                    scrollRow_ = 0;
                    revealSelection();
                }
                break;
            case SDLK_END:
                if (!view_.empty()) {
                    sel_ = view_.back();
                    clampSel();
                    revealSelection();
                }
                break;
            case SDLK_BACKSPACE: goUp(); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                activate();
                if (!pending_.empty()) return 1;
                break;
            case SDLK_a:
#ifdef XYO_JIT
                exportAot();
#endif
                break;
            case SDLK_e:
                if (sel_ >= 0 && sel_ < int(entries_.size()) &&
                    !entries_[size_t(sel_)].isDir) {
                    pending_ = entries_[size_t(sel_)].path;
                    pendingEdit_ = true;
                    rememberRecent(pending_);
                    return 4;
                }
                setStatus("編集するプロジェクトを選んでください", true);
                break;
            case SDLK_SLASH:
                setSearchActive(true);
                break;
            default: break;
            }
        }
        if (ev.type == SDL_MOUSEMOTION) {
            mx_ = ev.motion.x;
            my_ = ev.motion.y;
        }
        if (ev.type == SDL_MOUSEWHEEL) {
            if (settingsOpen_) continue;
            int W = 0, H = 0;
            SDL_GetWindowSize(win_, &W, &H);
            layoutGrid(W, H);
            int wx = 0, wy = 0;
#if SDL_VERSION_ATLEAST(2, 0, 26)
            wx = ev.wheel.mouseX;
            wy = ev.wheel.mouseY;
#else
            SDL_GetMouseState(&wx, &wy);
#endif
            const bool overGrid = wx >= 0 && wx < W && wy >= gridTop_ &&
                                  wy < gridTop_ + gridH_;
            if (overGrid) {
                int amount = ev.wheel.y;
                if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                    amount = -amount;
                if (amount != 0) scrollGrid(amount);
            }
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN &&
            ev.button.button == SDL_BUTTON_LEFT) {
            const int mx = ev.button.x, my = ev.button.y;
            mx_ = mx;
            my_ = my;
            auto in = [&](const SDL_Rect& r) {
                return r.w > 0 && r.h > 0 && mx >= r.x && mx < r.x + r.w &&
                       my >= r.y && my < r.y + r.h;
            };
            if (settingsOpen_) {
                clickSettings(mx, my);
                continue;
            }
            if (in(bSettings_)) {
                toggleSettings();
                continue;
            }
            if (in(bClear_)) {
                filter_.clear();
                rebuildView();
                clampSel();
                scrollRow_ = 0;
                revealSelection();
                continue;
            }
            if (in(bSearch_)) { setSearchActive(true); continue; }
            bool clickedCrumb = false;
            if (!pathActive_) {
                for (const auto& [rect, path] : breadcrumbs_) {
                    if (in(rect)) {
                        navigatePath(path);
                        clickedCrumb = true;
                        break;
                    }
                }
            }
            if (clickedCrumb) continue;
            if (in(bPath_)) { setPathActive(true); continue; }
            if (in(bIcons_)) { setListView(false); continue; }
            if (in(bList_)) { setListView(true); continue; }
            if (in(bBack_)) { goBack(); continue; }
            if (in(bForward_)) { goForward(); continue; }
            if (in(bUp_)) { goUp(); continue; }
            if (in(bHome_)) { navigatePath(homeDir_); continue; }
            if (in(bDesktop_)) {
                navigatePath((fs::path(homeDir_) / "Desktop").string());
                continue;
            }
            if (in(bDocuments_)) {
                navigatePath((fs::path(homeDir_) / "Documents").string());
                continue;
            }
            if (in(bDownloads_)) {
                navigatePath((fs::path(homeDir_) / "Downloads").string());
                continue;
            }
            if (in(bExamples_)) { navigatePath(examplesDir_); continue; }
            if (in(bSort_)) {
                sortRecent_ = !sortRecent_;
                rebuildView();
                clampSel();
                scrollRow_ = 0;
                revealSelection();
                continue;
            }
            bool handledRecent = false;
            for (size_t i = 0; i < recent_.size() && i < 5; ++i) {
                if (!in(bRecent_[i])) continue;
                const std::string path = recent_[i];
                if (navigatePath(path) && ev.button.clicks >= 2) {
                    activate();
                    if (!pending_.empty()) return 1;
                }
                handledRecent = true;
                break;
            }
            if (handledRecent) continue;
            setPathActive(false);
            if (in(bRun_)) {
                activate();
                if (!pending_.empty()) return 1;
                continue;
            }
            if (in(bAotRun_)) {
                if (sel_ < 0 || entries_[size_t(sel_)].isDir) continue;
                activate();
                if (!pending_.empty()) {
                    pendingAot_ = true;
                    return 2;
                }
                setStatus("AOT 実行するプロジェクトを選んでください", true);
                continue;
            }
            if (in(bExport_)) {
                if (sel_ < 0 || entries_[size_t(sel_)].isDir) continue;
                exportAot();
                continue;
            }
            if (in(bEdit_)) {
                if (sel_ >= 0 && sel_ < int(entries_.size()) &&
                    !entries_[size_t(sel_)].isDir) {
                    pending_ = entries_[size_t(sel_)].path;
                    pendingEdit_ = true;
                    rememberRecent(pending_);
                    return 4;
                }
                setStatus("編集するプロジェクトを選んでください", true);
                continue;
            }
            if (in(bQuit_)) return 3;
            const int pos = cardAt(mx, my);
            if (pos >= 0) {
                setSearchActive(false);
                sel_ = view_[size_t(pos)];
                clampSel();
                if (ev.button.clicks >= 2) {
                    activate();
                    if (!pending_.empty()) return 1;
                }
                continue;
            }
            setSearchActive(false);
        }
    }

    // 自動検証用アクション（1 フレーム 1 つ）
    if (!actions_.empty() && actIdx_ < actions_.size()) {
        const std::string& a = actions_[actIdx_++];
        if (a == "down") moveSelectionRow(1);
        else if (a == "up") moveSelectionRow(-1);
        else if (a == "home") {
            if (!view_.empty()) {
                sel_ = view_.front();
                clampSel();
                scrollRow_ = 0;
                revealSelection();
            }
        } else if (a == "end") {
            if (!view_.empty()) {
                sel_ = view_.back();
                clampSel();
                revealSelection();
            }
        } else if (a == "enter" || a == "run") {
            activate();
            if (!pending_.empty()) return 1;
        } else if (a == "aotrun") {
            activate();
            if (!pending_.empty()) { pendingAot_ = true; return 2; }
        } else if (a == "export") {
            exportAot();
        } else if (a == "edit") {
            if (sel_ >= 0 && sel_ < int(entries_.size()) &&
                !entries_[size_t(sel_)].isDir) {
                pending_ = entries_[size_t(sel_)].path;
                pendingEdit_ = true;
                rememberRecent(pending_);
                return 4;
            }
        } else if (a == "back") {
            goUp();
        } else if (a == "backnav") {
            goBack();
        } else if (a == "forward") {
            goForward();
        } else if (a == "browse-home") {
            if (!homeDir_.empty()) navigatePath(homeDir_);
        } else if (a == "examples") {
            if (!examplesDir_.empty()) navigatePath(examplesDir_);
        } else if (a == "sort") {
            sortRecent_ = !sortRecent_;
            rebuildView();
        } else if (a == "settings") {
            toggleSettings();
        } else if (a.rfind("scale:", 0) == 0) {
            int value = std::atoi(a.c_str() + 6);
            if (value >= 1 && value <= 4) {
                options_.scale = value;
                optionsChanged();
            }
        } else if (a.rfind("fps:", 0) == 0) {
            int value = std::atoi(a.c_str() + 4);
            if (value == -1 || value == 30 || value == 60 || value == 120) {
                options_.fps = value;
                optionsChanged();
            }
        } else if (a.rfind("jit:", 0) == 0) {
#ifdef XYO_JIT
            const std::string mode = a.substr(4);
            if (mode == "hotspot" || mode == "eager" || mode == "off") {
                options_.jitMode = mode == "hotspot" ? 0 :
                                   mode == "eager" ? 1 : 2;
                optionsChanged();
            }
#endif
        } else if (a.rfind("lazy:", 0) == 0) {
#ifdef XYO_JIT
            const std::string mode = a.substr(5);
            if (mode == "auto" || mode == "on" || mode == "off") {
                options_.lazyMode = mode == "auto" ? 0 :
                                    mode == "on" ? 1 : 2;
                optionsChanged();
            }
#endif
        } else if (a.rfind("gpu:", 0) == 0) {
            const std::string mode = a.substr(4);
            if (mode == "off" || mode == "ray" || mode == "path") {
                options_.gpuMode = mode == "off" ? -1 :
                                   mode == "ray" ? 0 : 1;
                optionsChanged();
            }
        } else if (a.rfind("turbo:", 0) == 0) {
            options_.turbo = a.substr(6) == "1";
            optionsChanged();
        } else if (a.rfind("danger:", 0) == 0) {
            options_.danger = a.substr(7) == "1";
        } else if (a == "view-list") {
            setListView(true);
        } else if (a == "view-icons") {
            setListView(false);
        } else if (a.rfind("click:", 0) == 0) {
            int x = 0, y = 0;
            if (std::sscanf(a.c_str() + 6, "%d:%d", &x, &y) == 2 &&
                x >= 0 && y >= 0) {
                SDL_Event click{};
                click.type = SDL_MOUSEBUTTONDOWN;
                click.button.button = SDL_BUTTON_LEFT;
                click.button.clicks = 1;
                click.button.x = x;
                click.button.y = y;
                SDL_PushEvent(&click);
            }
        } else if (a.rfind("resize:", 0) == 0) {
            int w = 0, h = 0;
            if (std::sscanf(a.c_str() + 7, "%dx%d", &w, &h) == 2 &&
                w >= 760 && w <= 4096 && h >= 560 && h <= 2160) {
                SDL_SetWindowSize(win_, w, h);
                SDL_RenderSetLogicalSize(ren_, w, h);
                layoutGrid(w, h);
            }
        } else if (a.rfind("path:", 0) == 0) {
            navigatePath(a.substr(5));
        } else if (a.rfind("search:", 0) == 0) {
            filter_ = a.substr(7);
            rebuildView();
            clampSel();
            revealSelection();
        } else if (a == "recent0" && !recent_.empty()) {
            navigatePath(recent_.front());
        } else if (a == "wheeldown") {
            scrollGrid(-1);
        } else if (a == "wheelup") {
            scrollGrid(1);
        } else if (a == "quit") {
            return 3;
        }
    }

    draw();
    SDL_RenderPresent(ren_);
    return 0;
}

void Launcher::captureNow() {
    if (shotPath_.empty() || !ren_ || !win_) return;
    int w = 0, h = 0;
    // ReadPixels operates on physical pixels even with a logical render
    // size, so allocate the actual framebuffer dimensions on Retina Macs.
    if (SDL_GetRendererOutputSize(ren_, &w, &h) != 0) return;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
        0, w, h, 24, SDL_PIXELFORMAT_RGB24);
    if (!surf) return;
    if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGB24, surf->pixels,
                             surf->pitch) == 0) {
        if (std::FILE* f = std::fopen(shotPath_.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            std::fwrite(surf->pixels, 1,
                        size_t(surf->h) * size_t(surf->pitch), f);
            std::fclose(f);
        }
    }
    SDL_FreeSurface(surf);
}

std::vector<std::string> splitActions(const char* spec) {
    std::vector<std::string> out;
    if (!spec || !*spec) return out;
    std::string s = spec;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t c = s.find(',', pos);
        std::string tok =
            s.substr(pos, c == std::string::npos ? std::string::npos
                                                 : c - pos);
        if (!tok.empty()) out.push_back(tok);
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    return out;
}

// AOT playback needs a short-lived object file. The standard temporary
// directory can be read-only in sandboxes; use the user's state directory
// first and never share a fixed filename with another running instance.
struct TemporaryAotObject {
    std::string path;

    TemporaryAotObject() {
        std::vector<fs::path> dirs;
        const std::string settings = settingsFilePath();
        if (!settings.empty())
            dirs.push_back(fs::path(settings).parent_path());
        std::error_code ec;
        const fs::path systemTmp = fs::temp_directory_path(ec);
        if (!ec) dirs.push_back(systemTmp);

        for (const fs::path& dir : dirs) {
            ec.clear();
            fs::create_directories(dir, ec);
            if (ec) continue;
            for (int i = 0; i < 8; ++i) {
                const auto stamp = std::chrono::steady_clock::now()
                                       .time_since_epoch().count();
                const uint64_t id = static_cast<uint64_t>(stamp) ^
                    static_cast<uint64_t>(std::hash<std::thread::id>{}(
                        std::this_thread::get_id())) ^ uint64_t(i);
                const fs::path candidate =
                    dir / ("xyo-gui-aot-" + std::to_string(id) + ".xyo");
                if (fs::exists(candidate, ec) || ec) continue;
                std::ofstream test(candidate, std::ios::binary);
                if (!test) continue;
                test.close();
                if (!test) {
                    fs::remove(candidate, ec);
                    continue;
                }
                path = candidate.string();
                return;
            }
        }
    }

    ~TemporaryAotObject() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
    TemporaryAotObject(const TemporaryAotObject&) = delete;
    TemporaryAotObject& operator=(const TemporaryAotObject&) = delete;
};

// Execution flags only affect the selected project, never the launcher
// thumbnail worker, future edits, or subsequent projects after returning.
class ScopedGuiRunFlags {
public:
    explicit ScopedGuiRunFlags(const GuiRunOptions& flags,
                               bool explicitAot = false) {
        if (const char* original = std::getenv("XYO_DANGER")) {
            hadDanger_ = true;
            previousDanger_ = original;
        }
#ifdef _WIN32
        _putenv_s("XYO_DANGER", flags.danger ? "1" : "0");
#else
        setenv("XYO_DANGER", flags.danger ? "1" : "0", 1);
#endif
#ifdef XYO_JIT
        oldMode_ = jitMode();
        oldLazy_ = jitLazyMode();
        // AOT is explicitly requested: its precompiled scripts must be
        // eligible for dispatch even if regular JIT is configured OFF.
        // Eager always picks the populated AOT code cache immediately.
        setJitMode(explicitAot ? JitMode::Eager :
                   flags.jitMode == 1 ? JitMode::Eager :
                   flags.jitMode == 2 ? JitMode::Off :
                                         JitMode::HotSpot);
        setJitLazyMode(flags.lazyMode == 1 ? JitLazyMode::On :
                       flags.lazyMode == 2 ? JitLazyMode::Off :
                                             JitLazyMode::Auto);
#endif
    }

    ScopedGuiRunFlags(const ScopedGuiRunFlags&) = delete;
    ScopedGuiRunFlags& operator=(const ScopedGuiRunFlags&) = delete;

    ~ScopedGuiRunFlags() {
#ifdef XYO_JIT
        setJitMode(oldMode_);
        setJitLazyMode(oldLazy_);
#endif
#ifdef _WIN32
        _putenv_s("XYO_DANGER", hadDanger_ ? previousDanger_.c_str() : "");
#else
        if (hadDanger_) setenv("XYO_DANGER", previousDanger_.c_str(), 1);
        else unsetenv("XYO_DANGER");
#endif
    }
private:
    bool hadDanger_ = false;
    std::string previousDanger_;
#ifdef XYO_JIT
    JitMode oldMode_ = JitMode::HotSpot;
    JitLazyMode oldLazy_ = JitLazyMode::Auto;
#endif
};

} // namespace

int guiMain(const char* startDir) {
    std::string start;
    if (startDir && *startDir) start = startDir;
    if (start.empty()) {
        if (const char* env = std::getenv("XYO_PROJECT_DIR"))
            if (*env) start = env;
    }
    if (start.empty()) {
        std::error_code ec;
        start = fs::current_path(ec).string();
        if (!hasProject(start)) {
            if (const char* home = std::getenv("HOME"))
                if (home && *home) start = home;
        }
    }
    auto actions = splitActions(std::getenv("XYO_GUI_ACTIONS"));
    // 検証モード: アクション列を与えたら 1 回の再生でプロセスを終える
    // （そうしないとランチャーが再帰的に開いてテストが止まらない）。
    const bool oneShot = !actions.empty();
    const char* dbg = std::getenv("XYO_GUI_DEBUG");

    std::string runPath;
    bool runAot = false;
    GuiRunOptions options = readRunOptions();
    for (;;) {
        Launcher l;
        l.setOptions(options);
        if (!l.open()) {
            std::fprintf(stderr, "gui: ウィンドウを作成できません\n");
            return 1;
        }
        l.openDir(start);
        const int exitFrames = [] {
            if (const char* v = std::getenv("XYO_GUI_EXIT_FRAMES")) return std::atoi(v);
            return 0;
        }();
        if (const char* sh = std::getenv("XYO_GUI_SHOT")) l.setShot(sh);
        if (oneShot) l.setActions(actions);
        int act = 0;
        double frameSum = 0, frameMax = 0;
        int frameCount = 0;
        for (long frame = 0;; ++frame) {
            const auto t0 = std::chrono::steady_clock::now();
            act = l.step();
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            frameSum += ms;
            frameMax = std::max(frameMax, ms);
            ++frameCount;
            if (std::getenv("XYO_GUI_DEBUG") && ms > 20.0)
                std::fprintf(stderr, "gui: slow frame %ld %.1fms\n", frame,
                             ms);
            if (act != 0) break;
            if (exitFrames > 0 && frame + 1 >= exitFrames) break;
            if (!actions.empty() && frame > 200) break;
        }
        if (const char* dbg = std::getenv("XYO_GUI_DEBUG"))
            if (*dbg && *dbg != '0' && frameCount > 0)
                std::fprintf(stderr,
                             "gui: step avg=%.2fms max=%.2fms frames=%d\n",
                             frameSum / frameCount, frameMax, frameCount);
        l.captureNow();
        runPath = l.pendingPath();
        runAot = l.pendingAot();
        const bool runEdit = l.pendingEdit();
        options = l.options();
        l.close();
        if (act == 3) return 0;
        if (act == 4) {
            if (!runPath.empty()) {
                editorMain(runPath.c_str());
            }
            if (oneShot) return 0;
            continue;
        }
        if (runPath.empty()) return 0;
        if (dbg) std::fprintf(stderr,
            "gui: play %s (aot=%d scale=%d fps=%d turbo=%d "
            "jit=%d lazy=%d gpu=%d danger=%d)\n",
            runPath.c_str(), int(runAot), options.scale, options.fps,
            int(options.turbo), options.jitMode, options.lazyMode,
            options.gpuMode, int(options.danger));
        ScopedGuiRunFlags flags(options, runAot);
        if (runAot) {
#ifdef XYO_JIT
            // 事前コンパイルしてオブジェクト経由で再生する
            std::vector<uint8_t> data;
            std::FILE* f = std::fopen(runPath.c_str(), "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (n > 0) {
                    data.resize(size_t(n));
                    if (std::fread(data.data(), 1, data.size(), f) !=
                        data.size())
                        data.clear();
                }
                std::fclose(f);
            }
            Runtime rt;
            if (!data.empty() &&
                loadSb3File(rt, data.data(), data.size()).ok) {
                const uint64_t hash =
                    aot::hashBytes(data.data(), data.size());
                aot::BuildResult br;
                std::string why;
                if (aot::compileProject(rt, hash, br, &why)) {
                    TemporaryAotObject temp;
                    if (temp.path.empty()) {
                        why = "cannot allocate temporary AOT file";
                    } else if (aot::writeObjectFile(temp.path, br, &why)) {
                        xyoShellPlay(runPath.c_str(), temp.path.c_str(),
                                     options.scale, options.turbo,
                                     options.fps, options.gpuMode);
                        if (oneShot) return 0;
                        continue;
                    }
                }
                std::fprintf(stderr, "gui: AOT 実行に失敗 (%s)\n",
                             why.c_str());
            }
#endif
            // AOT 不可なら通常再生へフォールバック
            xyoShellPlay(runPath.c_str(), nullptr, options.scale,
                         options.turbo, options.fps, options.gpuMode);
            if (oneShot) return 0;
            continue;
        }
        xyoShellPlay(runPath.c_str(), nullptr, options.scale,
                         options.turbo, options.fps, options.gpuMode);
        if (oneShot) return 0;
        // 再生後にランチャーへ戻る（start は据え置き）
    }
}

} // namespace xyo

#else // !XYO_HAVE_SDL2

#include <cstdio>

namespace xyo {
int guiMain(const char*) {
    std::fprintf(stderr,
                 "xyo was built without SDL2 support (install SDL2 and "
                 "rebuild to use 'xyo gui')\n");
    return 1;
}
} // namespace xyo

#endif
