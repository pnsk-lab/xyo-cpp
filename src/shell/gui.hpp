// xyo-cpp: ランチャー GUI。CLI を使わずにプロジェクトを選んで
// 実行 / AOT 事前コンパイル → 単体実行ファイルの書き出しができる。
#pragma once

namespace xyo {

/// ランチャーを起動する。startDir が空なら実行ディレクトリ
/// （.sb3 が無ければ $HOME）から始める。終了コードを返す。
int guiMain(const char* startDir);

} // namespace xyo
