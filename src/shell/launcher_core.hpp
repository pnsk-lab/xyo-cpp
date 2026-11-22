// xyo-cpp: ランチャー GUI の SDL/Qt 非依存コア。
// ディレクトリ列挙・サムネイル生成（sb3 を読んでステージ合成）・
// 実行設定・最近開いたプロジェクトの永続化を提供する。
// UI バックエンド（Qt Quick ランチャー）はこの層だけを使う。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct Costume;

// ---------------------------------------------------------------------------
// 実行設定（gui-settings.txt に永続化）
// ---------------------------------------------------------------------------

struct GuiRunOptions {
    int scale = 2;
    int fps = -1;       // Project-defined FPS (normally 30)
    int jitMode = 0;    // 0 = HotSpot, 1 = Eager, 2 = interpreter
    int lazyMode = 0;   // 0 = auto, 1 = on, 2 = off
    int gpuMode = -1;   // -1 = normal, 0 = ray, 1 = path
    bool turbo = false;
    bool danger = false;
};

GuiRunOptions readRunOptions();
void writeRunOptions(const GuiRunOptions& o);

// ---------------------------------------------------------------------------
// ディレクトリ項目
// ---------------------------------------------------------------------------

struct LauncherEntry {
    std::string name;   // 表示名
    std::string path;   // フルパス
    bool isDir = false;
    uint64_t size = 0;
    long long mtime = 0;
};

/// dir を絶対パスに正規化して列挙する。返り値は out に
/// 「ディレクトリ（名前順）→ .sb3/.sb2 ファイル（名前順）」の順で入る。
/// 失敗した場合は false を返し、out は無効のまま。
bool listProjectDir(const std::string& dir,
                    std::vector<LauncherEntry>& out,
                    std::string* normalizedDir = nullptr);

// ---------------------------------------------------------------------------
// パスユーティリティ / 永続化
// ---------------------------------------------------------------------------

std::string userHome();
std::string recentHistoryPath();
/// ソースチェックアウト内の examples/ を探す（実行位置から 3 階層遡る）。
std::string sampleDirectory();
/// dir に .sb3/.sb2 が 1 つでもあるか。
bool hasProject(const std::string& dir);

std::string lowerAscii(const std::string& s);
std::string humanSize(uint64_t n);
std::string formatDate(long long t);
/// XYO_GUI_ACTIONS="a,b,c" をパースする。
std::vector<std::string> splitActions(const char* spec);

/// 最近開いたプロジェクト（最大 5 件、新しい順）。
std::vector<std::string> loadRecentProjects();
void rememberRecentProject(std::vector<std::string>& recent,
                           const std::string& path);

// ---------------------------------------------------------------------------
// サムネイル（ワーカースレッドから呼ぶ想定、SDL/Qt 非依存）
// ---------------------------------------------------------------------------

/// コスチューム（PNG / SVG）を RGBA8 へデコードする。サムネイル用。
bool decodeCostumeRGBA(const Costume& c, std::vector<uint8_t>& out, int& w,
                       int& h, double* logicalW = nullptr,
                       double* logicalH = nullptr, int maxDim = 640);

/// アルファ合成しながら拡大縮小コピーする（バイリニア）。
void blendSpriteScaled(std::vector<uint8_t>& dst, int dw, int dh,
                       const std::vector<uint8_t>& src, int sw, int sh,
                       double dx, double dy, double dwid, double dhei);

/// sb3 を読み、背景とスプライトを合成した RGBA を返す（SDL 非依存）。
bool renderThumbRGBA(const std::string& path, int tw, int th,
                     std::vector<uint8_t>& out);

// ---------------------------------------------------------------------------
// サムネイルのディスクキャッシュ
// ---------------------------------------------------------------------------

struct ThumbJobKey {
    std::string path;
    long long mtime = 0;
    uint64_t size = 0;
    int w = 0, h = 0;
};

std::string thumbCacheDir();
std::string thumbCacheFile(const std::string& dir, const ThumbJobKey& j);
bool readThumbCache(const std::string& file, std::vector<uint8_t>& out,
                    int& w, int& h);
void writeThumbCache(const std::string& file, const std::vector<uint8_t>& rgba,
                     int w, int h);

} // namespace xyo
