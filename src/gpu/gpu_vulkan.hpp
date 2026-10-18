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
/// 戻り値 false なら呼出し側は CPU ループへフォールバックする。
bool vulkanExecParallel(ExecRequest& req);

} // namespace xyo::gpu
