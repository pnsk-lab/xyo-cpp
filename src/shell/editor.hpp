// xyo-cpp: ビルトインエディタ（SDL2）。
// scratch-gui のレイアウトを参考にした内蔵エディタ。
// ステージウィンドウ（SdlRenderer）+ インスペクタウィンドウの 2 ウィンドウ構成。
#pragma once

namespace xyo {

/// エディタを起動する。path がプロジェクトファイル。終了コードを返す。
int editorMain(const char* path);

} // namespace xyo