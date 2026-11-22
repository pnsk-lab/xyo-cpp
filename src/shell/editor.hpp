// xyo-cpp: ビルトインエディタ。
// scratch-gui のレイアウトを再現する単一ウィンドウ構成。
// 実装は Qt Quick 版（editor_qt.cpp + qml/Editor.qml）。
#pragma once
#include <cstdio>

namespace xyo {

/// エディタを起動する。path がプロジェクトファイル。終了コードを返す。
/// danger はスレッドごとの実行時間予算を外す危険モード（`--danger`）。
/// Qt6 Quick 必須（XYO_HAVE_QT 未定義ビルドは stub が 1 を返す）。
#ifdef XYO_HAVE_QT
int editorMainQt(const char* path, bool danger = false);
#else
inline int editorMainQt(const char* path, bool = false) {
    std::fprintf(stderr,
                 "editor: %s (this build has no Qt6 Quick)\n", path);
    return 1;
}
#endif

/// 旧 SDL 手描き版（editor_sdl.cpp）。参照用に残す。
int editorMainLegacy(const char* path);

} // namespace xyo
