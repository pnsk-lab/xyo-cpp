// xyo-cpp: ランチャー GUI（Qt Quick / Qt6）。
// CLI を使えないユーザー向けに「プロジェクトを選んで実行」と
// 「AOT 単体実行ファイルの書き出し」をマウス/キーだけで完結させる。
//
//  - `xyo`（引数なし）または `xyo gui [dir]` で起動
//  - プロジェクトはステージの実サムネイル付きカードグリッドで表示する
//    （背景 + 表示中スプライトを sb3 から遅延デコード、QThreadPool 上で生成）
//  - クリックで選択、ダブルクリック or Enter で実行、ディレクトリは開く
//  - `/` または Ctrl+F でインクリメンタル絞り込み
//  - [実行] で再生、[編集] で内蔵エディタ、[AOT 実行]/[AOT 書き出し] は
//    事前コンパイル経路（JIT 無しビルドでは書き出し不可）
//
// 自動検証用: XYO_GUI_ACTIONS="down,enter,export" をカンマ区切りで
// 順に実行する（down/up/home/end/enter/back/backnav/forward/
// browse-home/examples/sort/settings/view-list/view-icons/
// scale:N/fps:N/jit:MODE/lazy:MODE/gpu:MODE/turbo:0|1/danger:0|1/
// aotrun/export/edit/search:TEXT/path:PATH/recent0/wheeldown/wheelup/
// click:X:Y/resize:WxH/quit）。XYO_GUI_DEBUG=1 で選択・書き出しログ。
// XYO_GUI_EXIT_FRAMES=N で約 N フレーム分の時間後に終了、
// XYO_GUI_SHOT=path.ppm で終了時のウィンドウを保存する（レイアウト回帰用）。
#include "gui.hpp"

#ifdef XYO_HAVE_QT

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <QAbstractListModel>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QQuickAsyncImageProvider>
#include <QQuickImageProvider>
#include <QQuickTextureFactory>
#include <QQuickView>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqml.h>

// Qt は「slots」をQ_SLOTSのマクロにする。プロジェクト側ヘッダの
// BuildResult::slots と衝突するため解除してから xyo ヘッダを読む。
#ifdef slots
#undef slots
#endif
#include "launcher_core.hpp"
#include "editor.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#include "aot/progress_text.hpp"
#include "jit/jit.hpp"
#endif

/// main.cpp の再生経路（SDL ウィンドウで実行）。
/// aotObjPath は事前コンパイル済みオブジェクト（nullptr 可）。
int xyoShellPlay(const char* path, const char* aotObjPath, int scale,
                 bool turbo, int fps, int gpuMode,
                 const std::function<void(int, size_t, size_t)>& onProgress = {},
                 const std::function<void()>& beforePlayback = {});

namespace xyo {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// エントリモデル（ソース: LauncherEntry の配列）
// ---------------------------------------------------------------------------

class LauncherEntryModel : public QAbstractListModel {
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        DisplayNameRole,
        PathRole,
        IsDirRole,
        SizeTextRole,
        DateTextRole,
        MtimeRole,
        ExtRole,
        ThumbRole,
    };

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(entries_.size());
    }
    QHash<int, QByteArray> roleNames() const override {
        return {{NameRole, "name"},
                {DisplayNameRole, "displayName"},
                {PathRole, "path"},
                {IsDirRole, "isDir"},
                {SizeTextRole, "sizeText"},
                {DateTextRole, "dateText"},
                {MtimeRole, "mtime"},
                {ExtRole, "ext"},
                {ThumbRole, "thumbUrl"}};
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= int(entries_.size()))
            return {};
        const LauncherEntry& e = entries_[size_t(index.row())];
        switch (role) {
        case NameRole:
            return QString::fromStdString(e.name);
        case DisplayNameRole: {
            const fs::path p(e.name);
            const std::string s = e.isDir ? e.name : p.stem().string();
            return QString::fromStdString(s);
        }
        case PathRole:
            return QString::fromStdString(e.path);
        case IsDirRole:
            return e.isDir;
        case SizeTextRole:
            return e.isDir ? QString("—")
                           : QString::fromStdString(humanSize(e.size));
        case DateTextRole:
            return e.isDir ? QString("—")
                           : QString::fromStdString(formatDate(e.mtime));
        case MtimeRole:
            return QVariant::fromValue<qlonglong>(e.mtime);
        case ExtRole:
            return e.isDir
                       ? QString()
                       : QString::fromStdString(lowerAscii(
                             fs::path(e.name).extension().string()));
        case ThumbRole:
            if (e.isDir) return QString();
            return "image://xyothumb/" + QString::number(e.mtime) + "/" +
                   QString::fromUtf8(QUrl::toPercentEncoding(
                       QString::fromStdString(e.path)));
        }
        return {};
    }

    void setEntries(std::vector<LauncherEntry> entries) {
        beginResetModel();
        entries_ = std::move(entries);
        endResetModel();
    }
    const LauncherEntry* entryAt(int row) const {
        return row >= 0 && row < int(entries_.size())
                   ? &entries_[size_t(row)] : nullptr;
    }
    int count() const { return int(entries_.size()); }

private:
    std::vector<LauncherEntry> entries_;
};

// フィルタ（名前の部分一致・ASCII 小文字）とソート（名前順 / 更新日順）。
// 「更新日順」でもディレクトリは先頭のまま。
class LauncherSortFilter : public QSortFilterProxyModel {
public:
    void setNeedle(const QString& n) {
        if (needle_ == n) return;
        needle_ = n.toLower();
        invalidate();
    }
    void setSortRecent(bool on) {
        if (sortRecent_ == on) return;
        sortRecent_ = on;
        invalidate();
        sort(0);
    }
    bool sortRecent() const { return sortRecent_; }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (needle_.isEmpty()) return true;
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        const QString name =
            idx.data(LauncherEntryModel::NameRole).toString().toLower();
        return name.contains(needle_);
    }
    bool lessThan(const QModelIndex& l, const QModelIndex& r) const override {
        const bool ldir = l.data(LauncherEntryModel::IsDirRole).toBool();
        const bool rdir = r.data(LauncherEntryModel::IsDirRole).toBool();
        if (ldir != rdir) return ldir;
        if (!ldir && sortRecent_) {
            const auto lm = l.data(LauncherEntryModel::MtimeRole).toLongLong();
            const auto rm = r.data(LauncherEntryModel::MtimeRole).toLongLong();
            if (lm != rm) return lm > rm;
        }
        return l.data(LauncherEntryModel::NameRole)
                   .toString()
                   .toLower() < r.data(LauncherEntryModel::NameRole)
                                    .toString()
                                    .toLower();
    }

private:
    QString needle_;
    bool sortRecent_ = false;
};

// ---------------------------------------------------------------------------
// サムネイル: QQuickAsyncImageProvider（QThreadPool 上で生成、SDL 非依存の
// launcher_core::renderThumbRGBA + ディスクキャッシュを使う）。
// URL 形式: image://xyothumb/<mtime>/<percent-encoded-path>
// ---------------------------------------------------------------------------

// Qt6.12: QQuickImageResponse は QRunnable ではないため、
// 自分で QThreadPool へ流す（run() をワーカー側で走らせる）。
class ThumbResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbResponse(QString id, QSize requested)
        : id_(std::move(id)), requested_(requested) {}

    QQuickTextureFactory* textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }
    QString errorString() const override { return error_; }

    void run() override {
        const int slash = id_.indexOf('/');
        const QString encPath = slash >= 0 ? id_.mid(slash + 1) : QString();
        const std::string path =
            QUrl::fromPercentEncoding(encPath.toUtf8()).toStdString();
        int tw = requested_.width() > 0 ? requested_.width() : 192;
        int th = requested_.height() > 0 ? requested_.height() : 144;
        tw = std::clamp(tw, 8, 1024);
        th = std::clamp(th, 8, 1024);

        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        ThumbJobKey key;
        key.path = path;
        {
            std::error_code ec;
            key.size = fs::file_size(path, ec);
            const auto ft = fs::last_write_time(path, ec);
            if (!ec) {
                const auto sys = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                key.mtime = std::chrono::system_clock::to_time_t(sys);
            }
        }
        key.w = tw;
        key.h = th;
        const std::string cacheDir = thumbCacheDir();
        const std::string cache = thumbCacheFile(cacheDir, key);
        bool ok = readThumbCache(cache, rgba, w, h);
        if (!ok) {
            w = tw;
            h = th;
            ok = renderThumbRGBA(path, tw, th, rgba);
            if (ok) writeThumbCache(cache, rgba, w, h);
        }
        if (ok && w > 0 && h > 0) {
            image_ = QImage(w, h, QImage::Format_RGBA8888);
            std::memcpy(image_.bits(), rgba.data(),
                        size_t(w) * size_t(h) * 4);
        } else {
            error_ = "no thumbnail";
        }
        Q_EMIT finished();
    }

private:
    QString id_;
    QSize requested_;
    QImage image_;
    QString error_;
};

class ThumbImageProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(
        const QString& id, const QSize& requestedSize) override {
        auto* res = new ThumbResponse(id, requestedSize);
        res->setAutoDelete(false);  // 所有権はエンジン側
        pool_.start(res);
        return res;
    }

private:
    QThreadPool pool_;
};

// ---------------------------------------------------------------------------
// 実行フラグ（選択プロジェクトだけに効く。ランチャーのサムネイル
// ワーカー・後続の実行には影響しない）
// ---------------------------------------------------------------------------

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

// AOT playback needs a short-lived object file. The standard temporary
// directory can be read-only in sandboxes; use the user's state directory
// first and never share a fixed filename with another running instance.
struct TemporaryAotObject {
    std::string path;

    TemporaryAotObject() {
        std::vector<fs::path> dirs;
        const std::string settings = recentHistoryPath();
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

// ---------------------------------------------------------------------------
// コントローラ（QML から `launcher` 名で触る）
// ---------------------------------------------------------------------------

struct NavState {
    std::string dir;
    std::string selection;
};

class LauncherController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString dirPath READ dirPath NOTIFY dirChanged)
    Q_PROPERTY(QString dirName READ dirName NOTIFY dirChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY dirChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilterString NOTIFY filterChanged)
    Q_PROPERTY(bool sortRecent READ sortRecent WRITE setSortRecent NOTIFY viewChanged)
    Q_PROPERTY(bool listView READ listView WRITE setListView NOTIFY viewChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool projectSelected READ projectSelected NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedIsDir READ selectedIsDir NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedKind READ selectedKind NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedMeta READ selectedMeta NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedThumb READ selectedThumb NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList recentList READ recentList NOTIFY recentChanged)
    Q_PROPERTY(QString homeDir READ homeDir CONSTANT)
    Q_PROPERTY(QString desktopDir READ desktopDir CONSTANT)
    Q_PROPERTY(QString documentsDir READ documentsDir CONSTANT)
    Q_PROPERTY(QString downloadsDir READ downloadsDir CONSTANT)
    Q_PROPERTY(QString examplesDir READ examplesDir CONSTANT)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY dirChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY dirChanged)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY dirChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double aotProgress READ aotProgress NOTIFY aotProgressChanged)
    // 実行オプション（永続化）
    Q_PROPERTY(int optScale READ optScale WRITE setOptScale NOTIFY optionsChanged)
    Q_PROPERTY(int optFps READ optFps WRITE setOptFps NOTIFY optionsChanged)
    Q_PROPERTY(int optJit READ optJit WRITE setOptJit NOTIFY optionsChanged)
    Q_PROPERTY(int optLazy READ optLazy WRITE setOptLazy NOTIFY optionsChanged)
    Q_PROPERTY(int optGpu READ optGpu WRITE setOptGpu NOTIFY optionsChanged)
    Q_PROPERTY(bool optTurbo READ optTurbo WRITE setOptTurbo NOTIFY optionsChanged)
    Q_PROPERTY(bool optDanger READ optDanger WRITE setOptDanger NOTIFY optionsChanged)
    Q_PROPERTY(bool jitAvailable READ jitAvailable CONSTANT)
    Q_PROPERTY(bool playAvailable READ playAvailable CONSTANT)
    Q_PROPERTY(QString currentDirLabel READ currentDirLabel NOTIFY dirChanged)

public:
    enum Result {
        ResultNone = 0,
        ResultQuit = 3,
        ResultContinue = 4,  // 再生/編集済み。外側ループを継続
        ResultRun = 5,
        ResultEdit = 6,
    };

    explicit LauncherController(QObject* parent = nullptr)
        : QObject(parent) {
        proxy_.setSourceModel(&model_);
        proxy_.setDynamicSortFilter(true);
        proxy_.sort(0);
        options_ = readRunOptions();
        recentStd_ = loadRecentProjects();
        homeDir_ = userHome();
        examplesDir_ = sampleDirectory();
    }

    QAbstractItemModel* proxyModel() { return &proxy_; }
    void attach(QQuickView* v) { view_ = v; }
    bool oneShot() const { return oneShot_; }
    void setOneShot(bool on) { oneShot_ = on; }
    void setActions(std::vector<std::string> a) { actions_ = std::move(a); }
    const std::vector<std::string>& actions() const { return actions_; }
    void setShotPath(const std::string& p) { shotPath_ = p; }
    int result() const { return result_; }
    std::string pendingPath() const { return pendingPath_; }
    const GuiRunOptions& runOptions() const { return options_; }

    // ---- properties
    QString dirPath() const { return QString::fromStdString(dir_); }
    QString dirName() const {
        if (dir_.empty()) return QStringLiteral("プロジェクト");
        const QString n = QString::fromStdString(
            fs::path(dir_).filename().string());
        return n.isEmpty() ? QStringLiteral("プロジェクト") : n;
    }
    QVariantList crumbs() const {
        QVariantList out;
        fs::path running;
        for (const fs::path& part : fs::path(dir_)) {
            running /= part;
            const std::string raw = part.string();
            if (raw.empty()) continue;
            QVariantMap m;
            m["label"] = QString::fromStdString(raw == "/" ? "ルート" : raw);
            m["path"] = QString::fromStdString(running.string());
            out.push_back(m);
        }
        return out;
    }
    QString filter() const { return filter_; }
    void setFilterString(const QString& f) {
        if (filter_ == f) return;
        filter_ = f;
        proxy_.setNeedle(f);
        fixSelectionAfterFilter();
        Q_EMIT filterChanged();
        Q_EMIT revealSelection();
    }
    bool sortRecent() const { return proxy_.sortRecent(); }
    void setSortRecent(bool on) {
        if (proxy_.sortRecent() == on) return;
        proxy_.setSortRecent(on);
        fixSelectionAfterFilter();
        Q_EMIT viewChanged();
        Q_EMIT revealSelection();
    }
    bool listView() const { return listView_; }
    void setListView(bool on) {
        if (listView_ == on) return;
        listView_ = on;
        Q_EMIT viewChanged();
        Q_EMIT revealSelection();
    }
    QString statusText() const { return status_; }
    bool statusIsError() const { return statusErr_; }
    bool hasSelection() const { return !selPath_.isEmpty(); }
    bool projectSelected() const { return hasSelection() && !selIsDir_; }
    QString selectedPath() const { return selPath_; }
    bool selectedIsDir() const { return selIsDir_; }
    QString selectedName() const {
        if (selPath_.isEmpty()) return {};
        const fs::path p = fs::path(selPath_.toStdString()).filename();
        const std::string s =
            selIsDir_ ? p.string() : p.stem().string();
        return QString::fromStdString(s);
    }
    QString selectedKind() const {
        return selIsDir_ ? QStringLiteral("フォルダ")
                         : QStringLiteral("Scratch プロジェクト");
    }
    QString selectedMeta() const {
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e) return {};
        if (e->isDir) return QStringLiteral("フォルダ");
        QString s = QString::fromStdString(
            lowerAscii(fs::path(e->name).extension().string()));
        s += "  ·  " + QString::fromStdString(humanSize(e->size));
        const std::string d = formatDate(e->mtime);
        if (!d.empty())
            s += "  ·  " + QString::fromStdString(d);
        return s;
    }
    QString selectedThumb() const {
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e || e->isDir) return {};
        return "image://xyothumb/" + QString::number(e->mtime) + "/" +
               QString::fromUtf8(QUrl::toPercentEncoding(
                   QString::fromStdString(e->path)));
    }
    QVariantList recentList() const {
        QVariantList out;
        for (const std::string& p : recentStd_) {
            const fs::path f(p);
            QVariantMap m;
            m["stem"] = QString::fromStdString(f.stem().string());
            m["parent"] =
                QString::fromStdString(f.parent_path().filename().string());
            m["path"] = QString::fromStdString(p);
            out.push_back(m);
        }
        return out;
    }
    QString homeDir() const { return QString::fromStdString(homeDir_); }
    QString desktopDir() const { return existingSub(QStringLiteral("Desktop")); }
    QString documentsDir() const { return existingSub(QStringLiteral("Documents")); }
    QString downloadsDir() const { return existingSub(QStringLiteral("Downloads")); }
    QString examplesDir() const { return QString::fromStdString(examplesDir_); }
    bool canGoBack() const { return !backDirs_.empty(); }
    bool canGoForward() const { return !forwardDirs_.empty(); }
    bool canGoUp() const {
        if (dir_.empty()) return false;
        const fs::path parent = fs::path(dir_).parent_path();
        return !parent.empty() && parent != fs::path(dir_);
    }
    bool busy() const { return busy_; }
    double aotProgress() const { return aotProgress_; }
    int optScale() const { return options_.scale; }
    void setOptScale(int v) {
        if (v < 1 || v > 4) return;
        options_.scale = v;
        persistOptions();
    }
    int optFps() const { return options_.fps; }
    void setOptFps(int v) {
        if (v != -1 && v != 30 && v != 60 && v != 120) return;
        options_.fps = v;
        persistOptions();
    }
    int optJit() const { return options_.jitMode; }
    void setOptJit(int v) {
        if (v < 0 || v > 2) return;
        options_.jitMode = v;
        persistOptions();
    }
    int optLazy() const { return options_.lazyMode; }
    void setOptLazy(int v) {
        if (v < 0 || v > 2) return;
        options_.lazyMode = v;
        persistOptions();
    }
    int optGpu() const { return options_.gpuMode; }
    void setOptGpu(int v) {
        if (v < -1 || v > 1) return;
        options_.gpuMode = v;
        persistOptions();
    }
    bool optTurbo() const { return options_.turbo; }
    void setOptTurbo(bool v) {
        options_.turbo = v;
        persistOptions();
    }
    bool optDanger() const { return options_.danger; }
    void setOptDanger(bool v) {
        options_.danger = v;
        Q_EMIT optionsChanged();  // danger は永続化しない
    }
    bool jitAvailable() const {
#ifdef XYO_JIT
        return true;
#else
        return false;
#endif
    }
    bool playAvailable() const {
#ifdef XYO_HAVE_SDL2
        return true;
#else
        return false;
#endif
    }
    QString currentDirLabel() const { return dirName(); }

    // ---- invokables
    Q_INVOKABLE void openDir(const QString& d) { navigate(d.toStdString()); }
    Q_INVOKABLE void goUp() {
        if (dir_.empty()) return;
        const fs::path parent = fs::path(dir_).parent_path();
        if (!parent.empty() && parent != fs::path(dir_))
            navigate(parent.string());
    }
    Q_INVOKABLE void goBack() {
        if (backDirs_.empty()) return;
        NavState nav = std::move(backDirs_.back());
        backDirs_.pop_back();
        if (!dir_.empty()) forwardDirs_.push_back(currentNav());
        restoreNav(nav);
    }
    Q_INVOKABLE void goForward() {
        if (forwardDirs_.empty()) return;
        NavState nav = std::move(forwardDirs_.back());
        forwardDirs_.pop_back();
        if (!dir_.empty()) backDirs_.push_back(currentNav());
        restoreNav(nav);
    }
    Q_INVOKABLE void reload() { loadDir(dir_); }
    Q_INVOKABLE void selectAt(int proxyRow) {
        const LauncherEntry* e = entryAtProxy(proxyRow);
        if (!e) return;
        setSelection(QString::fromStdString(e->path), e->isDir);
    }
    Q_INVOKABLE void activateAt(int proxyRow) {
        selectAt(proxyRow);
        activate();
    }
    Q_INVOKABLE void activate() {
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e) return;
        if (e->isDir) {
            navigate(e->path);
            return;
        }
        runProject();
    }
    Q_INVOKABLE void runProject() {
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e || e->isDir || busy_) return;
#ifndef XYO_HAVE_SDL2
        setStatus(QStringLiteral(
                      "このビルドは SDL2 無しのため再生できません"),
                  true);
        return;
#endif
        pendingPath_ = e->path;
        rememberRecent(QString::fromStdString(e->path));
        result_ = ResultRun;
        QCoreApplication::quit();
    }
    Q_INVOKABLE void openEditor() {
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e || e->isDir) {
            setStatus(QStringLiteral(
                          "編集するプロジェクトを選んでください"),
                      true);
            return;
        }
#ifndef XYO_HAVE_SDL2
        setStatus(QStringLiteral(
                      "このビルドは SDL2 無しのため編集できません"),
                  true);
        return;
#endif
        pendingPath_ = e->path;
        rememberRecent(QString::fromStdString(e->path));
        result_ = ResultEdit;
        QCoreApplication::quit();
    }
    Q_INVOKABLE void quitApp() {
        result_ = ResultQuit;
        QCoreApplication::quit();
    }
    Q_INVOKABLE void openRecent(const QString& path) {
        navigate(path.toStdString());
    }
    Q_INVOKABLE void moveSelection(int delta) {
        const int rows = proxy_.rowCount();
        if (rows <= 0) return;
        int pos = proxyRowForPath(selPath_);
        pos = std::clamp(pos + delta, 0, rows - 1);
        selectAt(pos);
        Q_EMIT revealSelection();
    }
    Q_INVOKABLE void moveSelectionRows(int dir, int cols) {
        moveSelection(dir * std::max(1, cols));
    }
    Q_INVOKABLE void selectHome() {
        if (proxy_.rowCount() > 0) {
            selectAt(0);
            Q_EMIT revealSelection();
        }
    }
    Q_INVOKABLE void selectEnd() {
        if (proxy_.rowCount() > 0) {
            selectAt(proxy_.rowCount() - 1);
            Q_EMIT revealSelection();
        }
    }
    Q_INVOKABLE void scrollDelta(qreal delta) { Q_EMIT scrollBy(delta); }
    /// パス → プロキシ（表示）行。無ければ -1。QML の positionViewAtIndex 用。
    Q_INVOKABLE int proxyRowOf(const QString& path) const {
        return idxPathVisible(path) ? proxyRowForPath(path) : -1;
    }
    Q_INVOKABLE void simulateClick(int x, int y) {
        if (!view_) return;
        const QPointF local(x, y);
        const QPoint global = view_->mapToGlobal(QPoint(x, y));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QGuiApplication::sendEvent(view_, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QGuiApplication::sendEvent(view_, &release);
    }
    Q_INVOKABLE void resizeView(int w, int h) {
        if (view_) view_->resize(w, h);
    }
    Q_INVOKABLE void exportAot() {
#ifdef XYO_JIT
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e || e->isDir) {
            setStatus(QStringLiteral(
                          "プロジェクトファイルを選んでください"),
                      true);
            return;
        }
        if (busy_) return;
        setBusy(true);
        beginAotProgress(QStringLiteral("AOT 書き出し: ") +
                         QString::fromStdString(e->name));

        std::vector<uint8_t> data = readFileBytes(e->path);
        if (data.empty()) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("読み込みに失敗しました: ") +
                          QString::fromStdString(e->name),
                      true);
            return;
        }
        const uint64_t hash = aot::hashBytes(data.data(), data.size());
        Runtime rt;
        if (!loadSb3File(rt, data.data(), data.size()).ok) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral(
                          "プロジェクトのロードに失敗しました"),
                      true);
            return;
        }
        aot::BuildResult br;
        std::string why;
        if (!aot::compileProject(rt, hash, br, &why,
                                 [this](const aot::Progress& p) {
                                     showAotProgress(p);
                                 })) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("AOT コンパイル不可: ") +
                          QString::fromStdString(why),
                      true);
            return;
        }
        const fs::path src(e->path);
        fs::path out = src.parent_path() / src.stem();
        std::error_code ec;
        if (fs::exists(out, ec))
            out = src.parent_path() / (src.stem().string() + "-aot");
        showAotProgress({aot::ProgressStage::WriteOutput});
        if (!aot::writeExecutable(out.string(), data.data(), data.size(), br,
                                  hash, &why)) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("書き出し失敗: ") +
                          QString::fromStdString(why),
                      true);
            return;
        }
        endAotProgress();
        setBusy(false);
        const int slotCount = br.slotCount();
        setStatus(QStringLiteral("書き出しました: ") +
                      QString::fromStdString(out.string()) + "（" +
                      QString::number(br.scripts) + " scripts / " +
                      QString::number(slotCount) + " slots）",
                  false);
#else
        setStatus(QStringLiteral(
                      "このビルドは JIT 無しのため AOT できません"),
                  true);
#endif
    }
    Q_INVOKABLE void runAotProject() {
#ifdef XYO_JIT
        const LauncherEntry* e = entryForPath(selPath_);
        if (!e || e->isDir || busy_) {
            if (!busy_)
                setStatus(QStringLiteral(
                              "AOT 実行するプロジェクトを選んでください"),
                          true);
            return;
        }
        setBusy(true);
        beginAotProgress(QStringLiteral("AOT 実行: ") +
                         QString::fromStdString(e->name));

        std::vector<uint8_t> data = readFileBytes(e->path);
        if (data.empty()) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("読み込みに失敗しました: ") +
                          QString::fromStdString(e->name),
                      true);
            return;
        }
        const uint64_t hash = aot::hashBytes(data.data(), data.size());
        Runtime rt;
        if (!loadSb3File(rt, data.data(), data.size()).ok) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral(
                          "プロジェクトのロードに失敗しました"),
                      true);
            return;
        }
        aot::BuildResult br;
        std::string why;
        if (!aot::compileProject(rt, hash, br, &why,
                                 [this](const aot::Progress& p) {
                                     showAotProgress(p);
                                 })) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("AOT コンパイル不可: ") +
                          QString::fromStdString(why),
                      true);
            return;
        }
        TemporaryAotObject temp;
        if (temp.path.empty()) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral(
                          "一時ファイルを作成できません"),
                      true);
            return;
        }
        showAotProgress({aot::ProgressStage::WriteOutput});
        if (!aot::writeObjectFile(temp.path, br, &why)) {
            endAotProgress();
            setBusy(false);
            setStatus(QStringLiteral("AOT オブジェクト書き出し失敗: ") +
                          QString::fromStdString(why),
                      true);
            return;
        }
        showAotProgress({aot::ProgressStage::LoadObject});
        rememberRecent(QString::fromStdString(e->path));
        pendingPath_ = e->path;
        {
            ScopedGuiRunFlags flags(options_, /*explicitAot=*/true);
            if (view_) view_->hide();
            xyoShellPlay(e->path.c_str(), temp.path.c_str(), options_.scale,
                         options_.turbo, options_.fps, options_.gpuMode,
                         [this](int stage, size_t done, size_t total) {
                             showAotProgress(
                                 {static_cast<aot::ProgressStage>(stage),
                                  done, total});
                         });
        }
        endAotProgress();
        setBusy(false);
        result_ = ResultContinue;
        QCoreApplication::quit();
#else
        setStatus(QStringLiteral(
                      "このビルドは JIT 無しのため AOT できません"),
                  true);
#endif
    }
    /// 自動検証アクション列を 1 つ消費して実行する。
    /// true = まだ続きがある or 実行した、false = キューが空。
    bool stepAction() {
        if (actIdx_ >= actions_.size()) return false;
        const std::string a = actions_[actIdx_++];
        const QString qa = QString::fromStdString(a);
        if (a == "down") moveSelectionRows(1, gridColsHint());
        else if (a == "up") moveSelectionRows(-1, gridColsHint());
        else if (a == "home") selectHome();
        else if (a == "end") selectEnd();
        else if (a == "enter" || a == "run") activate();
        else if (a == "aotrun") runAotProject();
        else if (a == "export") exportAot();
        else if (a == "edit") openEditor();
        else if (a == "back") goUp();
        else if (a == "backnav") goBack();
        else if (a == "forward") goForward();
        else if (a == "browse-home") navigate(homeDir_);
        else if (a == "examples") navigate(examplesDir_);
        else if (a == "sort") setSortRecent(!sortRecent());
        else if (a == "settings") Q_EMIT toggleSettingsRequested();
        else if (a.rfind("scale:", 0) == 0) setOptScale(std::atoi(a.c_str() + 6));
        else if (a.rfind("fps:", 0) == 0) setOptFps(std::atoi(a.c_str() + 4));
        else if (a.rfind("jit:", 0) == 0) {
            const std::string m = a.substr(4);
            if (m == "hotspot") setOptJit(0);
            else if (m == "eager") setOptJit(1);
            else if (m == "off") setOptJit(2);
        } else if (a.rfind("lazy:", 0) == 0) {
            const std::string m = a.substr(5);
            if (m == "auto") setOptLazy(0);
            else if (m == "on") setOptLazy(1);
            else if (m == "off") setOptLazy(2);
        } else if (a.rfind("gpu:", 0) == 0) {
            const std::string m = a.substr(4);
            if (m == "off") setOptGpu(-1);
            else if (m == "ray") setOptGpu(0);
            else if (m == "path") setOptGpu(1);
        } else if (a.rfind("turbo:", 0) == 0)
            setOptTurbo(a.substr(6) == "1");
        else if (a.rfind("danger:", 0) == 0)
            setOptDanger(a.substr(7) == "1");
        else if (a == "view-list") setListView(true);
        else if (a == "view-icons") setListView(false);
        else if (a.rfind("click:", 0) == 0) {
            int x = 0, y = 0;
            if (std::sscanf(a.c_str() + 6, "%d:%d", &x, &y) == 2 &&
                x >= 0 && y >= 0)
                simulateClick(x, y);
        } else if (a.rfind("resize:", 0) == 0) {
            int w = 0, h = 0;
            if (std::sscanf(a.c_str() + 7, "%dx%d", &w, &h) == 2 &&
                w >= 760 && w <= 4096 && h >= 560 && h <= 2160)
                resizeView(w, h);
        } else if (a.rfind("path:", 0) == 0)
            navigate(a.substr(5));
        else if (a.rfind("search:", 0) == 0)
            setFilterString(QString::fromStdString(a.substr(7)));
        else if (a == "recent0" && !recentStd_.empty())
            navigate(recentStd_.front());
        else if (a == "wheeldown") Q_EMIT scrollBy(64.0 * 3);
        else if (a == "wheelup") Q_EMIT scrollBy(-64.0 * 3);
        else if (a == "quit") quitApp();
        return true;
    }
    bool actionsDone() const { return actIdx_ >= actions_.size(); }

    /// 検証用: 現在のウィンドウ内容を PPM で保存する。
    void captureNow() {
        if (shotPath_.empty() || !view_) return;
        QImage img = view_->grabWindow();
        if (img.isNull()) {
            std::fprintf(stderr, "gui: shot grab failed\n");
            return;
        }
        img = img.convertToFormat(QImage::Format_RGB888);
        if (std::FILE* f = std::fopen(shotPath_.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", img.width(), img.height());
            for (int y = 0; y < img.height(); ++y)
                std::fwrite(img.constScanLine(y), 1,
                            size_t(img.width()) * 3, f);
            std::fclose(f);
        }
    }

    void setStatus(const QString& s, bool err) {
        status_ = s;
        statusErr_ = err;
        if (std::getenv("XYO_GUI_DEBUG") || err)
            std::fprintf(stderr, "gui: %s\n", s.toUtf8().constData());
        Q_EMIT statusChanged();
    }
    void setOptions(const GuiRunOptions& o) {
        options_ = o;
        Q_EMIT optionsChanged();
    }

    void openInitialDir(const std::string& d) { navigate(d, /*record=*/false); }

signals:
    void dirChanged();
    void filterChanged();
    void viewChanged();
    void statusChanged();
    void selectionChanged();
    void recentChanged();
    void optionsChanged();
    void busyChanged();
    void aotProgressChanged();
    void revealSelection();
    void scrollBy(qreal delta);
    void toggleSettingsRequested();

private:
    void setBusy(bool on) {
        busy_ = on;
        Q_EMIT busyChanged();
    }
    void persistOptions() {
        writeRunOptions(options_);
        Q_EMIT optionsChanged();
    }
    static std::vector<uint8_t> readFileBytes(const std::string& path) {
        std::vector<uint8_t> data;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
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
        return data;
    }
    QString existingSub(const QString& sub) const {
        if (homeDir_.empty()) return {};
        std::error_code ec;
        const fs::path p = fs::path(homeDir_) / sub.toStdString();
        return fs::is_directory(p, ec)
                   ? QString::fromStdString(p.string()) : QString();
    }
    const LauncherEntry* entryForPath(const QString& path) const {
        const std::string sp = path.toStdString();
        for (int i = 0; i < model_.count(); ++i) {
            const LauncherEntry* e = model_.entryAt(i);
            if (e && e->path == sp) return e;
        }
        return nullptr;
    }
    const LauncherEntry* entryAtProxy(int row) const {
        const QModelIndex idx = proxy_.index(row, 0);
        if (!idx.isValid()) return nullptr;
        const int src = proxy_.mapToSource(idx).row();
        return model_.entryAt(src);
    }
    int proxyRowForPath(const QString& path) const {
        for (int i = 0; i < proxy_.rowCount(); ++i) {
            const QModelIndex idx = proxy_.index(i, 0);
            if (idx.data(LauncherEntryModel::PathRole).toString() == path)
                return i;
        }
        return proxy_.rowCount() > 0 ? 0 : -1;
    }
    void setSelection(const QString& path, bool isDir) {
        selPath_ = path;
        selIsDir_ = isDir;
        Q_EMIT selectionChanged();
    }
    /// フィルタ/ソート変更後に選択を妥当な行へ直す。
    void fixSelectionAfterFilter() {
        if (proxy_.rowCount() <= 0) {
            setSelection(QString(), false);
            return;
        }
        if (!idxPathVisible(selPath_)) {
            const LauncherEntry* e = entryAtProxy(0);
            if (e) setSelection(QString::fromStdString(e->path), e->isDir);
        }
    }
    bool idxPathVisible(const QString& path) const {
        for (int i = 0; i < proxy_.rowCount(); ++i) {
            if (proxy_.index(i, 0)
                    .data(LauncherEntryModel::PathRole)
                    .toString() == path)
                return true;
        }
        return false;
    }
    void loadDir(const std::string& dir) {
        std::vector<LauncherEntry> entries;
        std::string abs;
        if (!listProjectDir(dir, entries, &abs)) {
            setStatus(QStringLiteral("フォルダを開けません: ") +
                          QString::fromStdString(dir),
                      true);
            return;
        }
        dir_ = abs;
        model_.setEntries(std::move(entries));
        proxy_.setNeedle(QString());
        const bool hadFilter = !filter_.isEmpty();
        filter_.clear();
        selPath_.clear();
        selIsDir_ = false;
        // 先頭のプロジェクトを選ぶ（無ければ先頭のフォルダ）。
        for (int i = 0; i < model_.count(); ++i) {
            const LauncherEntry* e = model_.entryAt(i);
            if (e && !e->isDir) {
                setSelection(QString::fromStdString(e->path), false);
                break;
            }
        }
        if (selPath_.isEmpty() && model_.count() > 0) {
            const LauncherEntry* e = model_.entryAt(0);
            setSelection(QString::fromStdString(e->path), e->isDir);
        }
        if (hadFilter) Q_EMIT filterChanged();
        Q_EMIT dirChanged();
        Q_EMIT selectionChanged();
        Q_EMIT revealSelection();
        if (std::getenv("XYO_GUI_DEBUG"))
            std::fprintf(stderr, "gui: dir=%s entries=%d\n", dir_.c_str(),
                         model_.count());
    }
    bool navigate(const std::string& raw, bool record = true) {
        std::string path = raw;
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        if (path == "~" && !homeDir_.empty())
            path = homeDir_;
        else if (path.size() >= 2 && path[0] == '~' &&
                 (path[1] == '/' || path[1] == '\\') && !homeDir_.empty())
            path = homeDir_ + path.substr(1);
        if (path.empty()) {
            setStatus(QStringLiteral(
                          "開くフォルダのパスを入力してください"),
                      true);
            return false;
        }
        std::error_code ec;
        fs::path target(path);
        if (target.is_relative()) target = fs::path(dir_) / target;
        target = fs::absolute(target, ec).lexically_normal();
        if (ec) {
            setStatus(QStringLiteral("パスを解決できません: ") +
                          QString::fromStdString(path),
                      true);
            return false;
        }
        const bool isFile = fs::is_regular_file(target, ec);
        if (isFile) {
            const std::string ext = lowerAscii(target.extension().string());
            if (ext != ".sb3" && ext != ".sb2") {
                setStatus(QStringLiteral(
                              ".sb3 / .sb2 ファイルを選んでください"),
                          true);
                return false;
            }
        } else if (!fs::is_directory(target, ec)) {
            setStatus(QStringLiteral("フォルダが見つかりません: ") +
                          QString::fromStdString(path),
                      true);
            return false;
        }
        const std::string nextDir =
            (isFile ? target.parent_path() : target).string();
        if (record && !dir_.empty() && dir_ != nextDir) {
            backDirs_.push_back(currentNav());
            if (backDirs_.size() > 64) backDirs_.erase(backDirs_.begin());
            forwardDirs_.clear();
        }
        if (dir_ != nextDir || model_.count() == 0) loadDir(nextDir);
        if (isFile) {
            const QString fp = QString::fromStdString(target.string());
            const LauncherEntry* e = entryForPath(fp);
            if (e) {
                setSelection(fp, false);
                // フィルタで隠れていた場合は解除して再選択する。
                if (!idxPathVisible(fp)) {
                    filter_.clear();
                    proxy_.setNeedle(QString());
                    Q_EMIT filterChanged();
                    fixSelectionAfterFilter();
                    setSelection(fp, false);
                }
                Q_EMIT revealSelection();
            }
        }
        return true;
    }
    NavState currentNav() const {
        return {dir_, selPath_.toStdString()};
    }
    void restoreNav(const NavState& nav) {
        loadDir(nav.dir);
        if (!nav.selection.empty()) {
            const QString sp = QString::fromStdString(nav.selection);
            if (entryForPath(sp)) {
                setSelection(sp, entryForPath(sp)->isDir);
                Q_EMIT revealSelection();
            }
        }
    }
    void rememberRecent(const QString& path) {
        rememberRecentProject(recentStd_, path.toStdString());
        Q_EMIT recentChanged();
    }
    int gridColsHint() const {
        // レイアウトの列数推定（moveSelectionRow の近似）
        if (listView_) return 1;
        return view_ ? std::clamp(int((view_->width() - 218 - 24 - 24 + 16) /
                                      (192 + 16)),
                                  1, 5)
                     : 3;
    }

#ifdef XYO_JIT
    void beginAotProgress(const QString& operation) {
        aotOperation_ = operation;
        aotStartedAt_ = std::chrono::steady_clock::now();
        aotLastPump_ = {};
        aotProgress_ = -1.0;
        setStatus(aotOperation_ +
                      QStringLiteral(" — プロジェクトを読み込み中"),
                  false);
        Q_EMIT aotProgressChanged();
        QCoreApplication::processEvents();
    }
    void showAotProgress(const aot::Progress& p) {
        // 単スレッドの LLVM コンパイルが意味のあるチェックポイントで
        // 呼ぶ。UI を固めたままにしないよう QML を再描画する。
        const auto now = std::chrono::steady_clock::now();
        if (now - aotLastPump_ < std::chrono::milliseconds(80) &&
            p.total > 0)
            return;
        aotLastPump_ = now;
        aotProgress_ =
            p.total ? std::clamp(double(p.completed) / p.total, 0.0, 1.0)
                    : -1.0;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - aotStartedAt_)
                .count();
        setStatus(aotOperation_ + " — " +
                      QString::fromStdString(aot::progressText(p)) +
                      "  ·  " + QString::number(seconds) + "秒",
                  false);
        Q_EMIT aotProgressChanged();
        QCoreApplication::processEvents();
    }
    void endAotProgress() {
        aotProgress_ = -1.0;
        Q_EMIT aotProgressChanged();
    }
#endif

    LauncherEntryModel model_;
    LauncherSortFilter proxy_;
    QQuickView* view_ = nullptr;
    std::string dir_;
    QString selPath_;
    bool selIsDir_ = false;
    QString filter_;
    bool listView_ = true;
    std::vector<NavState> backDirs_, forwardDirs_;
    std::vector<std::string> recentStd_;
    std::string homeDir_, examplesDir_;
    GuiRunOptions options_;
    QString status_;
    bool statusErr_ = false;
    bool busy_ = false;
    double aotProgress_ = -1.0;
    QString aotOperation_;
    std::chrono::steady_clock::time_point aotStartedAt_{};
    std::chrono::steady_clock::time_point aotLastPump_{};
    int result_ = ResultNone;
    std::string pendingPath_;
    std::vector<std::string> actions_;
    size_t actIdx_ = 0;
    bool oneShot_ = false;
    std::string shotPath_;
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
            const std::string home = userHome();
            if (!home.empty()) start = home;
        }
    }
    auto actions = splitActions(std::getenv("XYO_GUI_ACTIONS"));
    // 検証モード: アクション列を与えたら 1 回の再生でプロセスを終える
    // （そうしないとランチャーが再帰的に開いてテストが止まらない）。
    const bool oneShot = !actions.empty();
    const char* dbg = std::getenv("XYO_GUI_DEBUG");

    static int appArgc = 1;
    static char appArg0[] = "xyo";
    static char* appArgv[] = {appArg0, nullptr};
    QGuiApplication app(appArgc, appArgv);
    QCoreApplication::setApplicationName(QStringLiteral("xyo"));

    const long exitFrames = [] {
        if (const char* v = std::getenv("XYO_GUI_EXIT_FRAMES"))
            return std::atol(v);
        return 0L;
    }();

    GuiRunOptions options = readRunOptions();
    for (;;) {
        LauncherController controller;
        controller.setOneShot(oneShot);
        if (oneShot) controller.setActions(actions);
        if (const char* sh = std::getenv("XYO_GUI_SHOT"))
            controller.setShotPath(sh);
        controller.setOptions(options);
        controller.openInitialDir(start);

        QQuickView view;
        view.engine()->addImageProvider("xyothumb",
                                        new ThumbImageProvider);
        view.rootContext()->setContextProperty("launcher", &controller);
        view.rootContext()->setContextProperty("entryModel",
                                               controller.proxyModel());
        view.setTitle(QStringLiteral("xyo — Project library"));
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setColor(QColor(0xF4, 0xF6, 0xFB));
        view.setSource(QUrl("qrc:/xyo/qml/Launcher.qml"));
        if (view.status() == QQuickView::Error) {
            std::fprintf(stderr, "gui: QML の読み込みに失敗しました\n");
            for (const auto& e : view.errors())
                std::fprintf(stderr, "gui: %s\n",
                             e.toString().toUtf8().constData());
            return 1;
        }
        view.resize(1120, 760);
        view.setMinimumSize(QSize(760, 560));
        controller.attach(&view);
        view.show();

        // 検証用: アクション列をタイマーで順に実行する。
        QTimer actionTimer;
        long tickFrames = 0;
        int doneStall = 0;
        if (oneShot || exitFrames > 0) {
            actionTimer.setInterval(30);
            QObject::connect(&actionTimer, &QTimer::timeout, [&] {
                ++tickFrames;
                if (oneShot) {
                    if (controller.stepAction()) {
                        doneStall = 0;
                        return;
                    }
                    // キュー消化後、描画が落ち着いてから終了
                    if (++doneStall > 20) {
                        controller.captureNow();
                        controller.quitApp();
                    }
                } else if (exitFrames > 0 && tickFrames >= exitFrames) {
                    controller.captureNow();
                    controller.quitApp();
                }
            });
            actionTimer.start();
        }

        app.exec();

        // 旧 SDL 版と同様、ループを抜ける直前の状態を残す。
        QCoreApplication::processEvents();
        controller.captureNow();

        const int act = controller.result();
        const std::string runPath = controller.pendingPath();
        options = controller.runOptions();
        if (dbg)
            std::fprintf(stderr,
                         "gui: act=%d path=%s (scale=%d fps=%d turbo=%d "
                         "jit=%d lazy=%d gpu=%d danger=%d)\n",
                         act, runPath.c_str(), options.scale, options.fps,
                         int(options.turbo), options.jitMode,
                         options.lazyMode, options.gpuMode,
                         int(options.danger));

        if (act == LauncherController::ResultQuit ||
            act == LauncherController::ResultNone)
            return 0;
        if (act == LauncherController::ResultContinue) {
            if (oneShot) return 0;
            continue;  // AOT 実行済み
        }
        view.hide();
        if (act == LauncherController::ResultEdit) {
            if (!runPath.empty())
                editorMain(runPath.c_str());
            if (oneShot) return 0;
            continue;
        }
        if (act == LauncherController::ResultRun && !runPath.empty()) {
            ScopedGuiRunFlags flags(options);
            xyoShellPlay(runPath.c_str(), nullptr, options.scale,
                         options.turbo, options.fps, options.gpuMode);
            if (oneShot) return 0;
            continue;
        }
        return 0;
    }
}

} // namespace xyo

#include "gui_qt.moc"

#else // !XYO_HAVE_QT

#include <cstdio>

namespace xyo {
int guiMain(const char*) {
    std::fprintf(stderr,
                 "xyo was built without Qt6 Quick support (install Qt6 "
                 "and rebuild to use 'xyo gui')\n");
    return 1;
}
} // namespace xyo

#endif // XYO_HAVE_QT
