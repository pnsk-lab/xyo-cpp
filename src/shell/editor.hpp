// xyo-cpp: ビルトインエディタ。
// scratch-gui のレイアウトを再現する単一ウィンドウ構成。
// Qt ありの環境では Qt Quick 版（editor_qt.cpp）が既定。
#pragma once

namespace xyo {

/// エディタを起動する。path がプロジェクトファイル。終了コードを返す。
/// danger はスレッドごとの実行時間予算を外す危険モード（`--danger`）。
/// エントリは環境により Qt Quick 版（editor_qt.cpp）を優先する。
int editorMain(const char* path, bool danger = false);

#ifdef XYO_HAVE_QT
/// Qt Quick 版エディタ（Editor.qml）。Qt6 Quick 必須。
int editorMainQt(const char* path, bool danger = false);
#else
/// Qt 無しビルドでは SDL 版へフォールバック。
inline int editorMainQt(const char* path, bool danger = false) {
    return editorMain(path, danger);
}
#endif

/// 旧 SDL 手描き版（editor_sdl.cpp）。参照用に残す。
int editorMainLegacy(const char* path);

} // namespace xyo
