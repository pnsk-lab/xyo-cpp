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
//
// 共通オプション:
//   --opt-all  現在の作業ツリー diff の opt-in 最適化を一括で有効化
//              （XYO_OPT_ALL=1。明示した個別 XYO_* は上書きしない）
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
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "gui.hpp"
#include "editor.hpp"
#include "wasm_command.hpp"

#include "core/cast.hpp"
#include "core/slist.hpp"
#include "core/fileio.hpp"
#include "core/sstr.hpp"
#include "analysis/project_analysis.hpp"
#include "analysis/type_sites.hpp"
#include "analysis/type_flow.hpp"
#include "analysis/procedure_effects.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/translate_service.hpp"
#include "engine/hot_start.hpp"
#include "engine/opt_flags.hpp"
#include "load/sb3.hpp"
#include "load/sb3_minify.hpp"
#include "load/sb3_rewrite.hpp"
#include "load/sb3_save.hpp"
#include "core/gc.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "jit/jit.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#include "aot/progress_text.hpp"
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
static bool g_blockCount = false;
#ifdef XYO_JIT
static bool g_aotProgress = true;
static bool g_aotRun = false;

// Stable stderr lines so progress survives redirection to build logs.
// Only stage changes, >=5% advances, or 200ms of work produce a line.
static aot::ProgressCallback aotCliProgress(const char* operation) {
    if (!g_aotProgress) return {};
    if (const char* e = std::getenv("XYO_AOT_PROGRESS"))
        if (e[0] == '0') return {};
    return [operation, phase = -1, bucket = -1,
            last = std::chrono::steady_clock::time_point{}]
           (const aot::Progress& p) mutable {
        const int next = static_cast<int>(p.stage);
        const int pct = p.total > 0
            ? int(p.completed * 100 / p.total) : -1;
        const int nextBucket = pct >= 0 ? pct / 5 : -1;
        const auto now = std::chrono::steady_clock::now();
        if (phase == next && p.completed != p.total &&
            nextBucket == bucket &&
            now - last < std::chrono::milliseconds(200))
            return;
        phase = next;
        bucket = nextBucket;
        last = now;
        std::fprintf(stderr, "[aot-%s] %s\n", operation,
                     aot::progressText(p).c_str());
        std::fflush(stderr);
    };
}
#endif
static bool g_highQualityPen = false;
// --render-scale: DLSS 相当の内部解像度（0 以下 = 未指定 → env/既定 1.0）
static double g_renderScale = -1.0;

/// ロード直後の Runtime に CLI の危険モードを反映する。
static void applyRuntimeOptions(Runtime& rt) {
    if (g_danger) rt.dangerMode = true;
    if (g_blockCount) rt.blockCountEnabled = true;
}

static int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  --string-storage rope|flat|append-log  (XYO_STRING_STORAGE; default rope)\n"
        "  --string-leaf utf16|latin1  (XYO_STRING_LEAF; default utf16)\n"
        "  --string-short-intern on|off  (XYO_STRING_SHORT_INTERN=1; default off)\n"
        "  --opt-all  (XYO_OPT_ALL=1; 現在の作業ツリー diff の opt-in 最適化を一括 ON。\n"
        "              明示した個別 XYO_* は上書きしない)\n"
        "  xyo run <project.sb3> [--frames N] [--aot obj]"
        " [--jit hotspot|eager|off] [--jit-lazy auto|on|off] [--danger]\n"
        "          [--block-count]  (XYO_BLOCK_COUNT=1; exact interpreter count)\n"
        "          [--fps N] [--print-list NAME]\n"
        "          [--hot-save FILE] [--hot-load FILE]"
        " [--hot-save-frame N]  (interpreter checkpoint)\n"
        "      --fps: 1 ステップを 1/N 秒に合わせる（scratch-vm の 30fps 相当。"
        "既定 0 = 最速）\n"
        "      --print-list: ステージのリスト NAME の未出力要素を 1 行ずつ"
        " stdout へ出す\n"
        "          （ログ出力つきプロジェクトをヘッドレスで観察する用。"
        "他は stderr へ退避）\n"
        "  xyo play <project.sb3> [--scale N] [--turbo] [--fps N]"
        "  (XYO_PLAY_PROFILE=/path/to/log.csv for frame profiling)\\n"
        "  --hot-save FILE: F5/save at exit; --hot-load FILE: resume state;"
        " --hot-save-frame N: save and exit after N frames\n"
        " [--gpu-realtime ray|path] [--danger] [--block-count]\n"
        " [--high-quality-pen] (XYO_HIGH_QUALITY_PEN=1; display pixel resolution)\n"
        " [--render-scale N] (XYO_RENDER_SCALE; 0.25..1.0, DLSS-equivalent)\n"
        "  xyo wasm <project.sb3> -o <out.wasm> [--script ID]\n"
        "           [--backend binaryen|builtin] [--opt 0|1|2|3]\n"
        "           生成 WASM は env.memory をインポートする (f(i32)->i32)\n"
        "  xyo aot <project.sb3> (-o <out> | --obj <file>)  "
        "事前コンパイル（実験的; 進捗は stderr）\n"
        "  --no-progress  AOT 進捗ログを無効化 (XYO_AOT_PROGRESS=0)\n"
        "  xyo gui [dir]                   ランチャー GUI（引数なしでも起動）\n  xyo edit <project.sb3>             ビルトインエディタ\n"
        "  xyo dump <project.sb3>\n"
        "  xyo analyze <project.sb3> [--dot out.dot] [--top N] [--scripts N] [--type-flow]\n"
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
        " [--jit hotspot|eager|off] [--jit-lazy auto|on|off] [--danger]  （既定 eager）\n"
        "  --jit: hotspot=インタプリタ→ホットなスクリプトだけ JIT（既定）,\n"
        "         eager=全スクリプトを spawn 時にコンパイル（テスト/ベンチ用）,"
        " off=インタプリタのみ\n"
        "  --jit-lazy: auto=HotSpotが手続き利用量で自動選択（既定）、\n"
        "              on=必要時に最適化・コンパイル、off=事前コンパイル\n"
        "  --danger: スレッドごとの実行時間予算（warp 500ms）を外す危険モード。\n"
        "  --list-limit N: リスト長の上限（0 = 無制限、既定 0）。Scratch と同じ\n"
        "         挙動にするなら 200000（XYO_LIST_LIMIT でも指定可）。\n"
        "         1 フレームで終わらないスクリプトが描画/他スレッドへ譲らず\n"
        "         走り続ける（アプリが固まる）— 計測・変換検証専用。\n"
        "         XYO_DANGER=1 でも有効（既定 off）。\n");
    return 1;
}

static std::vector<uint8_t> readFile(const char* path, bool& ok) {
    // Windows では UTF-8 パスを UTF-16 へ変換して開く。
    // Binary SB3s can be many megabytes: a single sized read avoids a
    // per-byte stream iterator and repeated vector growth on startup.
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size >= 0) {
        f.seekg(0, std::ios::beg);
        auto bytes = std::vector<uint8_t>(static_cast<size_t>(size));
        if (!bytes.empty() &&
            !f.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
            ok = false;
            return {};
        }
        ok = true;
        return bytes;
    }
    // Keep non-seekable inputs compatible with the old stream path.
    f.clear();
    f.seekg(0, std::ios::beg);
    ok = true;
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

#ifdef XYO_JIT
/// ロード済み sb3 に AOT オブジェクトを適用する（--aot / AOT 実行ファイル）。
static void applyAotObject(Runtime& rt, const char* objPath,
                           const void* sb3, size_t n,
                           const aot::ProgressCallback& extra = {}) {
    if (!objPath || !rt.jit) return;
    bool ok;
    std::vector<uint8_t> obj = readFile(objPath, ok);
    if (!ok) {
        std::fprintf(stderr, "aot: cannot read %s\n", objPath);
        return;
    }
    std::string why;
    const auto printer = aotCliProgress("run");
    const aot::ProgressCallback both = [&](const aot::Progress& p) {
        if (printer) printer(p);
        if (extra) extra(p);
    };
    if (!rt.jit->loadAot(rt, obj.data(), obj.size(),
                         aot::hashBytes(sb3, n), &why, both))
        std::fprintf(stderr, "aot: load failed (%s)\n", why.c_str());
}
#endif

static void jsonEscapeTo(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

/// 検証用: XYO_DUMPVARS=1 で実行後の全変数/リストを決定的な順で出力。
/// AOT / JIT / インタプリタの意味論一致検査に使う。
/// `XYO_DUMPVARS=json` は同じ内容を JSON で出す（リスト項目に空白や改行が
/// 含まれても切り分けられるようにするため）。
static void dumpVarsJson(Runtime& rt) {
    std::string out = "{";
    bool firstTarget = true;
    for (Target* t : rt.targets) {
        const std::string tn = SStr::toUtf8(t->getName());
        std::vector<std::string> ids;
        ids.reserve(t->variables.size());
        for (auto& [id, v] : t->variables) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        if (ids.empty()) continue;
        if (!firstTarget) out += ',';
        firstTarget = false;
        out += "\n  ";
        jsonEscapeTo(tn, out);
        out += ": {";
        bool first = true;
        for (const std::string& id : ids) {
            Variable& v = t->variables[id];
            const std::string nm = SStr::toUtf8(v.name);
            if (!first) out += ',';
            first = false;
            out += "\n    ";
            jsonEscapeTo(nm, out);
            out += ": ";
            if (v.type == Variable::kListType) {
                SList* l = v.list;
                out += '[';
                for (uint32_t i = 0; l && i < l->len; ++i) {
                    if (i) out += ',';
                    std::string item;
                    SStr* s = cast::toString(l->get0(i));
                    if (s) item = SStr::toUtf8(s->view());
                    jsonEscapeTo(item, out);
                }
                out += ']';
            } else {
                SStr* s = cast::toString(v.value);
                jsonEscapeTo(s ? SStr::toUtf8(s->view()) : std::string(), out);
            }
        }
        out += "\n  }";
    }
    out += firstTarget ? "}\n" : "\n}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

static void dumpVarsIfRequested(Runtime& rt) {
    const char* mode = std::getenv("XYO_DUMPVARS");
    if (!mode) return;
    if (std::strcmp(mode, "json") == 0) {
        dumpVarsJson(rt);
        return;
    }
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
                        SStr* s = cast::toString(l->get0(i));
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

static void printLoadInfo(Runtime& rt, const Sb3LoadResult& r, FILE* out) {
    std::fprintf(out, "loaded: %zu targets", rt.targets.size());
    if (!r.extensionIds.empty()) {
        std::fprintf(out, "  extensions:");
        for (auto& e : r.extensionIds) std::fprintf(out, " %s", e.c_str());
    }
    std::fprintf(out, "\n");
    for (Target* t : rt.targets) {
        std::fprintf(out, "  %s%s: %zu blocks, %zu scripts\n",
                     SStr::toUtf8(t->getName()).c_str(),
                     t->isStage ? " (stage)" : "",
                     t->blocks->size(), t->blocks->getScripts().size());
    }
}

static const char* g_saveAfterRun = nullptr;
// Portable interpreter checkpoints. These are intentionally independent of
// the Scratch project's --save (which exports only project data).
static const char* g_hotSave = nullptr;
static const char* g_hotLoad = nullptr;
static int g_hotSaveFrame = 0;
static uint64_t g_hotFingerprint = 0;


/// ステージ（無ければ先に見つかったターゲット）のリスト変数を名前で探す。
/// `--print-list NAME` が tail する対象。
static SList* findListByName(Runtime& rt, const char* name) {
    SList* found = nullptr;
    for (Target* t : rt.targets) {
        for (auto& [id, v] : t->variables) {
            if (v.type != Variable::kListType || !v.list) continue;
            if (SStr::toUtf8(v.name) != name) continue;
            if (t->isStage) return v.list;   // ステージ優先
            found = found ? found : v.list;
        }
    }
    return found;
}

/// `--print-list NAME`: リストの未出力要素を 1 行ずつ stdout へ出す。
/// 追記専用のリスト（ログ用途）を想定する — 途中で clear されると
/// インデックスがずれるので、縮んだ分は読み飛ばすだけにする。
static void flushList(SList* l, size_t& printed) {
    if (!l) return;
    if (l->len < printed) printed = l->len;
    for (; printed < l->len; ++printed) {
        SStr* s = cast::toString(l->get0(printed));
        const std::string u = s ? SStr::toUtf8(s->view()) : std::string();
        std::fwrite(u.data(), 1, u.size(), stdout);
        std::fputc('\n', stdout);
    }
    std::fflush(stdout);
}

/// `--print-list NAME` のリスト名（nullptr なら通常のヘッドレス実行）。
static const char* g_printList = nullptr;

/// ロード済み Runtime をヘッドレス実行する（cmdRun と AOT 実行で共有）。
/// `fps > 0` で 1 ステップを実時間に合わせる（scratch-vm の 30fps 相当 —
/// 非 warp ループは 1 フレーム 1 反復なので、スケジューラ系の計測に要る）。
/// `g_printList` 指定時は stdout をそのリストの内容だけにする（他は stderr）。
static int runLoaded(Runtime& rt, const Sb3LoadResult* r, int maxFrames,
                     int fps = 0) {
    const bool quietOut = g_printList != nullptr;
    if (r) printLoadInfo(rt, *r, quietOut ? stderr : stdout);
    SList* list = nullptr;
    size_t printed = 0;
    const bool startupTiming = std::getenv("XYO_STARTUP_TIMING") != nullptr;
    const auto startupT0 = std::chrono::steady_clock::now();
    if (g_hotLoad) {
        std::string why;
        if (!loadHotStart(rt, g_hotLoad, g_hotFingerprint, &why)) {
            std::fprintf(stderr, "hot-load: %s\n", why.c_str());
            return 1;
        }
        std::fprintf(stderr, "hot-load: %s\n", g_hotLoad);
    } else {
        rt.greenFlag();
    }
    // A hot-load replaces scalar/list variable nodes and SList objects:
    // resolve the print-list pointer only after the restored state is live.
    list = g_printList ? findListByName(rt, g_printList) : nullptr;
    if (g_printList && !list)
        std::fprintf(stderr, "no list named \"%s\"\n", g_printList);
    if (startupTiming)
        std::fprintf(stderr, "[startup] greenflag-spawn  %.3fms\n",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startupT0).count());
    const int limit = maxFrames > 0 ? maxFrames : 1000000;
    const auto frameDur =
        std::chrono::microseconds(fps > 0 ? 1000000 / fps : 0);
    bool finished = false;
    int executedFrames = 0;
#ifdef XYO_JIT
    const bool showAotFrames = g_aotRun && bool(aotCliProgress("run"));
    const auto aotFrameStart = std::chrono::steady_clock::now();
    auto lastAotFrameReport = aotFrameStart;
#endif
    for (int step = 0; step < limit; ++step) {
        bool anyActive = false;
        for (Thread* t : rt.threads) {
            if (!t->stack.empty() && t->status != STATUS_DONE) {
                anyActive = true;
                break;
            }
        }
        if (!anyActive) { finished = true; break; }
        // Local translation inference is I/O idle time, not a Scratch
        // frame. Only wait when all live scripts are suspended.
        if (fps <= 0 && rt.translationService &&
            rt.translationService->hasPending()) {
            bool allSuspended = true;
            for (const Thread* thread : rt.threads) {
                if (!thread->stack.empty() &&
                    thread->status != STATUS_DONE &&
                    thread->status != STATUS_PROMISE_WAIT) {
                    allSuspended = false;
                    break;
                }
            }
            if (allSuspended) {
                rt.translationService->waitForPending(
                    std::chrono::milliseconds(5));
                rt.step();  // resolve a finished request on the main thread
                --step;     // do not consume the user-specified frame limit
                continue;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        rt.step();
        executedFrames = step + 1;
#ifdef XYO_JIT
        if (showAotFrames) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastAotFrameReport >= std::chrono::seconds(1)) {
                const double seconds = std::chrono::duration<double>(
                    now - aotFrameStart).count();
                if (maxFrames > 0)
                    std::fprintf(stderr,
                        "[aot-run] frame=%d/%d (上限比 %.0f%%) elapsed=%.1fs threads=%zu\n",
                        executedFrames, maxFrames,
                        double(executedFrames) * 100 / maxFrames,
                        seconds, rt.threads.size());
                else
                    std::fprintf(stderr,
                        "[aot-run] frame=%d elapsed=%.1fs threads=%zu\n",
                        executedFrames, seconds, rt.threads.size());
                std::fflush(stderr);
                lastAotFrameReport = now;
            }
        }
#endif
        if (startupTiming && step == 0)
            std::fprintf(stderr, "[startup] first-step       %.3fms\n",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - startupT0).count());
        if (g_printList) flushList(list, printed);
        if (g_hotSave && g_hotSaveFrame > 0 &&
            step + 1 == g_hotSaveFrame) {
            std::string why;
            if (!saveHotStart(rt, g_hotSave, g_hotFingerprint, &why)) {
                std::fprintf(stderr, "hot-save: %s\n", why.c_str());
                return 1;
            }
            std::fprintf(stderr, "hot-save: %s\n", g_hotSave);
            return 0;
        }
        if (fps > 0) {
            const auto dt = std::chrono::steady_clock::now() - t0;
            if (dt < frameDur)
                std::this_thread::sleep_for(frameDur - dt);
        }
    }
    rt.finishBlockCount();
#ifdef XYO_JIT
    if (showAotFrames)
        std::fprintf(stderr, "[aot-run] 終了 frames=%d elapsed=%.3fs (%s)\n",
            executedFrames, std::chrono::duration<double>(
                std::chrono::steady_clock::now() - aotFrameStart).count(),
            finished ? "全スレッド終了" : "フレーム上限");
#endif
    if (g_hotSave && !g_hotSaveFrame) {
        std::string why;
        if (!saveHotStart(rt, g_hotSave, g_hotFingerprint, &why)) {
            std::fprintf(stderr, "hot-save: %s\n", why.c_str());
            return 1;
        }
        std::fprintf(stderr, "hot-save: %s\n", g_hotSave);
    }
    if (g_printList) flushList(list, printed);
    if (g_saveAfterRun) {
        if (saveSb3File(rt, g_saveAfterRun))
            std::printf("saved: %s\n", g_saveAfterRun);
        else
            std::fprintf(stderr, "save failed: %s\n", g_saveAfterRun);
    }
    dumpVarsIfRequested(rt);
    if (std::getenv("XYO_DEBUG_COUNTS")) {
        size_t frames = 0, pooled = 0;
        size_t alive = 0;
        for (const auto& t : rt.ownedThreads) {
            if (!t) continue;   // 回収済みスロット
            ++alive;
            frames += t->stackFrames.size();
            pooled += t->framePool.size();
        }
        std::fprintf(stderr,
                     "[counts] threads=%zu alive=%zu frames=%zu "
                     "pooledFrames=%zu "
                     "targets=%zu objects=%zu gcBytes=%.1fMB\n",
                     rt.ownedThreads.size(), alive, frames, pooled,
                     rt.targets.size(),
                     Gc::get().objectCount(),
                     Gc::get().bytesAllocated() / 1048576.0);
    }
    std::fprintf(quietOut ? stderr : stdout, "%s\n",
                 finished ? "done" : "frame limit reached");
    return finished ? 0 : 2;
}

static int cmdRun(const char* path, int maxFrames, int fps) {
    using StartupClock = std::chrono::steady_clock;
    const bool startupTiming = std::getenv("XYO_STARTUP_TIMING") != nullptr;
    const auto startupT0 = StartupClock::now();
    auto timeMark = [&](const char* name) {
        if (startupTiming)
            std::fprintf(stderr, "[startup] %-16s %.3fms\n", name,
                std::chrono::duration<double, std::milli>(
                    StartupClock::now() - startupT0).count());
    };
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    timeMark("read-sb3");
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    g_hotFingerprint = hotStartFingerprint(data.data(), data.size());

    Runtime rt;
    timeMark("runtime-init");
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    timeMark("sb3-parse");
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    applyRuntimeOptions(rt);
#ifdef XYO_JIT
    applyAotObject(rt, g_aotObj, data.data(), data.size());
#endif
    timeMark("before-greenflag");
    int result = runLoaded(rt, &r, maxFrames, fps);
    timeMark("after-run");
    return result;
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
    bool typeFlow = false;
    analysis::Options opt;
    for (int i = 3; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "--dot") || !std::strcmp(argv[i], "-o")) &&
            i + 1 < argc)
            dotPath = argv[++i];
        else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc)
            opt.topConflicts = size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--scripts") == 0 && i + 1 < argc)
            opt.topScripts = size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--type-flow") == 0)
            typeFlow = true;
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
    text += analysis::formatTypeSites(analysis::analyzeTypeSites(rt));
    if (typeFlow) {
        std::ostringstream out;
        out << "\n== CFG / 型集合の固定点解析 ==\n";
        size_t scripts = 0, totalNodes = 0, totalLatches = 0;
        size_t shown = 0;
        for (Target* target : rt.targets) {
            if (!target || !target->blocks) continue;
            for (const std::string& topId : target->blocks->getScripts()) {
                const Block* top = target->blocks->getBlock(topId);
                if (!top) continue;
                const auto flow = analysis::analyzeTypeFlow(
                    *target->blocks, top->next, 16384, target);
                ++scripts;
                totalNodes += flow.cfg.nodes.size();
                size_t latches = 0, knownWrites = 0, barriers = 0;
                for (size_t i = 0; i < flow.cfg.nodes.size(); ++i) {
                    const auto& node = flow.cfg.nodes[i];
                    latches += node.kind == analysis::FlowNodeKind::LoopLatch;
                    barriers += node.kind == analysis::FlowNodeKind::Barrier;
                    if (node.opcode != "data_setvariableto" &&
                        node.opcode != "data_changevariableby") continue;
                    const Block* block = target->blocks->getBlock(node.blockId);
                    if (!block) continue;
                    auto it = block->fields.find("VARIABLE");
                    if (it == block->fields.end()) continue;
                    constexpr uint32_t any =
                        Variable::kObservedNumber |
                        Variable::kObservedString |
                        Variable::kObservedBoolean |
                        Variable::kObservedColor |
                        Variable::kObservedOther;
                    uint32_t mask = flow.kindAfter(i, it->second.id);
                    if (mask == any) continue;
                    ++knownWrites;
                    if (shown++ < 40) {
                        out << "  " << SStr::toUtf8(target->getName())
                            << '/' << node.blockId << ' '
                            << it->second.id << " -> ";
                        bool firstKind = true;
                        for (auto [kind, name] : {
                            std::pair{uint32_t(Variable::kObservedNumber), "number"},
                            std::pair{uint32_t(Variable::kObservedString), "string"},
                            std::pair{uint32_t(Variable::kObservedBoolean), "boolean"},
                            std::pair{uint32_t(Variable::kObservedColor), "color"},
                            std::pair{uint32_t(Variable::kObservedOther), "other"}}) {
                            if (!(mask & kind)) continue;
                            if (!firstKind) out << '|';
                            out << name;
                            firstKind = false;
                        }
                        out << "\n";
                    }
                }
                totalLatches += latches;
                if (top->opcode == "procedures_definition") {
                    const xyo::BlockInput* prototypeInput =
                        top->inputs.find("custom_block");
                    const Block* prototype = prototypeInput
                        ? target->blocks->getBlock(prototypeInput->block)
                        : nullptr;
                    if (prototype && prototype->mutation) {
                        const std::string code =
                            prototype->mutation->attr("proccode");
                        const auto effects =
                            analysis::summarizeProcedureEffects(
                                *target->blocks, target, code);
                        std::vector<std::string> writes(
                            effects.writtenScalarIds.begin(),
                            effects.writtenScalarIds.end());
                        std::sort(writes.begin(), writes.end());
                        out << "  procedure=" << code
                            << " scalar-writes={";
                        const size_t displayCount = std::min<size_t>(
                            writes.size(), 12);
                        for (size_t wi = 0; wi < displayCount; ++wi) {
                            if (wi) out << ',';
                            if (Variable* v =
                                    target->lookupVariableById(writes[wi])) {
                                out << SStr::toUtf8(v->name) << '['
                                    << writes[wi] << ']';
                            } else {
                                out << writes[wi];
                            }
                        }
                        if (writes.size() > displayCount)
                            out << ",...+" << (writes.size() - displayCount);
                        out << "} unknown-writes="
                            << effects.unknownScalarWrites
                            << " body-yield=" << effects.bodyMayYield
                            << " call-yield=" << effects.callMayYield
                            << " recursion=" << effects.recursive
                            << " truncated=" << effects.truncated << "\n";
                    }
                }
                out << "  script=" << SStr::toUtf8(target->getName())
                    << '/' << topId << " nodes=" << flow.cfg.nodes.size()
                    << " loop-latches=" << latches
                    << " known-writes=" << knownWrites
                    << " barriers=" << barriers
                    << (flow.cfg.truncated ? " (truncated)" : "") << "\n";
            }
        }
        out << "total: scripts=" << scripts
            << " nodes=" << totalNodes << " loop-latches="
            << totalLatches << "\n"
            << "注: 型集合は保守的な静的候補。実行時ガード削除の根拠には"
               "利用していない。\n";
        text += out.str();
    }
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
    applyRuntimeOptions(rt);
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
//   op: md:x,y / mu:x,y / mm:x,y (window px), txt:<utf8>,
//       key:<name> / ku:<name> (key down/up), bc:<broadcast-name>
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
        ev.type = e.op[1] == 'u' ? SDL_KEYUP : SDL_KEYDOWN;
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
    if (g_highQualityPen) ren.setHighQualityPen(true);
    if (!ren.init(rt, scale)) return 1;
    // CLI が env より優先（init 内で XYO_RENDER_SCALE を反映済み）。
    if (g_renderScale > 0.0) ren.setRenderScale(g_renderScale);
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
    if (g_hotLoad) {
        std::string why;
        if (!loadHotStart(rt, g_hotLoad, g_hotFingerprint, &why)) {
            std::fprintf(stderr, "hot-load: %s\n", why.c_str());
            return 2;
        }
        std::fprintf(stderr, "hot-load: %s\n", g_hotLoad);
    } else {
        rt.greenFlag();
    }

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
    // Capture every Nth *presented* frame before SDL_RenderPresent, not the
    // post-swap framebuffer (undefined on hidden/offscreen EGL surfaces).
    // Filenames are PREFIX-000001.ppm, PREFIX-000002.ppm, ...
    const char* shotSeries = std::getenv("XYO_SHOT_SERIES");
    if (shotSeries && !*shotSeries) shotSeries = nullptr;
    const bool shotAsync = shotSeries && [] {
        const char* raw = std::getenv("XYO_SHOT_SERIES_ASYNC");
        return raw && raw[0] == '1';
    }();
    long shotEvery = 1;
    if (shotSeries) {
        if (const char* raw = std::getenv("XYO_SHOT_EVERY")) {
            char* end = nullptr;
            const long value = std::strtol(raw, &end, 10);
            if (end == raw || *end != '\0' || value < 1 ||
                value > 1000000) {
                std::fprintf(stderr,
                    "XYO_SHOT_EVERY must be an integer in [1, 1000000]\n");
                return 2;
            }
            shotEvery = value;
        }
    }
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
    // `--print-list NAME`: ウィンドウ実行でもリストの新規要素を 1 フレーム
    // ごとに stdout へ（パリティテストのような「GUI が要るが結果は行で出る」
    // プロジェクトをヘッドレスに測るため）。XYO_EXIT_ON_DONE=1 で全スレッド
    // 終了時に終了する。
    // The list is resolved after hot-load, which replaces VariableMap nodes.
    SList* printListObj = g_printList ? findListByName(rt, g_printList) : nullptr;
    if (g_printList && !printListObj)
        std::fprintf(stderr, "no list named \"%s\"\n", g_printList);
    size_t printListAt = 0;
    const bool exitOnDone = std::getenv("XYO_EXIT_ON_DONE") != nullptr;

    // Play-time frame profiler. Opt-in only, so normal games pay no timer
    // or file I/O overhead. The CSV is streaming and survives a window
    // close; flush once per second for postmortem inspection.
    FILE* profile = nullptr;
    if (const char* file = std::getenv("XYO_PLAY_PROFILE");
        file && *file) {
        profile = std::fopen(file, "wb");
        if (!profile) {
            std::fprintf(stderr, "cannot open XYO_PLAY_PROFILE=%s\n", file);
        } else {
            std::fprintf(profile,
                "frame,elapsed_s,frame_interval_ms,step_ms,vm_ms,draw_ms,"
                "sprite_ms,backdrop_ms,pen_ms,monitor_ms,ui_ms,present_ms,"
                "costume_decode_ms,effect_build_ms,collision_ms,"
                "sprite_copies,backdrop_copies,costume_decodes,effect_builds,"
                "effect_hits,texture_uploads,collision_calls,"
                "threads,rss_kib,gpu_effect_passes,gpu_effect_ms\n");
            std::fprintf(stderr, "PLAYPROFILE recording: %s\n", file);
        }
    }
    auto profileStarted = std::chrono::steady_clock::now();
    auto lastFrameStarted = profileStarted;
#ifdef XYO_JIT
    const bool showAotFrames = g_aotRun && bool(aotCliProgress("run"));
    auto lastAotFrameReport = profileStarted;
    long aotFrames = 0;
#endif
    const auto currentRssKiB = []() -> unsigned long {
#if defined(__linux__)
        FILE* fp = std::fopen("/proc/self/statm", "r");
        if (!fp) return 0;
        unsigned long resident = 0;
        const int matched = std::fscanf(fp, "%*lu %lu", &resident);
        std::fclose(fp);
        const long pageSize = sysconf(_SC_PAGESIZE);
        return matched == 1 && pageSize > 0
            ? resident * (static_cast<unsigned long>(pageSize) / 1024UL)
            : 0UL;
#else
        return 0UL;
#endif
    };

    // Save the final frame before buffer swap: readback after
    // SDL_RenderPresent is undefined and varies with hidden windows.
    long captureFrameIndex = -1;
    long lastSeriesFrame = -1;
    bool shotAttempted = false;
    bool captureFailed = false;
    if (shot || shotSeries) {
        ren.setFrameCapture([&] {
            if (shotSeries && captureFrameIndex >= 0 &&
                captureFrameIndex != lastSeriesFrame &&
                (captureFrameIndex + 1) % shotEvery == 0) {
                lastSeriesFrame = captureFrameIndex;
                char frameName[32];
                std::snprintf(frameName, sizeof(frameName), "%06ld",
                              captureFrameIndex + 1);
                const std::string filename =
                    std::string(shotSeries) + "-" + frameName + ".ppm";
                const bool captured = shotAsync
                    ? ren.savePPMAsync(filename.c_str())
                    : ren.savePPM(filename.c_str());
                if (!captured) {
                    std::fprintf(stderr,
                        shotAsync
                            ? "XYO_SHOT_SERIES: cannot capture %s\n"
                            : "XYO_SHOT_SERIES: cannot save %s\n",
                        filename.c_str());
                    captureFailed = true;
                }
            }
            if (shot && !shotAttempted &&
                ((exitOnStop && projectStopped) ||
                 (exitOnDone && rt.threads.empty()) ||
                 (exitFrames > 0 &&
                  captureFrameIndex + 1 >= exitFrames))) {
                shotAttempted = true;
                if (!ren.savePPM(shot)) {
                    std::fprintf(stderr, "XYO_SHOT: cannot save %s\n", shot);
                    captureFailed = true;
                }
            }
        });
    }

    // フレーム駆動: 既定 ~30fps（JS: STEP_TIME≈33ms 周期）。
    // --fps N で step 間隔を変更（TurboWarp のカスタム fps 相当 —
    // 非 warp ループは step 毎に進むため、fps を上げると実行も速くなる）。
    using clock = std::chrono::steady_clock;
    const auto kFrame = std::chrono::microseconds(
        fps > 0 ? 1000000 / fps : 33333);
    for (long frame = 0; ; ++frame) {
        captureFrameIndex = frame;
        auto t0 = clock::now();
        while (injectIdx < inject.size() && inject[injectIdx].frame <= frame) {
            const InjectEvent& event = inject[injectIdx++];
            if (std::strcmp(event.op, "bc") == 0) {
                SStr* name = SStr::fromUtf8(event.arg);
                rt.startBroadcast(name->view());
            } else if (!pushInjected(ren, event)) {
                goto done;
            }
        }
        if (!ren.pollEvents()) break;
        if (profile) ren.beginProfileFrame();
        const auto stepStarted = profile ? clock::now() : t0;
        rt.step();
#ifdef XYO_JIT
        ++aotFrames;
        if (showAotFrames &&
            clock::now() - lastAotFrameReport >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(
                clock::now() - profileStarted).count();
            std::fprintf(stderr,
                "[aot-run] 再生中 frame=%ld elapsed=%.1fs threads=%zu\n",
                aotFrames, seconds, rt.threads.size());
            std::fflush(stderr);
            lastAotFrameReport = clock::now();
        }
#endif
        const auto stepFinished = profile ? clock::now() : t0;
        if (profile) {
            const RenderFrameStats stats = ren.endProfileFrame();
            const auto milliseconds = [](clock::duration d) {
                return std::chrono::duration<double, std::milli>(d).count();
            };
            const double stepMs = milliseconds(stepFinished - stepStarted);
            const double intervalMs = frame == 0 ? 0 :
                milliseconds(t0 - lastFrameStarted);
            const double elapsed = std::chrono::duration<double>(
                t0 - profileStarted).count();
            const double vmMs = std::max(0.0, stepMs - stats.drawMs);
            std::fprintf(profile,
                "%ld,%.6f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,"
                "%u,%u,%u,%u,%u,%u,%u,%zu,%lu,%u,%.4f\n",
                frame, elapsed, intervalMs, stepMs, vmMs, stats.drawMs,
                stats.spriteMs, stats.backdropMs, stats.penMs,
                stats.monitorMs, stats.uiMs, stats.presentMs,
                stats.costumeDecodeMs, stats.effectBuildMs,
                stats.collisionMs, stats.sprites, stats.backdrops,
                stats.costumeDecodes, stats.effectBuilds, stats.effectHits,
                stats.textureUploads, stats.collisionCalls,
                rt.threads.size(), currentRssKiB(),
                stats.gpuEffectPasses, stats.gpuEffectMs);
            if ((frame % 30) == 0) std::fflush(profile);
        }
        lastFrameStarted = t0;
        audio.update();   // 再生完了 wait の解決（メインスレッド必須）
        if (g_hotSave && (ren.consumeHotSaveRequest() ||
            (g_hotSaveFrame > 0 && frame + 1 == g_hotSaveFrame))) {
            std::string why;
            if (!saveHotStart(rt, g_hotSave, g_hotFingerprint, &why)) {
                std::fprintf(stderr, "hot-save: %s\n", why.c_str());
                return 2;
            }
            std::fprintf(stderr, "hot-save: %s\n", g_hotSave);
            if (g_hotSaveFrame > 0 && frame + 1 == g_hotSaveFrame)
                break;
        }
        if (g_printList) flushList(printListObj, printListAt);
        if ((exitOnStop && projectStopped) ||
            (exitOnDone && rt.threads.empty()) ||
            (exitFrames > 0 && frame + 1 >= exitFrames)) {
            // A screenshot after SDL_RenderPresent would read undefined
            // back-buffer contents. Report a missing frame instead.
            if (shot && !shotAttempted) {
                std::fprintf(stderr,
                    "XYO_SHOT: no draw before final frame presentation\n");
                captureFailed = true;
            }
            break;
        }
        if (!rt.turboMode && fps > 0) {
            auto dt = clock::now() - t0;
            if (dt < kFrame)
                std::this_thread::sleep_for(kFrame - dt);
        }
    }
done:
#ifdef XYO_JIT
    if (showAotFrames)
        std::fprintf(stderr, "[aot-run] 再生終了 frames=%ld elapsed=%.3fs\n",
            aotFrames, std::chrono::duration<double>(
                clock::now() - profileStarted).count());
#endif
    if (g_hotSave && g_hotSaveFrame == 0) {
        std::string why;
        if (!saveHotStart(rt, g_hotSave, g_hotFingerprint, &why)) {
            std::fprintf(stderr, "hot-save: %s\n", why.c_str());
            return 2;
        }
        std::fprintf(stderr, "hot-save: %s\n", g_hotSave);
    }
    if (profile) {
        std::fflush(profile);
        std::fclose(profile);
        const std::string spritePath =
            std::string(std::getenv("XYO_PLAY_PROFILE")) + ".sprites.csv";
        if (FILE* sprites = std::fopen(spritePath.c_str(), "wb")) {
            std::fprintf(sprites, "sprite,draw_ms,copies\n");
            for (const SpriteProfileRow& row : ren.profileSprites()) {
                std::string escaped;
                for (char c : row.name) {
                    if (c == '"') escaped += "\"\"";
                    else escaped += c;
                }
                std::fprintf(sprites, "\"%s\",%.4f,%llu\n",
                             escaped.c_str(), row.drawMs,
                             static_cast<unsigned long long>(row.copies));
            }
            std::fclose(sprites);
        }
        std::fprintf(stderr, "PLAYPROFILE finished\n");
    }
    if (shotAsync && !ren.flushPPMAsync()) {
        std::fprintf(stderr,
            "XYO_SHOT_SERIES: asynchronous GPU readback or write failed\n");
        captureFailed = true;
    }
    if (exitOnStop) rt.onProjectStopped = std::move(previousStopped);
    if (const char* enabled = std::getenv("XYO_WAIT_READY_STATS");
        enabled && enabled[0] == '1' && rt.sequencer) {
        const auto& stats = rt.sequencer->waitReadyStats();
        std::fprintf(stderr,
            "[wait-ready-play] deferred=%llu sparse_passes=%llu "
            "waiter_visits=%llu full_rechecks=%llu\n",
            static_cast<unsigned long long>(stats.deferredSlots),
            static_cast<unsigned long long>(stats.sparsePasses),
            static_cast<unsigned long long>(stats.scannedWaiters),
            static_cast<unsigned long long>(stats.frameRechecks));
    }
    dumpVarsIfRequested(rt);
    return captureFailed ? 2 : 0;
}

/// GUI からも呼ぶ再生経路（aotObjPath は事前コンパイル済み
/// オブジェクト、nullptr 可。nullptr なら --aot 指定を使う）。
int xyoShellPlay(const char* path, const char* aotObjPath, int scale,
                 bool turbo, int fps, int gpuMode,
                 const std::function<void(int, size_t, size_t)>& onProgress = {},
                 const std::function<void()>& beforePlayback = {}) {
#ifdef XYO_JIT
    g_aotRun = aotObjPath || g_aotObj;
#endif
    bool ok;
    std::vector<uint8_t> data = readFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    applyRuntimeOptions(rt);
    g_hotFingerprint = hotStartFingerprint(data.data(), data.size());
#ifdef XYO_JIT
    const auto onLoad = [&](const aot::Progress& p) {
        if (onProgress)
            onProgress(int(p.stage), p.completed, p.total);
    };
    applyAotObject(rt, aotObjPath ? aotObjPath : g_aotObj, data.data(),
                   data.size(), onProgress ? aot::ProgressCallback(onLoad)
                                           : aot::ProgressCallback{});
#endif
    // GUI AOT Run can close its launcher exactly after the object is loaded,
    // before this playback creates a separate SDL window.
    if (beforePlayback) beforePlayback();
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
    if (!aot::compileProject(rt, hash, br, &why,
                             aotCliProgress("build"))) {
        std::fprintf(stderr, "aot: %s\n", why.c_str());
        return 1;
    }
    if (objOut) {
        aot::reportProgress(aotCliProgress("build"),
                            aot::ProgressStage::WriteOutput);
        if (!aot::writeObjectFile(objOut, br, &why)) {
            std::fprintf(stderr, "aot: %s\n", why.c_str());
            return 1;
        }
        std::printf("aot: %d scripts, %d slots, %zu bytes object -> %s\n",
                    br.scripts, br.slots, br.object.size(), objOut);
        return 0;
    }
    aot::reportProgress(aotCliProgress("build"),
                        aot::ProgressStage::WriteOutput);
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
        else if (!std::strcmp(argv[i], "--no-progress"))
            g_aotProgress = false;
        else
            return usage();
    }
    g_aotRun = true;
    if (auto p = aotCliProgress("run")) {
        std::fprintf(stderr, "[aot-run] プロジェクト初期化\n");
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
        if (!rt.jit->loadAot(rt, object.data(), object.size(), hash, &why,
                             aotCliProgress("run")))
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

/// リスト長の上限（0 = 無制限）。
/// `--list-limit N` または `XYO_LIST_LIMIT=N` で指定する。Scratch 互換の
/// 200000 を再現したいときは 200000 を渡す。
static void applyListLimitConfig(int argc, char** argv) {
    if (const char* e = std::getenv("XYO_LIST_LIMIT")) {
        char* end = nullptr;
        long n = std::strtol(e, &end, 10);
        if (end != e && n >= 0 && n <= 0x7FFFFFFF)
            xyo_list_limit = static_cast<uint32_t>(n);
    }
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;
        if (std::strcmp(a, "--list-limit") == 0 && i + 1 < argc)
            v = argv[++i];
        else if (std::strncmp(a, "--list-limit=", 13) == 0)
            v = a + 13;
        if (!v) continue;
        char* end = nullptr;
        long n = std::strtol(v, &end, 10);
        if (end != v && n >= 0 && n <= 0x7FFFFFFF)
            xyo_list_limit = static_cast<uint32_t>(n);
    }
}

// `--opt-all` / `XYO_OPT_ALL=1`: 現在の作業ツリー diff にある opt-in 最適化を
// 一括で有効化する（名前は engine/opt_flags.cpp の kToggles）。個別の
// XYO_* を明示している場合はそちらを上書きしない。プロジェクトをロードする
// 前に適用するので、AOT 実行ファイルでも同じ経路で効く。
static void applyOptAllConfig(int argc, char** argv) {
    enableReleaseOptimizations(); // v1.2.0 production defaults (not experimental opt-all)
    bool requested = allOptimizationsRequested();
    bool fromCli = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--opt-all") == 0) {
            requested = true;
            fromCli = true;
        }
    }
    if (!requested) return;
    const int applied = enableAllOptimizations();
    if (fromCli || std::getenv("XYO_OPT_ALL_LOG")) {
        std::fprintf(stderr,
                     "[opt-all] %d/%d experimental optimizations enabled "
                     "(unset XYO_OPT_ALL / drop --opt-all for A/B)\n",
                     applied, allOptimizationCount());
    }
}

// Select the Scratch join representation before any project is loaded.
// Environment is the default; CLI overrides it for run/test/play and AOT.
static bool applyStringStorageConfig(int argc, char** argv) {
    if (const char* env = std::getenv("XYO_STRING_STORAGE")) {
        if (!setStringStorageMode(env)) {
            std::fprintf(stderr, "invalid XYO_STRING_STORAGE: %s (rope|flat|append-log)\n", env);
            return false;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const char* mode = nullptr;
        if (std::strcmp(argv[i], "--string-storage") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--string-storage requires a value\n");
                return false;
            }
            mode = argv[++i];
        } else if (std::strncmp(argv[i], "--string-storage=", 17) == 0) {
            mode = argv[i] + 17;
        }
        if (mode && !setStringStorageMode(mode)) {
            std::fprintf(stderr, "invalid --string-storage: %s (rope|flat|append-log)\n", mode);
            return false;
        }
    }
    return true;
}

// Independently select leaf encoding; all joins and the JIT keep the same ABI.
static bool applyStringLeafConfig(int argc, char** argv) {
    if (const char* env = std::getenv("XYO_STRING_LEAF")) {
        if (!setStringLeafMode(env)) {
            std::fprintf(stderr, "invalid XYO_STRING_LEAF: %s (utf16|latin1)\n", env);
            return false;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const char* mode = nullptr;
        if (std::strcmp(argv[i], "--string-leaf") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--string-leaf requires a value\n");
                return false;
            }
            mode = argv[++i];
        } else if (std::strncmp(argv[i], "--string-leaf=", 14) == 0) {
            mode = argv[i] + 14;
        }
        if (mode && !setStringLeafMode(mode)) {
            std::fprintf(stderr, "invalid --string-leaf: %s (utf16|latin1)\n", mode);
            return false;
        }
    }
    return true;
}

// An experimental, capped strong cache for short ASCII join results.
static bool applyStringShortInternConfig(int argc, char** argv) {
    if (const char* env = std::getenv("XYO_STRING_SHORT_INTERN")) {
        if (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0)
            setStringShortInternEnabled(true);
        else if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0)
            setStringShortInternEnabled(false);
        else {
            std::fprintf(stderr,
                         "invalid XYO_STRING_SHORT_INTERN: %s (0|1)\n", env);
            return false;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const char* value = nullptr;
        if (std::strcmp(argv[i], "--string-short-intern") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--string-short-intern requires a value\n");
                return false;
            }
            value = argv[++i];
        } else if (std::strncmp(argv[i], "--string-short-intern=", 22) == 0) {
            value = argv[i] + 22;
        }
        if (!value) continue;
        if (std::strcmp(value, "1") == 0 ||
            std::strcmp(value, "on") == 0) {
            setStringShortInternEnabled(true);
        } else if (std::strcmp(value, "0") == 0 ||
                   std::strcmp(value, "off") == 0) {
            setStringShortInternEnabled(false);
        } else {
            std::fprintf(stderr,
                         "invalid --string-short-intern: %s (on|off)\n", value);
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    WinArgs winArgs;
    winInit(winArgs, argc, argv);
#endif
    // 実行前設定（AOT 実行ファイルでも同じ経路で効かせる）
    applyOptAllConfig(argc, argv);
    applyListLimitConfig(argc, argv);
    if (!applyStringStorageConfig(argc, argv)) return 2;
    if (!applyStringLeafConfig(argc, argv)) return 2;
    if (!applyStringShortInternConfig(argc, argv)) return 2;
#ifdef XYO_JIT
    {
        std::vector<uint8_t> sb3, object;
        uint64_t hash = 0;
        if (aot::readSelf(sb3, object, &hash)) {
            bool countRequested = blockCountEnabledFromEnv();
            for (int i = 1; i < argc; ++i)
                countRequested |= std::strcmp(argv[i], "--block-count") == 0;
            if (countRequested) {
                std::fprintf(stderr, "block-count: embedded AOT cannot be counted exactly; run the .sb3 with --block-count instead\n");
                return 2;
            }
            return runEmbedded(sb3, object, hash, argc, argv);
        }
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
        else if (std::strcmp(argv[i], "--render-scale") == 0 && i + 1 < argc)
            g_renderScale = std::atof(argv[++i]);
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
        else if (std::strcmp(argv[i], "--block-count") == 0)
            g_blockCount = true;
#ifdef XYO_JIT
        else if (std::strcmp(argv[i], "--no-progress") == 0)
            g_aotProgress = false;
#endif
        else if (std::strcmp(argv[i], "--high-quality-pen") == 0 ||
                 std::strcmp(argv[i], "--hq-pen") == 0)
            g_highQualityPen = true;
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
        else if (std::strncmp(argv[i], "--jit-lazy=", 11) == 0) {
            JitLazyMode m;
            if (!parseJitLazyMode(argv[i] + 11, &m)) return usage();
            setJitLazyMode(m);
        }
        else if (std::strcmp(argv[i], "--jit-lazy") == 0 &&
                 i + 1 < argc) {
            JitLazyMode m;
            if (!parseJitLazyMode(argv[++i], &m)) return usage();
            setJitLazyMode(m);
        }
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            g_saveAfterRun = argv[++i];
        }
        else if (std::strcmp(argv[i], "--hot-save") == 0 && i + 1 < argc) {
            g_hotSave = argv[++i];
        }
        else if (std::strcmp(argv[i], "--hot-load") == 0 && i + 1 < argc) {
            g_hotLoad = argv[++i];
        }
        else if (std::strcmp(argv[i], "--hot-save-frame") == 0 && i + 1 < argc) {
            g_hotSaveFrame = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--print-list") == 0 && i + 1 < argc) {
            g_printList = argv[++i];
        }
#ifdef XYO_JIT
        else if (std::strcmp(argv[i], "--aot") == 0 && i + 1 < argc)
            g_aotObj = argv[++i];
#endif
    }
#ifdef XYO_JIT
    g_aotRun = g_aotObj != nullptr;
#endif
    if ((g_hotSave || g_hotLoad) && (std::strcmp(cmd, "play") == 0 ||
                                     std::strcmp(cmd, "run") == 0)) {
        if (jitMode() != JitMode::Off) {
            std::fprintf(stderr,
                "hot-start: switching to interpreter; native fiber stacks "
                "cannot be serialized\n");
            setJitMode(JitMode::Off);
        }
        if (g_hotSaveFrame < 0 || (g_hotSaveFrame && !g_hotSave))
            return usage();
    }
#ifdef XYO_JIT
    if (g_aotObj && (g_blockCount || blockCountEnabledFromEnv())) {
        std::fprintf(stderr, "block-count: --aot cannot be counted exactly; remove --aot to use interpreter counting\n");
        return 2;
    }
#endif
    if (std::strcmp(cmd, "wasm") == 0)
        return cmdWasm(argc, argv);
    if (std::strcmp(cmd, "run") == 0)
        return cmdRun(argv[2], maxFrames, fps > 0 ? fps : 0);
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
