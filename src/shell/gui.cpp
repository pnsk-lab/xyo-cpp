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

// Scratch-gui 由来の明るい配色（エディタと統一）。
constexpr Color kBg{0xE9, 0xEE, 0xF2, 255};
constexpr Color kPanel{0xFF, 0xFF, 0xFF, 255};
constexpr Color kFg{0x57, 0x5E, 0x75, 255};
constexpr Color kStrong{0x2B, 0x32, 0x47, 255};
constexpr Color kDim{0x86, 0x91, 0xA7, 255};
constexpr Color kMuted{0xA8, 0xB1, 0xC2, 255};
constexpr Color kBorder{0x00, 0x00, 0x00, 0x26};
constexpr Color kField{0xF1, 0xF4, 0xF9, 255};
constexpr Color kAccent{0x85, 0x5C, 0xD6, 255};
constexpr Color kAccentHot{0x71, 0x4E, 0xB6, 255};
constexpr Color kFolder{0xF2, 0xC4, 0x6D, 255};
constexpr Color kFolderDark{0xE0, 0xAE, 0x4F, 255};
constexpr Color kFolderLight{0xF8, 0xD9, 0x96, 255};
constexpr Color kGreen{0x3F, 0xB2, 0x6B, 255};
constexpr Color kWarn{0xE0, 0x5B, 0x52, 255};
constexpr Color kShadow{0x57, 0x5E, 0x75, 0x24};

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
    void setStatus(const std::string& s, bool err);
    void openDir(const std::string& d) { loadDir(d); }
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
                bool accent);
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

void Launcher::loadDir(const std::string& dir) {
    // 絶対パスに正規化してから列挙する（相対パスの二重解決を防ぐ）
    std::error_code ec;
    dir_ = fs::absolute(dir, ec).lexically_normal().string();
    if (ec) dir_ = dir;
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
    const fs::path parent = fs::path(dir_).parent_path();
    if (!parent.empty() && parent != fs::path(dir_)) {
        Entry up;
        up.name = "..";
        up.path = parent.string();
        up.isDir = true;
        entries_.push_back(std::move(up));
    }
    for (auto& e : dirs) entries_.push_back(std::move(e));
    for (auto& e : files) entries_.push_back(std::move(e));
    rebuildView();
    clampSel();
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
void Launcher::layoutGrid(int W, int H) {
    const int headerH = 72;
    const int footerH = 76;
    gridTop_ = headerH + 40;
    gridH_ = std::max(80, H - gridTop_ - footerH - 10);
    gridX_ = 20;
    gap_ = 18;
    cols_ = std::clamp((W - 2 * gridX_ + gap_) / (200 + gap_), 2, 6);
    cardW_ = std::max(120, (W - 2 * gridX_ - (cols_ - 1) * gap_) / cols_);
    const int thumbH = int(std::lround(cardW_ * 0.75));
    cardH_ = thumbH + 50;
    // Whole rows only: cards never straddle the header or footer, so no
    // renderer clip is needed (SDL2-compat's GL backend ignores clip rects).
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
    scrollRow_ -= notches;
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
    searchActive_ = on;
    if (on)
        SDL_StartTextInput();
    else
        SDL_StopTextInput();
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
    win_ = SDL_CreateWindow("xyo — プロジェクトを開く",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            980, 660, SDL_WINDOW_RESIZABLE);
    if (!win_) return false;
    SDL_SetWindowMinimumSize(win_, 760, 520);
    ren_ = SDL_CreateRenderer(
        win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
    if (!ren_) return false;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    startThumbs();
    return true;
}

void Launcher::close() {
    setSearchActive(false);
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
        loadDir(e.path);
        scrollRow_ = 0;
        revealSel_ = true;
        return;
    }
    pending_ = e.path;
    pendingAot_ = false;
}

/// カードグリッド上の座標 → view_ の添字（無ければ -1）。
int Launcher::cardAt(int x, int y) const {
    if (y < gridTop_ || y >= gridTop_ + gridH_) return -1;
    if (x < gridX_ || x >= gridX_ + cols_ * (cardW_ + gap_)) return -1;
    const int col = (x - gridX_) / (cardW_ + gap_);
    if (col < 0 || col >= cols_) return -1;
    const int localY = y - gridTop_;
    const int row = scrollRow_ + localY / (cardH_ + gap_);
    if (row < 0) return -1;
    const int pos = row * cols_ + col;
    if (pos < 0 || pos >= int(view_.size())) return -1;
    return pos;
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
    if (t.failed || t.requested) return;
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
                      bool accent) {
    const Color bg = accent ? (hot ? kAccentHot : kAccent) : kPanel;
    const Color fg = accent ? Color{255, 255, 255, 255} : kFg;
    roundRect(r.x, r.y, r.w, r.h, r.h / 2, bg);
    if (!accent) {
        ringRect(r.x, r.y, r.w, r.h, r.h / 2,
                 hot ? Color{0xC9, 0xB8, 0xF0, 255} : Color{0xDA, 0xE0, 0xEA, 255},
                 1, bg);
    }
    int tw = 0, th = 0;
    textTexture(label, 14, fg, &tw, &th, accent);
    drawText(r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, label, 14, fg,
             nullptr, accent);
}

void Launcher::draw() {
    int W = 980, H = 660;
    SDL_GetWindowSize(win_, &W, &H);
    layoutGrid(W, H);
    const int headerH = 72;
    const int footerH = 76;
    // Only follow the selection when it actually changed; otherwise the view
    // would snap back and mouse-wheel scrolling would be impossible.
    if (revealSel_) {
        if (sel_ >= 0) {
            const auto it = std::find(view_.begin(), view_.end(), sel_);
            if (it != view_.end()) {
                const int pos = int(it - view_.begin());
                const int row = pos / cols_;
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

    // ---- header
    fill(0, 0, W, headerH, kPanel);
    fill(0, headerH - 1, W, 1, Color{0xDF, 0xE5, 0xEE, 255});
    roundRect(20, 20, 32, 32, 9, kAccent);
    {
        int mw = 0, mh = 0;
        textTexture("X", 18, Color{255, 255, 255, 255}, &mw, &mh, true);
        drawText(20 + (32 - mw) / 2, 20 + (32 - mh) / 2, "X", 18,
                 Color{255, 255, 255, 255}, nullptr, true);
    }
    drawText(62, 17, "XYO", 17, kStrong, nullptr, true);
    drawText(63, 38, "プロジェクト", 12, kDim);

    // search pill
    const int sw = std::min(360, W / 2);
    bSearch_ = {W - 20 - sw, 17, sw, 38};
    const bool searchHot = mx_ >= bSearch_.x && mx_ < bSearch_.x + bSearch_.w &&
                           my_ >= bSearch_.y && my_ < bSearch_.y + bSearch_.h;
    roundRect(bSearch_.x, bSearch_.y, bSearch_.w, bSearch_.h, 19, kField);
    ringRect(bSearch_.x, bSearch_.y, bSearch_.w, bSearch_.h, 19,
             searchActive_ ? kAccent
                           : (searchHot ? Color{0xC9, 0xB8, 0xF0, 255}
                                        : Color{0xDE, 0xE4, 0xEE, 255}),
             1, kField);
    {
        const int cx = bSearch_.x + 20, cy = bSearch_.y + 19;
        ringRect(cx - 7, cy - 7, 13, 13, 7, kDim, 2, kField);
        for (int i = 0; i < 5; ++i) fill(cx + 4 + i, cy + 4 + i, 2, 2, kDim);
    }
    const int textX = bSearch_.x + 40;
    const int textMax = bSearch_.w - 40 - 34;
    if (filter_.empty() && !searchActive_) {
        drawText(textX, bSearch_.y + 10, "検索 ( / )", 14, kMuted);
    } else {
        drawTextClipped(textX, bSearch_.y + 10, filter_, 14, kFg, textMax);
        int fw = 0;
        textTexture(filter_, 14, kFg, &fw, nullptr);
        if (searchActive_ && fw < textMax && (SDL_GetTicks() / 500) % 2 == 0)
            fill(textX + fw + 1, bSearch_.y + 9, 2, 19, kFg);
    }
    bClear_ = {};
    if (!filter_.empty()) {
        bClear_ = {bSearch_.x + bSearch_.w - 32, bSearch_.y + 9, 22, 20};
        const bool ch = mx_ >= bClear_.x && mx_ < bClear_.x + bClear_.w &&
                        my_ >= bClear_.y && my_ < bClear_.y + bClear_.h;
        if (ch) roundRect(bClear_.x, bClear_.y, bClear_.w, bClear_.h, 10,
                          Color{0xE4, 0xE9, 0xF2, 255});
        const int cx = bClear_.x + 11, cy = bClear_.y + 10;
        for (int i = -4; i <= 4; ++i) {
            fill(cx + i, cy + i, 2, 2, kDim);
            fill(cx + i, cy - i, 2, 2, kDim);
        }
    }

    // ---- path row
    {
        int cw = 0;
        char count[64];
        std::snprintf(count, sizeof(count), "%zu 件", view_.size());
        textTexture(count, 12, kDim, &cw, nullptr);
        const int avail = std::max(120, W - 40 - cw - 16);
        std::string shown = ellipsizeLeft(dir_, size_t(std::max(8, avail / 7)));
        // ホームアイコン
        const int hx = 20, hy = headerH + 10;
        roundRect(hx + 2, hy + 6, 12, 9, 2, kDim);
        fillTriangle(hx, hy + 7, hx + 8, hy, hx + 16, hy + 7, kDim);
        drawText(hx + 24, hy, shown, 12, kDim);
        drawText(W - 20 - cw, hy, count, 12, kDim);
    }

    // ---- grid (whole rows only)
    hotCard_ = cardAt(mx_, my_);
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
                e.isDir && e.name != ".." ? e.name + "/" : e.name;
            drawTextClipped(x + 12, cy + thumbH + 14, label, 14, kStrong,
                            cardW_ - 24, true);
            std::string meta;
            if (e.isDir)
                meta = e.name == ".." ? "親フォルダ" : "フォルダ";
            else {
                meta = humanSize(e.size);
                const std::string d = formatDate(e.mtime);
                if (!d.empty()) meta += "  ·  " + d;
            }
            drawTextClipped(x + 12, cy + thumbH + 32, meta, 11, kDim,
                            cardW_ - 24);
        }
    }

    if (view_.empty()) {
        const std::string msg = filter_.empty()
                                    ? "このフォルダにプロジェクトがありません"
                                    : "一致するプロジェクトがありません";
        int mw = 0, mh = 0;
        textTexture(msg, 15, kDim, &mw, &mh);
        drawText((W - mw) / 2, gridTop_ + gridH_ / 2 - mh, msg, 15, kDim);
        if (filter_.empty()) {
            const std::string sub =
                ".sb3 / .sb2 をこのフォルダに置いてください";
            int sw2 = 0;
            textTexture(sub, 12, kMuted, &sw2, nullptr);
            drawText((W - sw2) / 2, gridTop_ + gridH_ / 2 + 6, sub, 12,
                     kMuted);
        }
    }

    if (rowCount() > rowsVisible_) {
        const int trackH = gridH_ - 8;
        const int thumbH = std::max(30, trackH * rowsVisible_ / rowCount());
        const int maxRow = std::max(1, rowCount() - rowsVisible_);
        const int ty = gridTop_ + 4 + (trackH - thumbH) * scrollRow_ / maxRow;
        roundRect(W - 12, gridTop_ + 4, 4, trackH, 2,
                  Color{0xD3, 0xDA, 0xE6, 255});
        roundRect(W - 12, ty, 4, thumbH, 2, Color{0xA8, 0xB1, 0xC2, 255});
    }

    // ---- footer
    fill(0, H - footerH, W, footerH, kPanel);
    fill(0, H - footerH, W, 1, Color{0xDF, 0xE5, 0xEE, 255});
    const int statusY = H - footerH + 14;
    if (!status_.empty()) {
        fillCircle(24, statusY + 8, 4, statusErr_ ? kWarn : kGreen);
        SDL_Rect clip2{34, statusY - 3, W - 60, 24};
        SDL_RenderSetClipRect(ren_, &clip2);
        drawText(36, statusY, status_, 13, statusErr_ ? kWarn : kDim);
        SDL_RenderSetClipRect(ren_, nullptr);
    } else {
        drawText(20, statusY,
                 "ダブルクリック / Enter で実行    E 編集    A AOT 書き出し    "
                 "Backspace 親フォルダ    / 検索",
                 12, kMuted);
    }

    const int by = H - footerH + 34;
    const int bh = 34;
    bQuit_ = {W - 20 - 78, by, 78, bh};
    bRun_ = {bQuit_.x - 10 - 96, by, 96, bh};
    bEdit_ = {bRun_.x - 10 - 78, by, 78, bh};
    bAotRun_ = {bEdit_.x - 10 - 96, by, 96, bh};
    bExport_ = {bAotRun_.x - 10 - 118, by, 118, bh};
    auto hot = [&](const SDL_Rect& r) {
        return mx_ >= r.x && mx_ < r.x + r.w && my_ >= r.y && my_ < r.y + r.h;
    };
    button(bExport_, "AOT 書き出し", hot(bExport_), false);
    button(bAotRun_, "AOT 実行", hot(bAotRun_), false);
    button(bEdit_, "編集", hot(bEdit_), false);
    button(bRun_, "開く", hot(bRun_), true);
    button(bQuit_, "終了", hot(bQuit_), false);
}

int Launcher::step() {
    ++frameNo_;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return 3;
        if (ev.type == SDL_TEXTINPUT) {
            if (searchActive_) {
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
            const bool ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
            if (searchActive_) {
                if (sym == SDLK_ESCAPE) {
                    filter_.clear();
                    setSearchActive(false);
                    rebuildView();
                    clampSel();
                    scrollRow_ = 0;
                    revealSelection();
                } else if (sym == SDLK_BACKSPACE) {
                    popUtf8(filter_);
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
            switch (sym) {
            case SDLK_ESCAPE: return 3;
            case SDLK_UP: moveSelectionRow(-1); break;
            case SDLK_DOWN: moveSelectionRow(1); break;
            case SDLK_LEFT: moveSelection(-1); break;
            case SDLK_RIGHT: moveSelection(1); break;
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
            case SDLK_BACKSPACE: {
                fs::path p(dir_);
                if (p.has_parent_path() && p.parent_path() != p)
                    loadDir(p.parent_path().string());
                break;
            }
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
                    return 4;
                }
                setStatus("編集するプロジェクトを選んでください", true);
                break;
            case SDLK_f:
                if (ctrl) setSearchActive(true);
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
            if (in(bSearch_)) {
                setSearchActive(true);
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
            if (in(bRun_)) {
                activate();
                if (!pending_.empty()) return 1;
                continue;
            }
            if (in(bAotRun_)) {
                activate();
                if (!pending_.empty()) {
                    pendingAot_ = true;
                    return 2;
                }
                setStatus("AOT 実行するプロジェクトを選んでください", true);
                continue;
            }
            if (in(bExport_)) {
                exportAot();
                continue;
            }
            if (in(bEdit_)) {
                if (sel_ >= 0 && sel_ < int(entries_.size()) &&
                    !entries_[size_t(sel_)].isDir) {
                    pending_ = entries_[size_t(sel_)].path;
                    pendingEdit_ = true;
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
                return 4;
            }
        } else if (a == "back") {
            fs::path p(dir_);
            if (p.has_parent_path() && p.parent_path() != p)
                loadDir(p.parent_path().string());
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
    SDL_GetWindowSize(win_, &w, &h);
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
    for (;;) {
        Launcher l;
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
        if (dbg) std::fprintf(stderr, "gui: play %s (aot=%d)\n",
                              runPath.c_str(), int(runAot));
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
                    const std::string objPath =
                        (fs::temp_directory_path() / "xyo-gui-aot.xyo")
                            .string();
                    if (aot::writeObjectFile(objPath, br, &why)) {
                        xyoShellPlay(runPath.c_str(), objPath.c_str(), 2,
                                     false, -1, -1);
                        std::error_code ec;
                        fs::remove(objPath, ec);
                        if (oneShot) return 0;
                        continue;
                    }
                }
                std::fprintf(stderr, "gui: AOT 実行に失敗 (%s)\n",
                             why.c_str());
            }
#endif
            // AOT 不可なら通常再生へフォールバック
            xyoShellPlay(runPath.c_str(), nullptr, 2, false, -1, -1);
            if (oneShot) return 0;
            continue;
        }
        xyoShellPlay(runPath.c_str(), nullptr, 2, false, -1, -1);
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
