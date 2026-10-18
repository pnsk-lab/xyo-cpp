#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gpu_isa.hpp"

namespace xyo {
class Blocks;
}

namespace xyo::gpu {

enum class RealtimeMode : uint32_t { Ray, Path };

struct NativeProgram {
    std::string source;
    std::vector<GlobalVar> vars;
    std::vector<GlobalVar> lists;
    std::vector<int32_t> writtenVars;
    GlobalVar accumVars[4];
    GlobalVar stackVar;
    int32_t slotRes = -1;
    int32_t slotColVar = -1, slotRowVar = -1;
    int32_t slotStepX = -1, slotStepY = -1;
    int32_t slotPI = -1, slotPN = -1;
    int32_t slotThisColor = -1, slotLastColor = -1;
    bool path = false;
    bool sharedStack = false;
    bool inlineLocals = false;
};

std::optional<NativeProgram> compileRayKernel(Blocks* blocks, bool path,
                                              std::string* why = nullptr);

constexpr uint32_t kNativeParamBase = 32;
constexpr uint32_t kNativeOutStatus = 2;
constexpr uint32_t kNativeOutPen = 8;
constexpr uint32_t kNativeIterBudget = 1000000;
constexpr uint32_t kNativeStackCap = 128;
constexpr uint32_t kNativePathMaxPerLane = 1;
constexpr size_t kNativeSourceCap = 1u << 20;
constexpr uint32_t kNativeDepthCap = 256;

struct NativeRequest {
    const NativeProgram* program = nullptr;
    const float* vars = nullptr;
    uint32_t nVars = 0;
    const float* listData = nullptr;
    size_t listElems = 0;
    const uint32_t* listMeta = nullptr;
    uint32_t nLists = 0;
    uint64_t sceneVersion = 0;
    uint32_t cols = 0, rows = 0;
    float res = 1;
    uint32_t baseSeed = 1;
    float fenceL = -240, fenceR = 240, fenceB = -180, fenceT = 180;
    float startX = 0, startY = 0, penSize = 1;
    float penRGBA[4] = {0, 0, 1, 1};
    bool penDown = false;
    bool miscLimits = true;
    float lastColor = 0;
    const float* accumIn = nullptr;
    uint32_t accumLens[4] = {0, 0, 0, 0};
    bool accumReset = false;

    const float* scalarOut = nullptr;
    uint32_t nScalarOut = 0;
    const float* accumOut = nullptr;
    bool executed = false;
    bool overflow = false;
};

}
