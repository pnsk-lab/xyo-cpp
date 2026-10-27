// xyo-cpp: Vulkan compute による GPU 並列ループ executor。
// XYO_GPU_VULKAN 未定義ビルドでは両関数が false を返すスタブ。
#pragma once

#include "gpu_isa.hpp"

namespace xyo::gpu {

/// executor が利用可能か（lazy init — vulkan ローダ・device・
/// pipeline 構築まで試行。失敗時は false で CPU フォールバック）。
bool vulkanExecSupported();

/// ExecRequest を compute dispatch で実行。GL 版と同一の要求/結果
/// 意味論だが、ストロークのラスタ化は行わない（ペン層が無い
/// headless/Windows 用途 — nRecords は描画相当の件数のみ返す）。
/// 既定では未使用のstroke payloadは生成・確保しない。件数とoverflow/
/// instruction guard、ペン状態とwritebackは同じ共有VM経路を使う。
/// XYO_GPU_VK_RECORDS=1 は診断/A/B用に従来の記録バッファを有効にする。
/// 戻り値 false なら呼出し側は CPU ループへフォールバックする。
bool vulkanExecParallel(ExecRequest& req);

} // namespace xyo::gpu
