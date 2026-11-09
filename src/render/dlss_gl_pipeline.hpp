// Optional real NVIDIA DLSS Super Resolution in the SDL OpenGL display path.
// Linux-only. Opt in via XYO_DLSS_REAL=1, XYO_DLSS_FEATURE_DIR=...,
// and --render-scale 0.5. For arbitrary Scratch pen scenes, depth and
// motion vectors are unavailable: experimental per-frame history reset.
#pragma once
#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__) && defined(XYO_HAVE_SDL2)
#include "dlss_vk_backend.hpp"
#include "dlss_vk_images.hpp"
#include "dlss_gl_external.hpp"
#include "dlss_gl_vk_sync.hpp"
#include "dlss_vk_optical_flow.hpp"
#include "dlss_vk_motion_converter.hpp"
#include <SDL.h>
#include <vulkan/vulkan.h>
#include <string>

namespace xyo::render {

class DlssGlPipeline {
public:
    DlssGlPipeline() = default;
    ~DlssGlPipeline();
    DlssGlPipeline(const DlssGlPipeline&) = delete;
    DlssGlPipeline& operator=(const DlssGlPipeline&) = delete;

    // Real NGX dispatch with Vulkan->GL OPAQUE_FD memory sharing.
    // Returns a renderer-owned SDL GPU texture ready for SDL_RenderCopy,
    // or nullptr, leaving caller free to use GL upscale/FSR fallback.
    // forceHistoryReset is a per-frame override for content that cannot
    // safely supply geometric motion (e.g. moving semi-transparent sprites).
    // The main renderer uses it only under XYO_DLSS_ALPHA_SAFE=1.
    SDL_Texture* apply(SDL_Renderer* renderer, SDL_Texture* input,
                       int outputWidth, int outputHeight,
                       bool forceHistoryReset = false);
    void reset();
    // Diagnostic: last Optical Flow confidence assessment, updated
    // after GPU synchronization. Useful for real-GPU regression tests.
    bool motionLowConfidence() const { return resetForBadMotion_; }

private:
    bool initialize(SDL_Renderer* renderer);
    bool resize(SDL_Renderer* renderer, int inW, int inH, int outW, int outH);
    void discardImages();
    void fail(const std::string& error);

    bool tried_ = false;
    bool supported_ = false;
    bool submitted_ = false;
    DlssVkPreset activePreset_ = DlssVkPreset::K;
    DlssVkMode activeMode_ = DlssVkMode::Quality;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    bool opticalEnabled_ = false;
    uint32_t opticalFamily_ = UINT32_MAX;
    VkQueue opticalQueue_ = VK_NULL_HANDLE;
    VkCommandBuffer prepCmd_ = VK_NULL_HANDLE;
    VkSemaphore prepDone_ = VK_NULL_HANDLE;
    VkSemaphore flowDone_ = VK_NULL_HANDLE;
    VkFence opticalCostFence_ = VK_NULL_HANDLE;
    bool resetForBadMotion_ = false;
    uint64_t frameNumber_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    // Optional Vulkan timestamps: record actual GPU NGX command time,
    // not CPU command encoding/submission time.
    VkQueryPool gpuTimerPool_ = VK_NULL_HANDLE;
    double gpuTimestampNanoseconds_ = 0.0;
    double gpuTotalMs_ = 0.0;
    uint64_t gpuSamples_ = 0;
    DlssVkBackend ngx_;
    DlssVkFrameTargets images_;
    DlssVkOpticalFlow optical_;
    DlssVkMotionConverter motionConverter_;
    DlssGlSharedTexture glColor_;
    DlssGlSharedTexture glOutput_;
    DlssGlVkSync sync_;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* display_ = nullptr;
    // Keep two GL framebuffer objects for the lifetime of the pipeline,
    // rather than creating and deleting them twice per displayed frame.
    GLuint copyFbos_[2]{};

    int inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
    std::string error_;
};

} // namespace xyo::render
#endif
