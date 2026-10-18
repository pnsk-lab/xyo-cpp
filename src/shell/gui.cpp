// xyo-cpp: ランチャー GUI（SDL2）。CLI を使えないユーザー向けに
// 「プロジェクトを選んで実行」と「AOT 単体実行ファイルの書き出し」を
// マウス/キーだけで完結させる。
//
//  - `xyo`（引数なし）または `xyo gui [dir]` で起動
//  - 一覧は *.sb3 / *.sb2 とディレクトリ。クリックで選択、
//    ダブルクリック or Enter で実行、ディレクトリは開く
//  - [AOT 実行] は事前コンパイルしたオブジェクトで再生、
//    [AOT 書き出し] は <名前> という単体実行ファイルを同じ場所に作る
//  - フォントが無い環境でも操作できるよう、文字は矩形フォールバック付き
//
// 自動検証用: XYO_GUI_ACTIONS="down,enter,export" をカンマ区切りで
// 1 フレームに 1 つ実行する（down/up/home/end/enter/back/export/
// aotrun/run/quit）。XYO_GUI_DEBUG=1 で選択・書き出しを stderr に出す。
#include "gui.hpp"

#ifdef XYO_HAVE_SDL2

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"   // utf8ToU16
#include "render/text.hpp"
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
};

struct Tex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

struct Color {
    uint8_t r, g, b, a;
};

constexpr Color kBg{0x1e, 0x22, 0x2c, 255};
constexpr Color kHeader{0x2a, 0x30, 0x3d, 255};
constexpr Color kRowAlt{0x24, 0x29, 0x35, 255};
constexpr Color kSel{0x39, 0x4b, 0x6b, 255};
constexpr Color kFg{0xe8, 0xec, 0xf4, 255};
constexpr Color kDim{0x9a, 0xa5, 0xb8, 255};
constexpr Color kDir{0x8f, 0xc8, 0xff, 255};
constexpr Color kBtn{0x35, 0x40, 0x55, 255};
constexpr Color kBtnHot{0x46, 0x55, 0x72, 255};
constexpr Color kAccent{0x3d, 0x8b, 0x5a, 255};
constexpr Color kWarn{0xb8, 0x53, 0x4a, 255};

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

class Launcher {
public:
    bool open();
    void close();
    /// 1 フレーム描画してイベントを処理。
    /// 0 = 継続 / 1 = 再生 / 2 = AOT 再生 / 3 = 終了
    int step();
    const std::string& pendingPath() const { return pending_; }
    bool pendingAot() const { return pendingAot_; }
    void setStatus(const std::string& s, bool err);
    void openDir(const std::string& d) { loadDir(d); }
    void setActions(std::vector<std::string> a) {
        actions_ = std::move(a);
        actIdx_ = 0;
    }

private:
    void loadDir(const std::string& dir);
    void clampSel();
    void activate();
    void selectRow(int row);
    void exportAot();
    void draw();
    void drawText(int x, int y, const std::string& utf8, int px, Color c,
                  int* outW = nullptr);
    void fill(int x, int y, int w, int h, Color c);
    void outline(int x, int y, int w, int h, Color c);
    bool button(const SDL_Rect& r, const std::string& label, bool hot,
                bool accent);
    SDL_Texture* textTexture(const std::string& utf8, int px, Color c,
                             int* w, int* h);

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    std::unordered_map<std::string, Tex> cache_;
    std::string dir_;
    std::vector<Entry> entries_;
    int sel_ = 0;
    int scroll_ = 0;
    int hotRow_ = -1;
    std::string status_;
    bool statusErr_ = false;
    // AOT 書き出し中は操作を止める（同一フレーム内で完結する）
    size_t actIdx_ = 0;
    std::vector<std::string> actions_;
    bool pendingAot_ = false;
    std::string pending_;
    SDL_Rect bRun_{}, bAotRun_{}, bExport_{}, bQuit_{};
};

void Launcher::fill(int x, int y, int w, int h, Color c) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(ren_, &r);
}

void Launcher::outline(int x, int y, int w, int h, Color c) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderDrawRect(ren_, &r);
}

SDL_Texture* Launcher::textTexture(const std::string& utf8, int px,
                                   Color c, int* w, int* h) {
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "|%d|%02x%02x%02x", px, c.r,
                  c.g, c.b);
    const std::string key = utf8 + suffix;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        *w = it->second.w;
        *h = it->second.h;
        return it->second.tex;
    }
    std::u16string u = utf8ToU16(utf8);
    std::vector<uint8_t> bmp;
    int tw = 0, th = 0;
    FontRenderer* fr = FontRenderer::get();
    if (!fr->render(u, px, c.r, c.g, c.b, bmp, tw, th)) return nullptr;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        bmp.data(), tw, th, 32, tw * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    cache_[key] = Tex{tex, tw, th};
    *w = tw;
    *h = th;
    return tex;
}

void Launcher::drawText(int x, int y, const std::string& utf8, int px,
                        Color c, int* outW) {
    int w = 0, h = 0;
    SDL_Texture* tex = textTexture(utf8, px, c, &w, &h);
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

void Launcher::loadDir(const std::string& dir) {
    // 絶対パスに正規化してから列挙する（相対パスの二重解決を防ぐ）
    std::error_code ec;
    dir_ = fs::absolute(dir, ec).lexically_normal().string();
    if (ec) dir_ = dir;
    entries_.clear();
    sel_ = 0;
    scroll_ = 0;
    std::vector<Entry> dirs, files;
    fs::directory_iterator it(dir_, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(e2)) {
            dirs.push_back({name + "/", it->path().string(), true});
        } else if (it->is_regular_file(e2)) {
            const std::string ext = it->path().extension().string();
            if (ext != ".sb3" && ext != ".sb2") continue;
            files.push_back({name, it->path().string(), false});
        }
    }
    auto byName = [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    const fs::path parent = fs::path(dir_).parent_path();
    if (!parent.empty() && parent != fs::path(dir_))
        entries_.push_back({"../", parent.string(), true});
    for (auto& e : dirs) entries_.push_back(std::move(e));
    for (auto& e : files) entries_.push_back(std::move(e));
    if (std::getenv("XYO_GUI_DEBUG"))
        std::fprintf(stderr, "gui: dir=%s entries=%zu\n", dir_.c_str(),
                     entries_.size());
}

bool Launcher::open() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "gui: SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    win_ = SDL_CreateWindow("xyo — プロジェクトを開く",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            760, 560, SDL_WINDOW_RESIZABLE);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(
        win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
    if (!ren_) return false;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    return true;
}

void Launcher::close() {
    for (auto& [k, t] : cache_)
        if (t.tex) SDL_DestroyTexture(t.tex);
    cache_.clear();
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    ren_ = nullptr;
    win_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Launcher::clampSel() {
    if (entries_.empty()) {
        sel_ = 0;
        scroll_ = 0;
        return;
    }
    sel_ = std::clamp(sel_, 0, int(entries_.size()) - 1);
}

void Launcher::selectRow(int row) {
    if (row < 0) return;
    const int idx = scroll_ + row;
    if (idx < int(entries_.size())) sel_ = idx;
    clampSel();
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
    if (entries_.empty()) return;
    const Entry& e = entries_[size_t(sel_)];
    if (e.isDir) {
        loadDir(e.path);
        return;
    }
    pending_ = e.path;
    pendingAot_ = false;
}

/// AOT 事前コンパイルして単体実行ファイルを書き出す。
void Launcher::exportAot() {
#ifndef XYO_JIT
    setStatus("このビルドは JIT 無しのため AOT できません", true);
#else
    clampSel();
    if (entries_.empty() || entries_[size_t(sel_)].isDir) {
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

bool Launcher::button(const SDL_Rect& r, const std::string& label,
                      bool hot, bool accent) {
    fill(r.x, r.y, r.w, r.h, accent ? kAccent : (hot ? kBtnHot : kBtn));
    outline(r.x, r.y, r.w, r.h, kDim);
    int w = 0;
    int h = 0;
    SDL_Texture* tex =
        textTexture(label, 18, kFg, &w, &h);
    if (tex) {
        SDL_Rect dst{r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
        SDL_RenderCopy(ren_, tex, nullptr, &dst);
    } else {
        fill(r.x + 10, r.y + r.h / 2 - 3, r.w - 20, 6, kFg);
    }
    return false;
}

void Launcher::draw() {
    int W = 760, H = 560;
    SDL_GetWindowSize(win_, &W, &H);
    const int headerH = 44;
    const int footerH = 66;
    const int listTop = headerH + 6;
    const int listH = std::max(40, H - headerH - footerH - 10);
    const int rowH = 28;
    const int visible = std::max(1, listH / rowH);

    // 選択が見えるようにスクロールを合わせる
    clampSel();
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + visible) scroll_ = sel_ - visible + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, int(entries_.size()) - visible));

    SDL_SetRenderDrawColor(ren_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(ren_);

    // ヘッダ
    fill(0, 0, W, headerH, kHeader);
    drawText(12, 10, dir_, 18, kFg);

    // リスト
    SDL_Rect clip{0, listTop, W, listH};
    SDL_RenderSetClipRect(ren_, &clip);
    for (int i = 0; i < visible; ++i) {
        const int idx = scroll_ + i;
        if (idx >= int(entries_.size())) break;
        const Entry& e = entries_[size_t(idx)];
        const int y = listTop + i * rowH;
        if (idx == sel_) fill(0, y, W, rowH, kSel);
        else if (idx & 1) fill(0, y, W, rowH, kRowAlt);
        else if (idx == hotRow_) fill(0, y, W, rowH, kRowAlt);
        drawText(14, y + 5, e.name, 18, e.isDir ? kDir : kFg);
    }
    SDL_RenderSetClipRect(ren_, nullptr);
    if (entries_.empty())
        drawText(14, listTop + 8, "（このフォルダに .sb3 はありません）", 18,
                 kDim);

    // フッタ
    const int by = H - footerH + 8;
    const int bh = 40;
    bRun_ = {12, by, 110, bh};
    bAotRun_ = {130, by, 120, bh};
    bExport_ = {258, by, 140, bh};
    bQuit_ = {W - 100, by, 88, bh};
    button(bRun_, "実行", false, false);
    button(bAotRun_, "AOT 実行", false, false);
    button(bExport_, "AOT 書き出し", false, false);
    button(bQuit_, "終了", false, false);
    if (!status_.empty())
        drawText(12, by - 22, status_, 16, statusErr_ ? kWarn : kDim);
    else
        drawText(12, by - 22,
                 "↑↓ 選択 / Enter 実行 / Backspace 親フォルダ / Esc 終了",
                 16, kDim);
}

int Launcher::step() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return 3;
        if (ev.type == SDL_KEYDOWN) {
            switch (ev.key.keysym.sym) {
            case SDLK_ESCAPE: return 3;
            case SDLK_UP: --sel_; clampSel(); break;
            case SDLK_DOWN: ++sel_; clampSel(); break;
            case SDLK_PAGEUP: sel_ -= 10; clampSel(); break;
            case SDLK_PAGEDOWN: sel_ += 10; clampSel(); break;
            case SDLK_HOME: sel_ = 0; break;
            case SDLK_END: sel_ = int(entries_.size()) - 1; break;
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
            default: break;
            }
        }
        if (ev.type == SDL_MOUSEWHEEL) {
            scroll_ -= ev.wheel.y * 3;
        }
        if (ev.type == SDL_MOUSEMOTION) {
            int W = 0, H = 0;
            SDL_GetWindowSize(win_, &W, &H);
            const int listTop = 50;
            const int listH = std::max(40, H - 44 - 66 - 10);
            const int row = (ev.motion.y - listTop) / 28;
            hotRow_ = (ev.motion.y >= listTop && ev.motion.y < listTop + listH)
                          ? scroll_ + row
                          : -1;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            const int mx = ev.button.x, my = ev.button.y;
            if (mx >= bRun_.x && mx < bRun_.x + bRun_.w && my >= bRun_.y &&
                my < bRun_.y + bRun_.h) {
                activate();
                if (!pending_.empty()) return 1;
                continue;
            }
            if (mx >= bAotRun_.x && mx < bAotRun_.x + bAotRun_.w &&
                my >= bAotRun_.y && my < bAotRun_.y + bAotRun_.h) {
                activate();
                if (!pending_.empty()) {
                    pendingAot_ = true;
                    return 2;
                }
                setStatus("AOT 実行にはプロジェクトを選んでください", true);
                continue;
            }
            if (mx >= bExport_.x && mx < bExport_.x + bExport_.w &&
                my >= bExport_.y && my < bExport_.y + bExport_.h) {
                exportAot();
                continue;
            }
            if (mx >= bQuit_.x && mx < bQuit_.x + bQuit_.w &&
                my >= bQuit_.y && my < bQuit_.y + bQuit_.h)
                return 3;
            // リスト
            int W = 0, H = 0;
            SDL_GetWindowSize(win_, &W, &H);
            const int listTop = 50;
            const int listH = std::max(40, H - 44 - 66 - 10);
            if (my >= listTop && my < listTop + listH) {
                const int row = (my - listTop) / 28;
                if (scroll_ + row < int(entries_.size())) {
                    sel_ = scroll_ + row;
                    if (ev.button.clicks >= 2) {
                        activate();
                        if (!pending_.empty()) return 1;
                    }
                }
            }
        }
    }

    // 自動検証用アクション（1 フレーム 1 つ）
    if (!actions_.empty() && actIdx_ < actions_.size()) {
        const std::string& a = actions_[actIdx_++];
        if (a == "down") { ++sel_; clampSel(); }
        else if (a == "up") { --sel_; clampSel(); }
        else if (a == "home") { sel_ = 0; }
        else if (a == "end") { sel_ = int(entries_.size()) - 1; clampSel(); }
        else if (a == "enter" || a == "run") {
            activate();
            if (!pending_.empty()) return 1;
        } else if (a == "aotrun") {
            activate();
            if (!pending_.empty()) { pendingAot_ = true; return 2; }
        } else if (a == "export") {
            exportAot();
        } else if (a == "back") {
            fs::path p(dir_);
            if (p.has_parent_path() && p.parent_path() != p)
                loadDir(p.parent_path().string());
        } else if (a == "quit") {
            return 3;
        }
    }

    draw();
    SDL_RenderPresent(ren_);
    return 0;
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
        if (oneShot) l.setActions(actions);
        int act = 0;
        for (long frame = 0;; ++frame) {
            act = l.step();
            if (act != 0) break;
            if (!actions.empty() && frame > 200) break;
        }
        runPath = l.pendingPath();
        runAot = l.pendingAot();
        l.close();
        if (act == 3) return 0;
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
