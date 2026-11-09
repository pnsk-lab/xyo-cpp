// Human-readable presentation of factual AOT stage checkpoints.
#pragma once
#include "aot.hpp"
#include <string>

namespace xyo::aot {
inline const char* progressStageName(ProgressStage stage) {
    switch (stage) {
    case ProgressStage::Codegen:         return "スクリプト生成";
    case ProgressStage::Optimize:        return "LLVM O2 最適化";
    case ProgressStage::EmitObject:      return "オブジェクト生成";
    case ProgressStage::Compiled:        return "コンパイル完了";
    case ProgressStage::WriteOutput:     return "ファイル書き出し";
    case ProgressStage::LoadObject:      return "AOT オブジェクト読込";
    case ProgressStage::BindSlots:       return "参照スロット束縛";
    case ProgressStage::RegisterScripts: return "スクリプト登録";
    case ProgressStage::Loaded:          return "AOT ロード完了";
    }
    return "AOT";
}
inline std::string progressText(const Progress& p) {
    std::string s = progressStageName(p.stage);
    if (p.total > 0) {
        s += " " + std::to_string(p.completed) + "/" +
             std::to_string(p.total) + " (" +
             std::to_string(p.completed * 100 / p.total) + "%)";
    } else if (p.stage == ProgressStage::Optimize ||
               p.stage == ProgressStage::EmitObject ||
               p.stage == ProgressStage::LoadObject ||
               p.stage == ProgressStage::WriteOutput) {
        s += "（進捗率不明）";
    }
    return s;
}
} // namespace xyo::aot
