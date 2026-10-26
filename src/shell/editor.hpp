// xyo-cpp: ビルトインエディタ（Dear ImGui + SDL2 バックエンド）。
// scratch-gui のレイアウトを完全再現する単一ウィンドウ構成。
#pragma once

namespace xyo {

/// エディタを起動する。path がプロジェクトファイル。終了コードを返す。
/// danger はスレッドごとの実行時間予算を外す危険モード（`--danger`）。
int editorMain(const char* path, bool danger = false);
/// 旧 SDL 手描き版（editor_sdl.cpp）。参照用に残す。
int editorMainLegacy(const char* path);

} // namespace xyo
