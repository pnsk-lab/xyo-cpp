// xyo-cpp: ブロックグラフの基本操作（Blockly の接続モデル）。
//
// next チェーン・value input・statement input への接続を、parent/topLevel/
// scripts の整合を保ったまま行う。描画にもイベントにも依存しない。
#pragma once

#include <string>

namespace xyo {

class Blocks;

namespace blockly {

/// anchor の直後に head..tail を挿入する。
void insertAfter(Blocks* blocks, const std::string& head, const std::string& tail,
                 const std::string& anchor);
/// anchor の直前に head..tail を挿入する（先頭なら新しいトップレベルになる）。
void insertBefore(Blocks* blocks, const std::string& head, const std::string& tail,
                  const std::string& anchor);
/// value input（inputName）へ head..tail を接続する。
void attachToValue(Blocks* blocks, const std::string& head, const std::string& target,
                   const std::string& inputName);
/// C-block の本体へ head..tail を接続する（branch 0=SUBSTACK, 2=SUBSTACK2）。
void attachToSubstack(Blocks* blocks, const std::string& head, const std::string& tail,
                      const std::string& target, int branch);
/// head を新しいトップレベルスクリプトにする（parent を切る）。
void makeTopLevel(Blocks* blocks, const std::string& head);

} // namespace blockly
} // namespace xyo
