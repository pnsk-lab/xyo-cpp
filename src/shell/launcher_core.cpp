// xyo-cpp: ランチャー GUI の SDL/Qt 非依存コア実装。
// （旧 gui.cpp から UI 描画・入力を除いた部分を移植）
#include "launcher_core.hpp"

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

#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#include "../../third_party/stb_image.h"
#include "../../third_party/nanosvg.h"
#include "../../third_party/nanosvgrast.h"

namespace xyo {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 実行設定の永続化
// ---------------------------------------------------------------------------

static std::string settingsFilePath() {
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

// ---------------------------------------------------------------------------
// パスユーティリティ
// ---------------------------------------------------------------------------

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

std::string lowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
        out.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

std::string humanSize(uint64_t n) {
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

// ---------------------------------------------------------------------------
// ディレクトリ列挙
// ---------------------------------------------------------------------------

bool listProjectDir(const std::string& dir,
                    std::vector<LauncherEntry>& out,
                    std::string* normalizedDir) {
    std::error_code ec;
    const std::string abs = fs::absolute(dir, ec).lexically_normal().string();
    if (ec || !fs::is_directory(abs, ec)) return false;
    if (normalizedDir) *normalizedDir = abs;

    std::vector<LauncherEntry> dirs, files;
    fs::directory_iterator it(abs, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(e2)) {
            LauncherEntry e;
            e.name = name;
            e.path = it->path().string();
            e.isDir = true;
            dirs.push_back(std::move(e));
        } else if (it->is_regular_file(e2)) {
            const std::string ext = it->path().extension().string();
            if (ext != ".sb3" && ext != ".sb2") continue;
            LauncherEntry e;
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
    auto byName = [](const LauncherEntry& a, const LauncherEntry& b) {
        return a.name < b.name;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (auto& e : dirs) out.push_back(std::move(e));
    for (auto& e : files) out.push_back(std::move(e));
    return !ec;
}

// ---------------------------------------------------------------------------
// 最近開いたプロジェクト
// ---------------------------------------------------------------------------

std::vector<std::string> loadRecentProjects() {
    std::vector<std::string> recent;
    const std::string filename = recentHistoryPath();
    if (filename.empty()) return recent;
    std::ifstream stream(filename);
    std::string path;
    while (recent.size() < 5 && std::getline(stream, path)) {
        if (path.empty() || path.size() > 4096) continue;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        const std::string ext = lowerAscii(fs::path(path).extension().string());
        if (ext != ".sb3" && ext != ".sb2") continue;
        if (std::find(recent.begin(), recent.end(), path) == recent.end())
            recent.push_back(path);
    }
    return recent;
}

void rememberRecentProject(std::vector<std::string>& recent,
                           const std::string& path) {
    if (path.empty() || path.find('\n') != std::string::npos ||
        path.find('\r') != std::string::npos) return;
    recent.erase(std::remove(recent.begin(), recent.end(), path),
                 recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 5) recent.resize(5);
    const std::string filename = recentHistoryPath();
    if (filename.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(filename).parent_path(), ec);
    if (ec) return;
    const std::string tmp = filename + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        for (const std::string& item : recent) f << item << '\n';
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

// ---------------------------------------------------------------------------
// コスチュームデコード / ステージ合成サムネイル
// ---------------------------------------------------------------------------

bool decodeCostumeRGBA(const Costume& c, std::vector<uint8_t>& out, int& w,
                       int& h, double* logicalW, double* logicalH,
                       int maxDim) {
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
        const float scale = std::min(1.0f, float(maxDim) / longest);
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

// ---------------------------------------------------------------------------
// サムネイルのディスクキャッシュ
// ---------------------------------------------------------------------------

static uint64_t fnv1aHash(const std::string& s,
                          uint64_t h = 1469598103934665603ull) {
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

std::string thumbCacheFile(const std::string& dir, const ThumbJobKey& j) {
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

} // namespace xyo
