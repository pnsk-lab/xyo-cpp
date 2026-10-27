// xyo — Scratch/TurboWarp 完全互換ランタイム（CLI シェル）。
//
// usage:
//   xyo run <project.sb3> [--frames N]
//   xyo play <project.sb3> [--scale N] [--turbo]   SDL2 ウィンドウで実行
//   xyo dump <project.sb3>          ブロック一覧をダンプ
//   xyo analyze <project.sb3>       実行せず構造を解析（並列性・warp候補・ループ内アンチパターン）
//   xyo minify <in.sb3> [-o out.sb3] sb3 を小さくする（zip 圧縮 + ID 短縮）
//   xyo rewrite <in.sb3> [-o out.sb3] 最適化しやすい形へ書き換える（warp 化・一時変数）
//   xyo test <fixture.sb3|dir>      execute フィクスチャ（SAY pass/fail/end）
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX          // std::min/std::max をマクロで壊させない
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gui.hpp"
#include "editor.hpp"

#include "core/cast.hpp"
#include "core/fileio.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "analysis/project_analysis.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/sb3_minify.hpp"
#include "load/sb3_rewrite.hpp"
#include "load/sb3_save.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "jit/jit.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#endif
#include "wasm/host_bridge.hpp"

using namespace xyo;

#ifdef XYO_JIT
/// `--aot <obj>`: 事前コンパイル済みオブジェクトをロードする。
static const char* g_aotObj = nullptr;
#endif
/// `--danger`: スレッドごとの実行時間予算（warp 500ms）を外す危険モード。
/// 環境変数 `XYO_DANGER` は Runtime 構築時に読まれる（こちらは上書き）。
static bool g_danger = false;

/// ロード直後の Runtime に CLI の危険モードを反映する。
static void applyDangerMode(Runtime& rt) {
    if (g_danger) rt.dangerMode = true;
}

static int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  xyo run <project.sb3> [--frames N] [--aot obj]"
        " [--jit hotspot|eager|off] [--danger]\n"
        "  xyo play <project.sb3> [--scale N] [--turbo] [--fps N]"
        " [--gpu-realtime ray|path] [--danger]\n"
        "  xyo aot <project.sb3> (-o <out> | --obj <file>)  "
        "事前コンパイル（実験的）\n"
        "  xyo gui [dir]                   ランチャー GUI（引数なしでも起動）\n  xyo edit <project.sb3>             ビルトインエディタ\n"
        "  xyo dump <project.sb3>\n"
        "  xyo analyze <project.sb3> [--dot out.dot] [--top N] [--scripts N]\n"
        "      実行せずに構造を解析（並列性・直列化キー・warp化候補・ループ内アンチパターン）\n"
        "      --dot: workerSafe スクリプトの衝突グラフを Graphviz DOT で出力\n"
        "      --top: 衝突キーの表示件数（既定 20）/ --scripts: スクリプト表の件数（0=全部）\n"
        "  xyo minify <in.sb3> [-o out.sb3] [--aggressive]"
        " [--strip-comments]\n"
        "  xyo rewrite <in.sb3> [-o out.sb3] [--no-warp] [--no-inline-temps]\n"
        "              [--warp-min N] [--report]\n"
        "      sb3 を小さくする（zip 圧縮 + ブロック ID 短縮 +"
        " 未参照アセット削除）\n"
        "      --aggressive: 未使用の変数/リスト/ブロードキャストも削除\n"
        "  xyo test <fixture.sb3|dir> [--aot obj]"
        " [--jit hotspot|eager|off] [--danger]  （既定 eager）\n"
        "  --jit: hotspot=インタプリタ→ホットなスクリプトだけ JIT（既定）,\n"
        "         eager=全スクリプトを spawn 時にコンパイル（テスト/ベンチ用）,"
        " off=インタプリタのみ\n"
        "  --danger: スレッドごとの実行時間予算（warp 500ms）を外す危険モード。\n"
        "         1 フレームで終わらないスクリプトが描画/他スレッドへ譲らず\n"
        "         走り続ける（アプリが固まる）— 計測・変換検証専用。\n"
        "         XYO_DANGER=1 でも有効（既定 off）。\n");
    return 1;
}

static std::vector<uint8_t> readFile(const char* path, bool& ok) {
    // Windows では UTF-8 パスを UTF-16 へ変換して開く
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { ok = false; return {}; }
    ok = true;
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

#ifdef XYO_JIT
/// ロード済み sb3 に AOT オブジェクトを適用する（--aot / AOT 実行ファイル）。
static void applyAotObject(Runtime& rt, const char* objPath,
                           const void* sb3, size_t n) {
    if (!objPath || !rt.jit) return;
    bool ok;
    std::vector<uint8_t> obj = readFile(objPath, ok);
    if (!ok) {
        std::fprintf(stderr, "aot: cannot read %s\n", objPath);
        return;
    }
    std::string why;
    if (!rt.jit->loadAot(rt, obj.data(), obj.size(),
                         aot::hashBytes(sb3, n), &why))
        std::fprintf(stderr, "aot: load failed (%s)\n", why.c_str());
}
#endif

/// 検証用: XYO_DUMPVARS=1 で実行後の全変数/リストを決定的な順で出力。
/// AOT / JIT / インタプリタの意味論一致検査に使う。
static void dumpVarsIfRequested(Runtime& rt) {
    if (!std::getenv("XYO_DUMPVARS")) return;
    for (Target* t : rt.targets) {
        const std::string tn = SStr::toUtf8(t->getName());
        std::vector<std::string> ids;
        ids.reserve(t->variables.size());
        for (auto& [id, v] : t->variables) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (const std::string& id : ids) {
            Variable& v = t->variables[id];
            const std::string nm = SStr::toUtf8(v.name);
            if (v.type == Variable::kListType) {
                SList* l = v.list;
                std::printf("var %s/%s[%s] list[%u]", tn.c_str(),
                            nm.c_str(), id.c_str(), l ? l->len : 0u);
                if (l)
                    for (uint32_t i = 0; i < l->len; ++i) {
                        SStr* s = cast::toString(l->items[i]);
                        std::printf(" %s",
                                    s ? SStr::toUtf8(s->view()).c_str() : "");
                    }
                std::printf("\n");
            } else {
                SStr* s = cast::toString(v.value);
                std::printf("var %s/%s[%s]=%s\n", tn.c_str(), nm.c_str(),
                            id.c_str(),
                            s ? SStr::toUtf8(s->view()).c_str() : "");
            }
        }
    }
}

static void printLoadInfo(Runtime& rt, const Sb3LoadResult& r) {
    std::printf("loaded: %zu targets", rt.targets.size());
    if (!r.extensionIds.empty()) {
        std::printf("  extensions:");
        for (auto& e : r.extensionIds) std::printf(" %s", e.c_str());
    }
    std::printf("\n");
    for (Target* t : rt.targets) {
        std::printf("  %s%s: %zu blocks, %zu scripts\n",
                    SStr::toUtf8(t->getName()).c_str(),
                    t->isStage ? " (stage)" : "",
                    t->blocks->size(), t->blocks->getScripts().size());
    }
}

static const char* g_saveAfterRun = nullptr;

/// ロード済み Runtime をヘッドレス実行する（cmdRun と AOT 実行で共有）。
static int runLoaded(Runtime& rt, const Sb3LoadResult* r, int maxFrames) {
    if (r) printLoadInfo(rt, *r);
    rt.greenFlag();
    bool finished = rt.runUntilDone(maxFrames > 0 ? maxFrames : 1000000);
    if (g_saveAfterRun) {
        if (saveSb3File(rt, g_saveAfterRun))
            std::printf("saved: %s\n", g_saveAfterRun);
        else
            std::fprintf(stderr, "save failed: %s\n", g_saveAfterRun);
    }
    dumpVarsIfRequested(rt);
    std::printf("%s\n", finished ? "done" : "frame limit reached");
    return finished ? 0 : 2;
}

static int cmdRun(const char* path, int maxFrames) {
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }

    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    applyDangerMode(rt);
#ifdef XYO_JIT
    applyAotObject(rt, g_aotObj, data.data(), data.size());
#endif
    return runLoaded(rt, &r, maxFrames);
}

static int cmdDump(const char* path) {
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    for (Target* t : rt.targets) {
        std::printf("== %s%s ==\n", SStr::toUtf8(t->getName()).c_str(),
                    t->isStage ? " (stage)" : "");
        for (auto& [id, b] : t->blocks->allBlocks()) {
            std::printf("  %s: %s%s%s\n", id.c_str(), b.opcode.c_str(),
                        b.topLevel ? " [top]" : "",
                        b.shadow ? " [shadow]" : "");
        }
        for (auto& [vid, v] : t->variables) {
            SStr* sv = v.type == Variable::kListType
                           ? nullptr
                           : cast::toString(v.value);
            std::printf("  var %s: %s = %s\n", vid.c_str(),
                        SStr::toUtf8(v.name).c_str(),
                        sv ? SStr::toUtf8(sv->view()).c_str() : "<list>");
        }
    }
    return 0;
}

// --- xyo analyze: 実行しない構造解析 ------------------------------------
static int cmdAnalyze(int argc, char** argv) {
    const char* path = argv[2];
    std::string dotPath;
    analysis::Options opt;
    for (int i = 3; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "--dot") || !std::strcmp(argv[i], "-o")) &&
            i + 1 < argc)
            dotPath = argv[++i];
        else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc)
            opt.topConflicts = size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--scripts") == 0 && i + 1 < argc)
            opt.topScripts = size_t(std::atoi(argv[++i]));
        else
            return usage();
    }
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    analysis::Report rep = analysis::analyzeProject(rt, path, opt);
    std::string text = analysis::formatText(rep, opt);
    std::fwrite(text.data(), 1, text.size(), stdout);
    if (!dotPath.empty()) {
        std::string dot = analysis::formatDot(rep);
        std::ofstream f(pathFromUtf8(dotPath), std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", dotPath.c_str());
            return 1;
        }
        f.write(dot.data(), std::streamsize(dot.size()));
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", dotPath.c_str());
            return 1;
        }
        std::fprintf(stderr, "analyze: dependency graph -> %s\n", dotPath.c_str());
    }
    return 0;
}

// --- xyo minify: sb3 のサイズ削減 ---------------------------------------
static std::string defaultMinifiedPath(const char* in) {
    std::string s = in;
    const size_t slash = s.find_last_of("/\\");
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0 &&
        (slash == std::string::npos || dot > slash) &&
        dot + 1 < s.size()) {
        s.insert(dot, ".min");
    } else {
        s += ".min.sb3";
    }
    return s;
}

/// `xyo rewrite <in.sb3> [-o out.sb3] [--no-warp] [--no-inline-temps]
///  [--warp-min N] [--report]`
/// 意味を変えない範囲で「機械的に最適化しやすい形」へ書き換える。
static int cmdRewrite(int argc, char** argv) {
    const char* in = argv[2];
    std::string out;
    Sb3RewriteOptions opt;
    bool report = false;
    for (int i = 3; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "-o") ||
             !std::strcmp(argv[i], "--output")) &&
            i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--no-warp"))
            opt.warp = false;
        else if (!std::strcmp(argv[i], "--no-inline-temps"))
            opt.inlineTemps = false;
        else if (!std::strcmp(argv[i], "--warp-min") && i + 1 < argc)
            opt.warpMinBlocks = size_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--minify"))
            opt.minify = true;
        else if (!std::strcmp(argv[i], "--aggressive"))
            opt.minifyAggressive = true, opt.minify = true;
        else if (!std::strcmp(argv[i], "--report"))
            report = true;
        else
            return usage();
    }
    if (out.empty()) {
        std::string s(in);
        if (s.size() > 4 && s.substr(s.size() - 4) == ".sb3")
            s.resize(s.size() - 4);
        out = s + ".opt.sb3";
    }
    Sb3RewriteResult r = rewriteSb3File(in, out.c_str(), opt);
    if (!r.ok) {
        std::fprintf(stderr, "rewrite failed: %s\n", r.error.c_str());
        return 1;
    }
    const Sb3RewriteStats& s = r.stats;
    std::printf("rewrite: %s -> %s\n", in, out.c_str());
    std::printf("  projects: %zu procedures scanned, %zu warp-ized\n",
                s.procsScanned, s.procsWarped);
    std::printf("  temps inlined: %zu\n", s.tempsInlined);
    std::printf("  project.json: %zu -> %zu bytes, output: %zu bytes\n",
                s.projectJsonIn, s.projectJsonOut, s.outputBytes);
    if (report)
        for (const std::string& l : s.log) std::fprintf(stderr, "  %s\n", l.c_str());
    return 0;
}

static int cmdMinify(int argc, char** argv) {
    const char* in = argv[2];
    std::string out;
    Sb3MinifyOptions opt;
    for (int i = 3; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "-o") ||
             !std::strcmp(argv[i], "--output")) &&
            i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--aggressive"))
            opt.dropUnusedVariables = true;
        else if (!std::strcmp(argv[i], "--strip-comments"))
            opt.stripComments = true;
        else if (!std::strcmp(argv[i], "--keep-ids"))
            opt.compactIds = false;
        else if (!std::strcmp(argv[i], "--keep-assets"))
            opt.dropUnusedAssets = false;
        else if (!std::strcmp(argv[i], "--keep-orphans"))
            opt.dropOrphanBlocks = false;
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc)
            opt.level = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--store"))
            opt.level = 0;
        else
            return usage();
    }
    if (out.empty()) out = defaultMinifiedPath(in);
    if (opt.level < 0) opt.level = 0;
    if (opt.level > 9) opt.level = 9;

    Sb3MinifyResult r = minifySb3File(in, out.c_str(), opt);
    if (!r.ok) {
        std::fprintf(stderr, "minify failed: %s\n", r.error.c_str());
        return 1;
    }
    const Sb3MinifyStats& s = r.stats;
    const double pct = s.inputBytes
                           ? 100.0 * double(s.outputBytes) / double(s.inputBytes)
                           : 0.0;
    std::printf("minify: %s -> %s\n", in, out.c_str());
    std::printf("  size: %zu -> %zu bytes (%.1f%%)\n", s.inputBytes,
                s.outputBytes, pct);
    std::printf("  project.json: %zu -> %zu bytes\n", s.projectJsonIn,
                s.projectJsonOut);
    std::printf("  blocks: %zu -> %zu, comments: %zu -> %zu,"
                " variables: %zu -> %zu\n",
                s.blocksBefore, s.blocksAfter, s.commentsBefore,
                s.commentsAfter, s.variablesBefore, s.variablesAfter);
    std::printf("  assets: %zu -> %zu (dropped %zu, deduped %zu),"
                " zip: %zu compressed / %zu stored\n",
                s.assetsBefore, s.assetsAfter, s.assetsDropped,
                s.assetsDeduped, s.zipCompressed, s.zipStored);
    return 0;
}

// --- 適合性テスト: integration/execute.js 相当 --------------------------
// SAY "pass ..." / "fail ..." / "plan N" / "end" を収集して採点する。
struct TestResult {
    int pass = 0, fail = 0;
    bool sawEnd = false, timeout = false;
    std::string firstFail;
};

static TestResult runFixture(const std::string& path) {
    TestResult res;
    bool ok;
    std::vector<uint8_t> data = readFile(path.c_str(), ok);
    if (!ok) { res.fail = 1; res.firstFail = "read error"; return res; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        res.fail = 1;
        res.firstFail = "load: " + r.error;
        return res;
    }
    applyDangerMode(rt);
#ifdef XYO_JIT
    applyAotObject(rt, g_aotObj, data.data(), data.size());
#endif
    bool done = false;
    rt.onSay = [&](Target*, const char*, const std::u16string& msg) {
        std::string text = SStr::toUtf8(msg);
        // sayforsecs クリアの空メッセージも流れる
        std::string first = text.substr(0, text.find(' '));
        if (first == "pass") ++res.pass;
        else if (first == "fail") {
            ++res.fail;
            if (res.firstFail.empty()) res.firstFail = text;
        } else if (first == "end") { res.sawEnd = true; done = true; }
        // plan/comment は記録のみ
    };
    rt.greenFlag();
    // TW harness: 非モニタスレッドが尽きるか "end" までステップ。
    // JS harness は status 無関係に threads 数を数え、実時間で VM を回す。
    // edge-activated hat（タイマー閾値超過等）が実時間を要する場合に備え、
    // 稼働スレッドがないときだけ実フレーム相当の間隔で回し、
    // 壁時計リミットを設ける。
    const auto wallStart = std::chrono::steady_clock::now();
    const auto wallLimit = std::chrono::seconds(25);
    bool threadsExhausted = false;
    for (int i = 0; i < 200000 && !done; ++i) {
        rt.step();
        bool anyRunning = false;
        bool anyWait = false;
        bool anyThread = false;
        for (Thread* th : rt.threads) {
            if (th->updateMonitor) continue;
            anyThread = true;
            if (th->status == STATUS_PROMISE_WAIT) anyWait = true;
            else if (th->status != STATUS_DONE) anyRunning = true;
        }
        if (!anyThread) { threadsExhausted = true; break; }
        if (anyRunning) continue;
        // PROMISE_WAIT は waitQueue の実時間期限に依存 — 実時間を進める
        // （JS ハーネスは setInterval で VM を実時間駆動する）
        if (anyWait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - wallStart > wallLimit)
                break;
            continue;
        }
        // 残っているのは retire 済み edge-activated hat のみ —
        // 実時間 30ms 間隔で再評価する（JS のフレームレート相当）
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (std::chrono::steady_clock::now() - wallStart > wallLimit) break;
    }
    // スレッドが全滅して自然終了（stopAll 等で "end" に届かない）場合は
    // タイムアウトとしない — JS ハーネスはスレッド尽きるまで回すのみ
    if (!done && !res.sawEnd && !threadsExhausted) res.timeout = true;
    return res;
}

static int cmdTest(const char* path) {
    // フィクスチャ検証は「全スクリプトをコンパイルした JIT」との差分を
    // 見るためのツールなので、--jit 未指定なら従来動作（eager）にする。
    if (!jitModeExplicit()) setJitMode(JitMode::Eager);
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    if (fs::is_directory(path)) {
        for (auto& e : fs::directory_iterator(path)) {
            std::string p = e.path().string();
            if (p.size() > 4 &&
                (p.ends_with(".sb3") || p.ends_with(".sb2") ||
                 p.ends_with(".sb"))) {
                files.push_back(p);
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(path);
    }
    int tp = 0, tf = 0, tfiles = 0, failedFiles = 0;
    for (auto& f : files) {
        TestResult r = runFixture(f);
        ++tfiles;
        tp += r.pass;
        tf += r.fail;
        if (r.fail > 0 || !r.sawEnd) {
            ++failedFiles;
            std::string base = fs::path(f).filename().string();
            std::printf("not ok %s (pass=%d fail=%d end=%d%s) %s\n",
                        base.c_str(), r.pass, r.fail, int(r.sawEnd),
                        r.timeout ? " timeout" : "",
                        r.firstFail.c_str());
        }
    }
    std::printf("%d files, %d passed, %d failed; assertions: %d pass, %d fail\n",
                tfiles, tfiles - failedFiles, failedFiles, tp, tf);
    return tf > 0 || failedFiles > 0 ? 1 : 0;
}

// --- xyo play: SDL2 ウィンドウ実行 ---------------------------------------
#ifdef XYO_HAVE_SDL2
#include <SDL.h>

#include "audio/sdl_audio.hpp"
#include "gpu/gpu_realtime.hpp"
#include "render/sdl_renderer.hpp"

// 検証用イベントインジェクタ:
//   XYO_INJECT="N:op:args|N:op:args|..."
//   op: md:x,y / mu:x,y / mm:x,y (window px), txt:<utf8>, key:<name>
// フレーム N の pollEvents 前に SDL_PushEvent する。
struct InjectEvent {
    long frame;
    char op[4];
    std::string arg;
};

static uint32_t g_injectButtons = 0;   // 注入ボタン状態（mm 用）

static bool pushInjected(SdlRenderer& ren, const InjectEvent& e) {
    SDL_Event ev{};
    if (e.op[0] == 'm') {
        int x = 0, y = 0;
        std::sscanf(e.arg.c_str(), "%d,%d", &x, &y);
        if (e.op[1] == 'm') {
            ev.type = SDL_MOUSEMOTION;
            ev.motion.x = x; ev.motion.y = y;
            ev.motion.state = g_injectButtons;
        } else {
            if (e.op[1] == 'd') g_injectButtons |= SDL_BUTTON_LMASK;
            else g_injectButtons &= ~SDL_BUTTON_LMASK;
            ev.type = e.op[1] == 'd' ? SDL_MOUSEBUTTONDOWN
                                     : SDL_MOUSEBUTTONUP;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.x = x; ev.button.y = y;
        }
    } else if (e.op[0] == 't' && e.op[1] == 'x') {
        ev.type = SDL_TEXTINPUT;
        std::strncpy(ev.text.text, e.arg.c_str(),
                     sizeof(ev.text.text) - 1);
    } else if (e.op[0] == 'k') {
        ev.type = SDL_KEYDOWN;
        const char* n = e.arg.c_str();
        SDL_Keycode sym = SDLK_UNKNOWN;
        if (!std::strcmp(n, "enter")) sym = SDLK_RETURN;
        else if (!std::strcmp(n, "backspace")) sym = SDLK_BACKSPACE;
        else if (!std::strcmp(n, "escape")) sym = SDLK_ESCAPE;
        else if (!std::strcmp(n, "left")) sym = SDLK_LEFT;
        else if (!std::strcmp(n, "right")) sym = SDLK_RIGHT;
        else if (!std::strcmp(n, "home")) sym = SDLK_HOME;
        else if (!std::strcmp(n, "end")) sym = SDLK_END;
        else if (std::strlen(n) == 1)
            sym = SDL_GetKeyFromName(n);
        ev.key.keysym.sym = sym;
    } else {
        return true;
    }
    return ren.handleEvent(ev);
}

static std::vector<InjectEvent> parseInject(const char* spec) {
    std::vector<InjectEvent> out;
    if (!spec || !*spec) return out;
    std::string s = spec;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find('|', pos);
        std::string tok = s.substr(pos, end == std::string::npos
                                        ? std::string::npos : end - pos);
        pos = end == std::string::npos ? s.size() : end + 1;
        // "N:op:arg"（arg は残り全部 — txt の ':' を許容）
        size_t c1 = tok.find(':');
        size_t c2 = c1 == std::string::npos
                        ? std::string::npos
                        : tok.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        InjectEvent e;
        e.frame = std::atol(tok.c_str());
        std::string op = tok.substr(c1 + 1, c2 - c1 - 1);
        std::snprintf(e.op, sizeof(e.op), "%s", op.c_str());
        e.arg = tok.substr(c2 + 1);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const InjectEvent& a, const InjectEvent& b) {
                  return a.frame < b.frame;
              });
    return out;
}

/// ロード済み Runtime を SDL2 ウィンドウで実行する（cmdPlay と AOT で共有）。
static int playLoaded(Runtime& rt, const std::string& title, int scale,
                      bool turbo, int fps, int gpuMode) {
    SdlRenderer ren;
    if (!ren.init(rt, scale)) return 1;
    ren.setTitle(title.c_str());
    rt.renderer = &ren;
    rt.turboMode = turbo || rt.turboMode;   // _twconfig_ の turbo を潰さない
    if (fps < 0)   // --fps 未指定: プロジェクト設定（既定 30）
        fps = int(std::lround(1000.0 / rt.currentStepTime));
    SdlAudioBank audio;
    audio.init(rt);   // デバイス不在環境では無音で継続
    gpu::RealtimePlayer gpuPlayer;
    if (gpuMode >= 0) {
        std::string why;
        if (!gpuPlayer.init(
                rt, gpuMode == 1 ? gpu::RealtimeMode::Path
                                 : gpu::RealtimeMode::Ray,
                &why)) {
            std::fprintf(stderr,
                         "--gpu-realtime: unsupported (%s) — normal play\n",
                         why.c_str());
        }
    }
    rt.greenFlag();

    // 検証用: XYO_SET="name=value;name2=value2" で緑旗直後に変数をセット
    // （name は変数名か id。値は数値にパースできなければ文字列）。
    if (const char* spec = std::getenv("XYO_SET")) {
        std::string s = spec;
        for (size_t p = 0; p <= s.size();) {
            size_t e = s.find(';', p);
            std::string kv =
                s.substr(p, e == std::string::npos ? e : e - p);
            p = e == std::string::npos ? s.size() + 1 : e + 1;
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string name = kv.substr(0, eq), val = kv.substr(eq + 1);
            Value v;
            char* end = nullptr;
            double d = std::strtod(val.c_str(), &end);
            if (end != val.c_str() && *end == 0)
                v = Value::num(d);
            else
                v = Value::string(SStr::fromUtf8(val));
            for (Target* t : rt.targets)
                for (auto& [id, var] : t->variables)
                    if (id == name || SStr::toUtf8(var.name) == name)
                        var.value = v;
        }
    }

    // 検証用: XYO_EXIT_FRAMES=N で N フレーム後に終了、
    // XYO_SHOT=path.ppm で最終フレームをダンプする。
    int exitFrames = 0;
    if (const char* e = std::getenv("XYO_EXIT_FRAMES"))
        exitFrames = std::atoi(e);
    const char* shot = std::getenv("XYO_SHOT");
    // Opt-in benchmark automation: finish after a project's stop-all, not an
    // arbitrary frame cutoff which could interrupt a large warp batch.
    const bool exitOnStop = std::getenv("XYO_EXIT_ON_STOP") != nullptr;
    bool projectStopped = false;
    auto previousStopped = rt.onProjectStopped;
    if (exitOnStop) rt.onProjectStopped = [&, previousStopped] {
        if (previousStopped) previousStopped();
        projectStopped = true;
    };
    std::vector<InjectEvent> inject = parseInject(std::getenv("XYO_INJECT"));
    size_t injectIdx = 0;

    // フレーム駆動: 既定 ~30fps（JS: STEP_TIME≈33ms 周期）。
    // --fps N で step 間隔を変更（TurboWarp のカスタム fps 相当 —
    // 非 warp ループは step 毎に進むため、fps を上げると実行も速くなる）。
    using clock = std::chrono::steady_clock;
    const auto kFrame = std::chrono::microseconds(
        fps > 0 ? 1000000 / fps : 33333);
    for (long frame = 0; ; ++frame) {
        auto t0 = clock::now();
        while (injectIdx < inject.size() && inject[injectIdx].frame <= frame)
            if (!pushInjected(ren, inject[injectIdx++])) goto done;
        if (!ren.pollEvents()) break;
        rt.step();
        audio.update();   // 再生完了 wait の解決（メインスレッド必須）
        if ((exitOnStop && projectStopped) ||
            (exitFrames > 0 && frame + 1 >= exitFrames)) {
            if (shot) ren.savePPM(shot);
            break;
        }
        if (!rt.turboMode && fps > 0) {
            auto dt = clock::now() - t0;
            if (dt < kFrame)
                std::this_thread::sleep_for(kFrame - dt);
        }
    }
done:
    if (exitOnStop) rt.onProjectStopped = std::move(previousStopped);
    dumpVarsIfRequested(rt);
    return 0;
}

/// GUI からも呼ぶ再生経路（aotObjPath は事前コンパイル済み
/// オブジェクト、nullptr 可。nullptr なら --aot 指定を使う）。
int xyoShellPlay(const char* path, const char* aotObjPath, int scale,
                 bool turbo, int fps, int gpuMode) {
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    applyDangerMode(rt);
#ifdef XYO_JIT
    applyAotObject(rt, aotObjPath ? aotObjPath : g_aotObj, data.data(),
                   data.size());
#endif
    std::string base = path;
    auto sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    return playLoaded(rt, "xyo — " + base, scale, turbo, fps, gpuMode);
}

static int cmdPlay(const char* path, int scale, bool turbo, int fps,
                   int gpuMode) {
    return xyoShellPlay(path, nullptr, scale, turbo, fps, gpuMode);
}
#else
static int playLoaded(Runtime&, const std::string&, int, bool, int, int) {
    std::fprintf(stderr,
                 "xyo was built without SDL2 support (install SDL2 and "
                 "rebuild to use 'xyo play')\n");
    return 1;
}
static int cmdPlay(const char*, int, bool, int, int) {
    std::fprintf(stderr,
                 "xyo was built without SDL2 support (install SDL2 and "
                 "rebuild to use 'xyo play')\n");
    return 1;
}
#endif

#ifdef XYO_JIT
/// `xyo aot`: 全スクリプトを事前コンパイルし、sb3 とオブジェクトを
/// 実行中バイナリへ埋め込んだ単体実行ファイルを作る（実験的機能）。
static int cmdAot(const char* path, const char* out, const char* objOut) {
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    const uint64_t hash = aot::hashBytes(data.data(), data.size());
    aot::BuildResult br;
    std::string why;
    if (!aot::compileProject(rt, hash, br, &why)) {
        std::fprintf(stderr, "aot: %s\n", why.c_str());
        return 1;
    }
    if (objOut) {
        if (!aot::writeObjectFile(objOut, br, &why)) {
            std::fprintf(stderr, "aot: %s\n", why.c_str());
            return 1;
        }
        std::printf("aot: %d scripts, %d slots, %zu bytes object -> %s\n",
                    br.scripts, br.slots, br.object.size(), objOut);
        return 0;
    }
    if (!aot::writeExecutable(out, data.data(), data.size(), br, hash,
                              &why)) {
        std::fprintf(stderr, "aot: %s\n", why.c_str());
        return 1;
    }
    std::printf("aot: %d scripts, %d slots, %zu bytes object -> %s\n",
                br.scripts, br.slots, br.object.size(), out);
    return 0;
}

/// AOT 実行ファイルとして起動された場合の実行。
/// `app [run] [--frames N] [--scale N] [--fps N] [--turbo] [--danger]`
static int runEmbedded(const std::vector<uint8_t>& sb3,
                       const std::vector<uint8_t>& object, uint64_t hash,
                       int argc, char** argv) {
    bool headless = false;
    int maxFrames = 0, scale = 2, fps = -1;
    bool turbo = false, danger = false;
    int i = 1;
    if (i < argc && std::strcmp(argv[i], "run") == 0) {
        headless = true;
        ++i;
    }
    for (; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--fps") && i + 1 < argc)
            fps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--turbo"))
            turbo = true;
        else if (!std::strcmp(argv[i], "--danger"))
            danger = true;
        else
            return usage();
    }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, sb3.data(), sb3.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    if (danger) rt.dangerMode = true;
    if (rt.jit) {
        std::string why;
        if (!rt.jit->loadAot(rt, object.data(), object.size(), hash, &why))
            std::fprintf(stderr,
                         "aot: load failed (%s) — JIT へフォールバック\n",
                         why.c_str());
    }
#ifdef XYO_HAVE_SDL2
    if (!headless)
        return playLoaded(rt, "xyo aot", scale, turbo, fps, -1);
#else
    (void)scale;
    (void)turbo;
    (void)fps;
#endif
    return runLoaded(rt, &r, maxFrames);
}
#endif // XYO_JIT

#ifdef _WIN32
namespace {
/// Windows: コンソールを UTF-8 にし、コマンドライン引数を UTF-16 から
/// UTF-8 へ取り直す（狭い argv は ANSI コードページのため日本語が化ける）。
struct WinArgs {
    std::vector<std::string> store;
    std::vector<char*> ptrs;
};
void winInit(WinArgs& out, int& argc, char**& argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0) return;
    out.store.reserve(size_t(wargc));
    for (int i = 0; i < wargc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                          0, nullptr, nullptr);
        std::string a(n > 0 ? size_t(n - 1) : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n,
                                nullptr, nullptr);
        out.store.push_back(std::move(a));
    }
    LocalFree(wargv);
    out.ptrs.reserve(out.store.size() + 1);
    for (std::string& a : out.store) out.ptrs.push_back(a.data());
    out.ptrs.push_back(nullptr);
    argc = int(out.store.size());
    argv = out.ptrs.data();
}
} // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    WinArgs winArgs;
    winInit(winArgs, argc, argv);
#endif
#ifdef XYO_JIT
    {
        std::vector<uint8_t> sb3, object;
        uint64_t hash = 0;
        if (aot::readSelf(sb3, object, &hash))
            return runEmbedded(sb3, object, hash, argc, argv);
    }
#endif
    // wasm JIT ブリッジの自己テスト（Emscripten ビルドでのみ実処理）
    if (argc >= 2 && std::strcmp(argv[1], "wasm-selftest") == 0)
        return xyo_wasm_selftest();
    // 引数なし = GUI ランチャー（ダブルクリック起動向け）
    if (argc < 2) return guiMain(nullptr);
    if (argc < 3) {
        if (std::strcmp(argv[1], "gui") == 0) return guiMain(nullptr);
        return usage();
    }
    const char* cmd = argv[1];
    int maxFrames = 0, scale = 0, fps = -1, gpuMode = -1;
    bool turbo = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            fps = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--gpu-realtime") == 0 &&
                 i + 1 < argc) {
            const char* m = argv[++i];
            if (std::strcmp(m, "ray") == 0) gpuMode = 0;
            else if (std::strcmp(m, "path") == 0) gpuMode = 1;
            else return usage();
        }
        else if (std::strcmp(argv[i], "--turbo") == 0)
            turbo = true;
        else if (std::strcmp(argv[i], "--danger") == 0)
            g_danger = true;
        else if (std::strncmp(argv[i], "--jit=", 6) == 0) {
            JitMode m;
            if (!parseJitMode(argv[i] + 6, &m)) return usage();
            setJitMode(m);
        }
        else if (std::strcmp(argv[i], "--jit") == 0 && i + 1 < argc) {
            JitMode m;
            if (!parseJitMode(argv[++i], &m)) return usage();
            setJitMode(m);
        }
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            g_saveAfterRun = argv[++i];
        }
#ifdef XYO_JIT
        else if (std::strcmp(argv[i], "--aot") == 0 && i + 1 < argc)
            g_aotObj = argv[++i];
#endif
    }
    if (std::strcmp(cmd, "run") == 0) return cmdRun(argv[2], maxFrames);
    if (std::strcmp(cmd, "play") == 0)
        return cmdPlay(argv[2], scale > 0 ? scale : 2, turbo, fps,
                       gpuMode);
#ifdef XYO_JIT
    if (std::strcmp(cmd, "aot") == 0) {
        const char* out = nullptr;
        const char* objOut = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                out = argv[++i];
            else if (std::strcmp(argv[i], "--obj") == 0 && i + 1 < argc)
                objOut = argv[++i];
        }
        if (!out && !objOut) return usage();
        return cmdAot(argv[2], out, objOut);
    }
#endif
    if (std::strcmp(cmd, "gui") == 0) return guiMain(argv[2]);
    if (std::strcmp(cmd, "dump") == 0) return cmdDump(argv[2]);
    if (std::strcmp(cmd, "analyze") == 0) return cmdAnalyze(argc, argv);
    if (std::strcmp(cmd, "minify") == 0) return cmdMinify(argc, argv);
    if (std::strcmp(cmd, "rewrite") == 0) return cmdRewrite(argc, argv);
    if (std::strcmp(cmd, "edit") == 0) return editorMain(argv[2], g_danger);
    if (std::strcmp(cmd, "test") == 0) return cmdTest(argv[2]);
    return usage();
}
