#define GL_GLEXT_PROTOTYPES
#include "dlss_gl_pipeline.hpp"
#include "dlss_optical_confidence.hpp"
#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__) && defined(XYO_HAVE_SDL2)
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <vector>

namespace xyo::render {
namespace {
constexpr char kProjectId[] = "ba7388e4-905b-4a89-892c-4df5a0773046";

GLuint textureId(SDL_Texture* texture) {
    if (!texture || SDL_GL_BindTexture(texture, nullptr, nullptr)) return 0;
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    SDL_GL_UnbindTexture(texture);
    return id > 0 ? GLuint(id) : 0;
}

bool blit(GLuint readFbo, GLuint drawFbo,
          GLuint src, GLuint dst,
          int srcW, int srcH, int dstW, int dstH) {
    GLint originalRead = 0, originalDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &originalRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &originalDraw);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glFramebufferTexture(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
    glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst, 0);
    const bool complete =
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, dstW, dstH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    const bool ok = complete && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, originalRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, originalDraw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    return ok;
}

DlssVkMode selectedMode() {
    const char* value = std::getenv("XYO_DLSS_MODE");
    if (value && (!std::strcmp(value, "performance") ||
                  !std::strcmp(value, "perf")))
        return DlssVkMode::Performance;
    if (value && !std::strcmp(value, "balanced"))
        return DlssVkMode::Balanced;
    return DlssVkMode::Quality;
}

const char* modeLabel(DlssVkMode mode) {
    switch (mode) {
        case DlssVkMode::Performance: return "Performance";
        case DlssVkMode::Balanced: return "Balanced";
        default: return "Quality";
    }
}

DlssVkPreset selectedPreset() {
    const char* value = std::getenv("XYO_DLSS_PRESET");
    if (value && (!std::strcmp(value, "J") || !std::strcmp(value, "j")))
        return DlssVkPreset::J;
    if (value && (!std::strcmp(value, "M") || !std::strcmp(value, "m")))
        return DlssVkPreset::M;
    if (value && (!std::strcmp(value, "auto") ||
                  !std::strcmp(value, "default")))
        return DlssVkPreset::Auto;
    return DlssVkPreset::K;
}

const char* presetLabel(DlssVkPreset preset) {
    switch (preset) {
        case DlssVkPreset::K: return "K (transformer hint)";
        case DlssVkPreset::J: return "J (transformer hint)";
        case DlssVkPreset::M: return "M (Performance hint)";
        default: return "driver default";
    }
}

DlssVkImage ngxImage(const DlssVkOwnedImage& x) {
    DlssVkImage out{};
    out.image = x.image;
    out.view = x.view;
    out.format = x.format;
    out.width = x.width;
    out.height = x.height;
    out.range = {x.aspect, 0, 1, 0, 1};
    return out;
}
// Submit one command buffer on Vulkan 1.3, with optional binary semaphore
// wait/signal. This is used only when the NV optical-flow path is enabled.
VkResult submitBinary(VkQueue queue, VkCommandBuffer cmd,
                      VkSemaphore wait, VkPipelineStageFlags2 waitStage,
                      VkSemaphore signal, VkFence fence = VK_NULL_HANDLE) {
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wi.semaphore = wait;
    wi.stageMask = waitStage;
    VkSemaphoreSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    si.semaphore = signal;
    si.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &ci;
    info.waitSemaphoreInfoCount = wait ? 1u : 0u;
    info.pWaitSemaphoreInfos = wait ? &wi : nullptr;
    info.signalSemaphoreInfoCount = signal ? 1u : 0u;
    info.pSignalSemaphoreInfos = signal ? &si : nullptr;
    return vkQueueSubmit2(queue, 1, &info, fence);
}

} // namespace

DlssGlPipeline::~DlssGlPipeline() { reset(); }

void DlssGlPipeline::fail(const std::string& why) {
    error_ = why;
    supported_ = false;
    if (std::getenv("XYO_DLSS_REAL_DEBUG"))
        std::fprintf(stderr, "[real DLSS] %s; using GL fallback\n", why.c_str());
}

bool DlssGlPipeline::initialize(SDL_Renderer* renderer) {
    if (tried_) return supported_;
    tried_ = true;
    if (!renderer || !SDL_GL_GetCurrentContext() ||
        !SDL_GL_ExtensionSupported("GL_EXT_memory_object_fd") ||
        !SDL_GL_ExtensionSupported("GL_EXT_semaphore_fd")) {
        fail("OpenGL external memory/semaphore extensions unavailable");
        return false;
    }
    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(renderer, &info) || !info.name ||
        std::strcmp(info.name, "opengl") != 0) {
        fail("SDL OpenGL renderer required");
        return false;
    }
    const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* glGpu = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (!glVendor || !std::strstr(glVendor, "NVIDIA") || !glGpu) {
        fail("NVIDIA OpenGL renderer required");
        return false;
    }

    std::error_code fsError;
    const auto tempDir = std::filesystem::temp_directory_path(fsError);
    if (fsError) {
        fail("cannot locate writable NVIDIA NGX data directory");
        return false;
    }
    auto data = tempDir / "xyo-cpp-ngx";
    std::filesystem::create_directories(data, fsError);
    if (fsError) {
        fail("cannot create writable NVIDIA NGX data directory");
        return false;
    }
    const std::wstring path = data.wstring();
    std::vector<std::string> extensions;
    if (!DlssVkBackend::requiredInstanceExtensions(
            kProjectId, path, extensions)) {
        fail("NVIDIA NGX instance extension discovery failed");
        return false;
    }
    std::vector<const char*> extNames;
    for (const auto& e : extensions) extNames.push_back(e.c_str());
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xyo-cpp";
    const bool requestOptical = [] {
        const char* env = std::getenv("XYO_DLSS_OPTICAL_FLOW");
        return env && env[0] == '1';
    }();
    app.apiVersion = requestOptical ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(extNames.size());
    ci.ppEnabledExtensionNames = extNames.data();
    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        fail("vkCreateInstance failed");
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (!count || vkEnumeratePhysicalDevices(instance_, &count, devices.data())
                      != VK_SUCCESS) {
        fail("no Vulkan devices");
        return false;
    }
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.vendorID == 0x10DE &&
            std::string(glGpu).find(props.deviceName) != std::string::npos) {
            physical_ = candidate;
            break;
        }
    }
    if (!physical_) {
        fail("Vulkan GPU does not match SDL OpenGL GPU");
        return false;
    }

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> queues(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, queues.data());
    bool found = false;
    for (uint32_t i = 0; i < families; ++i)
        if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily_ = i;
            found = true;
            break;
        }
    const bool supportsTimestamps = found &&
        queues[queueFamily_].timestampValidBits > 0;
    opticalFamily_ = UINT32_MAX;
    if (requestOptical) {
        for (uint32_t i = 0; i < families; ++i) {
            if ((queues[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) &&
                (queues[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                opticalFamily_ = i;
                break;
            }
        }
    }
    if (!found ||
        !DlssVkBackend::requiredDeviceExtensions(
            instance_, physical_, kProjectId, path, extensions)) {
        fail("NGX Vulkan compute queue/extensions unavailable");
        return false;
    }
    VkPhysicalDeviceOpticalFlowFeaturesNV flowFeatures{};
    flowFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &flowFeatures;
    VkPhysicalDeviceFeatures2 capabilities{};
    capabilities.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    capabilities.pNext = &features13;
    if (requestOptical && opticalFamily_ != UINT32_MAX)
        vkGetPhysicalDeviceFeatures2(physical_, &capabilities);
    opticalEnabled_ = requestOptical &&
                      opticalFamily_ != UINT32_MAX &&
                      flowFeatures.opticalFlow && features13.synchronization2;
    if (opticalEnabled_) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr,
                                            &extCount, nullptr);
        std::vector<VkExtensionProperties> supported(extCount);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr,
                                            &extCount, supported.data());
        opticalEnabled_ = std::any_of(
            supported.begin(), supported.end(),
            [](const VkExtensionProperties& ext) {
                return std::strcmp(ext.extensionName,
                                   VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0;
            });
    }
    if (opticalEnabled_) {
        features13.synchronization2 = VK_TRUE;
        flowFeatures.opticalFlow = VK_TRUE;
        if (std::find(extensions.begin(), extensions.end(),
                      VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == extensions.end())
            extensions.emplace_back(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
    } else if (requestOptical && std::getenv("XYO_DLSS_REAL_DEBUG")) {
        std::fprintf(stderr,
            "[real DLSS] VK_NV_optical_flow unsupported, using zero MVs\n");
    }
    for (const char* extra : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                               VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
        if (std::find(extensions.begin(), extensions.end(), extra) ==
            extensions.end()) extensions.emplace_back(extra);
    extNames.clear();
    for (const auto& e : extensions) extNames.push_back(e.c_str());
    float priority = 1;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    VkDeviceQueueCreateInfo flowQci = qci;
    flowQci.queueFamilyIndex = opticalFamily_;
    VkDeviceQueueCreateInfo queuesToCreate[2] = {qci, flowQci};
    dci.queueCreateInfoCount =
        opticalEnabled_ && opticalFamily_ != queueFamily_ ? 2 : 1;
    dci.pQueueCreateInfos = queuesToCreate;
    if (opticalEnabled_) dci.pNext = &features13;
    dci.enabledExtensionCount = uint32_t(extNames.size());
    dci.ppEnabledExtensionNames = extNames.data();
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        fail("vkCreateDevice external extensions unavailable");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (opticalEnabled_)
        vkGetDeviceQueue(device_, opticalFamily_, 0, &opticalQueue_);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) {
        fail("vkCreateCommandPool failed");
        return false;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd_) != VK_SUCCESS) {
        fail("vkAllocateCommandBuffers failed");
        return false;
    }
    if (opticalEnabled_) {
        VkFenceCreateInfo costFenceInfo{};
        costFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &costFenceInfo, nullptr,
                          &opticalCostFence_) != VK_SUCCESS) {
            fail("OpticalFlow confidence fence allocation failed");
            return false;
        }
        if (vkAllocateCommandBuffers(device_, &ai, &prepCmd_) != VK_SUCCESS) {
            fail("Vulkan optical-flow preparation command buffer failed");
            return false;
        }
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &sci, nullptr, &prepDone_) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sci, nullptr, &flowDone_) != VK_SUCCESS) {
            fail("Vulkan optical-flow semaphores failed");
            return false;
        }
    }
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fi, nullptr, &fence_) != VK_SUCCESS) {
        fail("vkCreateFence failed");
        return false;
    }

    // GPU timing is opt-in and does not create a synchronization dependency.
    // The preceding frame's fence is already waited on for command-buffer
    // reuse; query results can be read without a new GPU stall.
    if (std::getenv("XYO_DLSS_GPU_TIMING") && supportsTimestamps) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        VkQueryPoolCreateInfo timerInfo{};
        timerInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        timerInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        timerInfo.queryCount = 2;
        if (vkCreateQueryPool(device_, &timerInfo, nullptr,
                              &gpuTimerPool_) == VK_SUCCESS)
            gpuTimestampNanoseconds_ = properties.limits.timestampPeriod;
    }
    std::wstring featureDir;
    if (const char* env = std::getenv("XYO_DLSS_FEATURE_DIR"))
        featureDir = std::filesystem::path(env).wstring();
    if (!ngx_.initialize(instance_, physical_, device_, kProjectId, path,
                         vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                         featureDir)) {
        fail(ngx_.lastError() + "; set XYO_DLSS_FEATURE_DIR");
        return false;
    }
    activePreset_ = selectedPreset();
    activeMode_ = selectedMode();
    frameNumber_ = 0;
    supported_ = true;
    renderer_ = renderer;
    if (std::getenv("XYO_DLSS_REAL_DEBUG"))
        std::fprintf(stderr, "[real DLSS] NVIDIA NGX initialized on %s; "
                     "requested preset: %s\n",
                     glGpu, presetLabel(selectedPreset()));
    return true;
}

void DlssGlPipeline::discardImages() {
    sync_.destroy();
    glOutput_.destroy();
    glColor_.destroy();
    if (display_) SDL_DestroyTexture(display_);
    display_ = nullptr;
    if (copyFbos_[0] || copyFbos_[1]) {
        glDeleteFramebuffers(2, copyFbos_);
        copyFbos_[0] = copyFbos_[1] = 0;
    }
    biasMask_.destroy();
    motionConverter_.destroy();
    optical_.destroy();
    images_.destroy();
    inW_ = inH_ = outW_ = outH_ = 0;
    frameNumber_ = 0;
    resetForBadMotion_ = false;
    submitted_ = false;
}

bool DlssGlPipeline::resize(SDL_Renderer* renderer, int w, int h,
                            int ow, int oh) {
    if (display_ && w == inW_ && h == inH_ && ow == outW_ && oh == outH_)
        return true;
    // Resize is rare. Synchronize before releasing GL/VK shared allocations.
    if (device_) vkDeviceWaitIdle(device_);
    SDL_RenderFlush(renderer);
    glFinish();
    // NGX may hold references to the Vulkan image views during feature
    // evaluation; release its feature while those images are still alive.
    ngx_.releaseFeature();
    discardImages();
    if (std::getenv("XYO_DLSS_REAL_DEBUG")) {
        DlssVkOptimalSize recommended{};
        if (ngx_.optimalSize(ow, oh, selectedMode(), recommended)) {
            std::fprintf(stderr,
                "[real DLSS] mode=%s; render input=%dx%d, SDK optimal=%ux%u "
                "(range %ux%u - %ux%u) for output=%dx%d\n",
                modeLabel(selectedMode()), w, h,
                recommended.width, recommended.height,
                recommended.minWidth, recommended.minHeight,
                recommended.maxWidth, recommended.maxHeight, ow, oh);
        }
    }
    if (!images_.create(physical_, device_, w, h, ow, oh, true) ||
        !glColor_.import(images_, images_.color(), SDL_GL_GetProcAddress) ||
        !glOutput_.import(images_, images_.output(), SDL_GL_GetProcAddress) ||
        !sync_.initialize(device_, SDL_GL_GetProcAddress)) {
        fail("external memory or semaphore setup failed: " +
             images_.lastError() + " " + glColor_.lastError() + " " +
             glOutput_.lastError() + " " + sync_.lastError());
        return false;
    }
    if (opticalEnabled_ && std::getenv("XYO_DLSS_REAL_DEBUG"))
        std::fprintf(stderr,
            "[real DLSS] VK_NV_optical_flow active: optical queue family %u, "
            "DLSS compute queue %u; temporal history %s\n",
            opticalFamily_, queueFamily_,
            (std::getenv("XYO_DLSS_TEMPORAL") &&
             std::getenv("XYO_DLSS_TEMPORAL")[0] == '1')
                ? "experimental ON" : "OFF (safe default)");
    if (opticalEnabled_ &&
        (!optical_.create(physical_, device_, queueFamily_, opticalFamily_,
                          w, h) ||
         !motionConverter_.create(physical_, device_,
                                   optical_.packedBuffer(), w, h,
                                   optical_.gridSize(),
                                   optical_.costBuffer(),
                                   [] {
                                       const char* rigid =
                                           std::getenv("XYO_DLSS_RIGID_MOTION");
                                       const char* affine =
                                           std::getenv("XYO_DLSS_AFFINE_MOTION");
                                       return (rigid && rigid[0] == '1') ||
                                              (affine && affine[0] == '1');
                                   }()))) {
        fail("OpticalFlow/NGX RG16F motion allocation failed: " +
             optical_.lastError() + " " + motionConverter_.lastError());
        return false;
    }
    const char* biasEnv = std::getenv("XYO_DLSS_ALPHA_BIAS");
    if (biasEnv && biasEnv[0] == '1' &&
        !biasMask_.create(physical_, device_, uint32_t(w), uint32_t(h))) {
        fail("DLSS alpha bias allocation failed: " + biasMask_.lastError());
        return false;
    }
    display_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_TARGET, ow, oh);
    if (!display_) {
        fail(SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(display_, SDL_BLENDMODE_NONE);
    glGenFramebuffers(2, copyFbos_);
    if (!copyFbos_[0] || !copyFbos_[1]) {
        fail("could not allocate Vulkan/GL interop blit FBOs");
        return false;
    }
    inW_ = w; inH_ = h; outW_ = ow; outH_ = oh;
    return true;
}

SDL_Texture* DlssGlPipeline::apply(SDL_Renderer* renderer,
                                   SDL_Texture* input, int ow, int oh,
                                   bool forceHistoryReset,
                                   std::span<const DlssBiasRect> biasRects,
                                   std::span<const DlssRigidMotionRect> rigidRects,
                                   float jitterX, float jitterY,
                                   float opticalJitterDeltaX,
                                   float opticalJitterDeltaY) {
    if (!input || ow < 1 || oh < 1) return nullptr;
    int w = 0, h = 0;
    if (SDL_QueryTexture(input, nullptr, nullptr, &w, &h) ||
        !w || !h || ow < w || oh < h)
        return nullptr;

    // NGX 310.9.16 may retain Vulkan allocations across feature changes.
    // A display resize and any quality/preset transition must retire the
    // *entire* NGX/Vulkan context after all GPU work, not just its feature.
    // This is deliberately slower during resizing but avoids stale resource
    // references on drivers with strict external-memory ownership.
    const bool configurationChanged =
        supported_ && (activePreset_ != selectedPreset() ||
                       activeMode_ != selectedMode());
    const bool dimensionsChanged =
        supported_ && display_ &&
        (w != inW_ || h != inH_ || ow != outW_ || oh != outH_);
    if (configurationChanged || dimensionsChanged) {
        if (std::getenv("XYO_DLSS_REAL_DEBUG"))
            std::fprintf(stderr,
                "[real DLSS] %s; retiring Vulkan device/NGX context\n",
                configurationChanged ? "quality preset/mode changed" :
                                       "display size changed");
        reset();
    }
    if (!initialize(renderer) || !resize(renderer, w, h, ow, oh))
        return nullptr;
    using Clock = std::chrono::steady_clock;
    const bool profile = std::getenv("XYO_DLSS_REAL_DEBUG") != nullptr;
    const auto start = profile ? Clock::now() : Clock::time_point{};
    const char* cutMode = std::getenv("XYO_DLSS_SCENE_CUT");
    const bool confidenceEnabled = opticalEnabled_ &&
        optical_.costAvailable() && (!cutMode || cutMode[0] != '0');
    const char* syncEnv = std::getenv("XYO_DLSS_SCENE_CUT_SYNC");
    const bool syncCost = confidenceEnabled && syncEnv && syncEnv[0] == '1';
    auto assessCost = [&](const char* timing) {
        DlssVkOpticalFlow::CostStats stats{};
        if (!optical_.readCost(stats, 8)) return;
        DlssOpticalCostThreshold threshold{};
        if (const char* raw = std::getenv("XYO_DLSS_COST_MEAN")) {
            const float value = std::strtof(raw, nullptr);
            if (std::isfinite(value) && value > 0 && value <= 255)
                threshold.mean = value;
        }
        if (const char* raw = std::getenv("XYO_DLSS_COST_FRACTION")) {
            const float value = std::strtof(raw, nullptr);
            if (std::isfinite(value) && value > 0 && value <= 1)
                threshold.highFraction = value;
        }
        resetForBadMotion_ = dlssMotionUnreliable(
            stats.mean, stats.highFraction, threshold);
        if (profile && (resetForBadMotion_ || frameNumber_ % 120 == 0))
            std::fprintf(stderr,
                "[real DLSS] optical confidence %s: cost mean %.3f, "
                "cost>=8 fraction %.3f, temporal %s\n",
                timing, stats.mean, stats.highFraction,
                resetForBadMotion_ ? "RESET" : "retain");
    };
    if (submitted_) {
        const VkResult waited =
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (waited != VK_SUCCESS) {
            fail("Vulkan command buffer fence wait failed (VkResult " +
                 std::to_string(static_cast<int>(waited)) + ")");
            return nullptr;
        }
        if (gpuTimerPool_) {
            uint64_t stamps[2]{};
            if (vkGetQueryPoolResults(device_, gpuTimerPool_, 0, 2,
                    sizeof(stamps), stamps, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                stamps[1] >= stamps[0]) {
                gpuTotalMs_ += double(stamps[1] - stamps[0]) *
                               gpuTimestampNanoseconds_ / 1e6;
                ++gpuSamples_;
            }
        }
        if (confidenceEnabled && !syncCost && frameNumber_ > 1)
            assessCost("previous frame");
        if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) {
            fail("Vulkan fence reset failed");
            return nullptr;
        }
    }
    const auto afterFence = profile ? Clock::now() : Clock::time_point{};
    SDL_RenderFlush(renderer);
    GLuint srcId = textureId(input), dstId = textureId(display_);
    if (!srcId || !dstId ||
        !blit(copyFbos_[0], copyFbos_[1],
              srcId, glColor_.name(), w, h, w, h)) {
        fail("SDL stage -> Vulkan shared input GL blit failed");
        return nullptr;
    }
    const auto afterInput = profile ? Clock::now() : Clock::time_point{};
    const bool first = !submitted_;
    const std::array<GLuint, 2> textures{glColor_.name(), glOutput_.name()};
    const std::array<GLenum, 2> layouts{
        GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_SHADER_READ_ONLY_EXT};
    if ((first &&
         !sync_.signalGlToVk(glColor_.name(), GL_LAYOUT_GENERAL_EXT)) ||
        (!first && !sync_.signalGlToVk(textures, layouts))) {
        fail("GL -> Vulkan semaphore signal failed");
        return nullptr;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const bool haveFlow = opticalEnabled_ && frameNumber_ > 0;
    if (opticalEnabled_) {
        // Compute/transfer queue: acquire current SDL GL image, convert the
        // color attachment to the BGRA OpticalFlow input. The GL semaphore
        // wait is attached to this FIRST Vulkan submission, not DLSS.
        if (vkResetCommandBuffer(prepCmd_, 0) != VK_SUCCESS ||
            vkBeginCommandBuffer(prepCmd_, &bi) != VK_SUCCESS ||
            !images_.acquireGlColorForTransfer(prepCmd_, queueFamily_)) {
            fail("OpticalFlow preparation command/acquire failed");
            return nullptr;
        }
        optical_.recordInput(prepCmd_, images_.color().image, frameNumber_);
        if (!images_.finishGlColorTransfer(prepCmd_) ||
            vkEndCommandBuffer(prepCmd_) != VK_SUCCESS ||
            submitBinary(queue_, prepCmd_, sync_.glToVkSemaphore(),
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         prepDone_) != VK_SUCCESS) {
            fail("OpticalFlow GPU input copy submission failed");
            return nullptr;
        }
        if (haveFlow) {
            // Dedicated OpticalFlow queue (RTX3070: family 5).
            VkCommandBuffer opticalCmd = optical_.opticalCommandBuffer();
            if (vkResetCommandBuffer(opticalCmd, 0) != VK_SUCCESS ||
                vkBeginCommandBuffer(opticalCmd, &bi) != VK_SUCCESS) {
                fail("OpticalFlow command buffer begin/reset failed");
                return nullptr;
            }
            if (!optical_.recordFlow(opticalCmd, frameNumber_)) {
                fail("OpticalFlow image binding failed: " +
                     optical_.lastError());
                return nullptr;
            }
            if (vkEndCommandBuffer(opticalCmd) != VK_SUCCESS ||
                submitBinary(opticalQueue_, opticalCmd, prepDone_,
                             VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV |
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             flowDone_, syncCost ? opticalCostFence_ :
                                                   VK_NULL_HANDLE) != VK_SUCCESS) {
                fail("OpticalFlow GPU queue submission failed");
                return nullptr;
            }
            if (syncCost) {
                if (vkWaitForFences(device_, 1, &opticalCostFence_,
                                    VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
                    vkResetFences(device_, 1, &opticalCostFence_) != VK_SUCCESS) {
                    fail("OpticalFlow immediate confidence fence failed");
                    return nullptr;
                }
                assessCost("current frame");
            }
        }
    }
    if (vkResetCommandBuffer(cmd_, 0) != VK_SUCCESS) {
        fail("vkResetCommandBuffer failed");
        return nullptr;
    }
    if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS ||
        (!opticalEnabled_ &&
         !images_.acquireGlColor(cmd_, queueFamily_)) ||
        (!first && !images_.acquireGlOutput(cmd_, queueFamily_)) ||
        !images_.prepareDepthMotionUpload(cmd_)) {
        fail("Vulkan command buffer/image acquisition failed");
        return nullptr;
    }
    if (gpuTimerPool_) {
        vkCmdResetQueryPool(cmd_, gpuTimerPool_, 0, 2);
    }
    // Scratch pen layers have no geometric depth/motion vectors. To avoid
    // temporal ghosting we reset DLSS history every frame. This is native NGX,
    // but not equivalent to DLSS integration in a conventional 3D renderer.
    VkClearColorValue zeros{};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkClearDepthStencilValue depth{1, 0};
    if (!haveFlow)
        vkCmdClearColorImage(cmd_, images_.motion().image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zeros, 1, &cr);
    vkCmdClearDepthStencilImage(cmd_, images_.depth().image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &dr);
    if (!images_.prepareEvaluate(cmd_) ||
        !ngx_.configure(cmd_, w, h, ow, oh,
                        selectedPreset(), selectedMode())) {
        fail("DLSS feature creation failed: " + ngx_.lastError());
        return nullptr;
    }
    if (haveFlow) {
        // The optical queue has copied SFIXED5 vectors into a shared buffer.
        // A GPU-only compute shader expands them to RG16F motion per pixel.
        const char* clip = std::getenv("XYO_DLSS_MOTION_COST_CLIP");
        uint32_t costLimit = 0;
        if (clip && clip[0] != '0' && optical_.costAvailable()) {
            costLimit = 8;
            if (const char* limit = std::getenv("XYO_DLSS_MOTION_COST_LIMIT")) {
                const long value = std::strtol(limit, nullptr, 10);
                if (value >= 1 && value <= 255)
                    costLimit = static_cast<uint32_t>(value);
            }
        }
        motionConverter_.record(cmd_, costLimit, rigidRects,
                                opticalJitterDeltaX, opticalJitterDeltaY);
        if (!rigidRects.empty() && std::getenv("XYO_DLSS_REAL_DEBUG")) {
            static bool loggedRigid = false;
            if (!loggedRigid) {
                loggedRigid = true;
                if (rigidRects[0].affine > 0.5f) {
                    const auto& r = rigidRects[0];
                    std::fprintf(stderr,
                        "[real DLSS] affine motion override active: %zu "
                        "opaque quads; current->previous rows "
                        "[(%.4f, %.4f, %.4f), (%.4f, %.4f, %.4f)]\n",
                        rigidRects.size(),
                        r.previousX[0], r.previousX[1], r.previousX[2],
                        r.previousY[0], r.previousY[1], r.previousY[2]);
                } else {
                    std::fprintf(stderr,
                        "[real DLSS] rigid motion override active: %zu opaque "
                        "rectangles, first current->previous=(%.3f, %.3f)\n",
                        rigidRects.size(), rigidRects[0].deltaX,
                        rigidRects[0].deltaY);
                }
            }
        }
    }
    // A zero-region frame supplies no mask to NGX and needs no compute pass.
    // Old image contents are irrelevant until a later nonempty frame rewrites it.
    if (biasMask_.ready() && !biasRects.empty()) {
        if (profile) {
            static bool loggedActiveMask = false;
            if (!loggedActiveMask) {
                loggedActiveMask = true;
                std::fprintf(stderr,
                    "[real DLSS] GPU current-color bias mask active "
                    "(R8_UNORM, %zu rectangles; opt-in)\n",
                    biasRects.size());
            }
        }
        float strength = 1.0f;
        if (const char* raw = std::getenv("XYO_DLSS_ALPHA_BIAS_STRENGTH")) {
            const float parsed = std::strtof(raw, nullptr);
            if (std::isfinite(parsed) && parsed >= 0.0f && parsed <= 1.0f)
                strength = parsed;
        }
        if (!biasMask_.record(cmd_, biasRects, strength)) {
            fail("Vulkan current-color bias mask recording failed");
            return nullptr;
        }
    }
    DlssVkFrame frame{};
    frame.color = ngxImage(images_.color());
    frame.depth = ngxImage(images_.depth());
    frame.motionVectors = haveFlow
        ? DlssVkImage{motionConverter_.image(), motionConverter_.view(),
                      motionConverter_.format(),
                      motionConverter_.width(), motionConverter_.height(),
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}
        : ngxImage(images_.motion());
    frame.output = ngxImage(images_.output());
    frame.jitterX = jitterX;
    frame.jitterY = jitterY;
    if (biasMask_.ready() && !biasRects.empty()) {
        frame.currentColorBias = DlssVkImage{
            biasMask_.image(), biasMask_.view(), biasMask_.format(),
            biasMask_.width(), biasMask_.height(),
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    // Optical flow alone is not geometric motion: temporal reuse stays
    // experimental and only starts after the second valid frame.
    const char* temporalEnv = std::getenv("XYO_DLSS_TEMPORAL");
    const bool temporal = temporalEnv && temporalEnv[0] == '1';
    frame.resetHistory = !(haveFlow && temporal) || forceHistoryReset ||
                         (confidenceEnabled && resetForBadMotion_);
    if (gpuTimerPool_)
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            gpuTimerPool_, 0);
    const bool evaluated = ngx_.evaluate(cmd_, frame);
    if (evaluated && gpuTimerPool_)
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            gpuTimerPool_, 1);
    if (!evaluated || !images_.releaseToGl(cmd_, queueFamily_) ||
        vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
        fail("NGX DLSS evaluation or GL image release failed: " +
             ngx_.lastError() + " " + images_.lastError());
        return nullptr;
    }
    if (opticalEnabled_) {
        // Vulkan final compute waits on the optical engine GPU semaphore
        // (or input-copy semaphore for the first frame).
        const VkSemaphore wait = haveFlow ? flowDone_ : prepDone_;
        if (submitBinary(queue_, cmd_, wait,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         sync_.vkToGlSemaphore(), fence_) != VK_SUCCESS) {
            fail("vkQueueSubmit2 NVIDIA NGX + OpticalFlow failed");
            return nullptr;
        }
    } else {
        VkSemaphore waitSem = sync_.glToVkSemaphore();
        VkSemaphore signalSem = sync_.vkToGlSemaphore();
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &waitSem;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signalSem;
        if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) {
            fail("vkQueueSubmit DLSS failed");
            return nullptr;
        }
    }
    submitted_ = true;
    const auto afterSubmit = profile ? Clock::now() : Clock::time_point{};
    if (!sync_.waitVkToGl(textures, layouts) ||
        !blit(copyFbos_[0], copyFbos_[1],
              glOutput_.name(), dstId, ow, oh, ow, oh)) {
        fail("DLSS output Vulkan -> SDL GL blit failed");
        return nullptr;
    }
    ++frameNumber_;
    if (profile) {
        const auto done = Clock::now();
        static unsigned frames = 0;
        static double totals[4]{};
        const auto ms = [](Clock::duration elapsed) {
            return std::chrono::duration<double, std::milli>(elapsed).count();
        };
        totals[0] += ms(afterFence - start);
        totals[1] += ms(afterInput - afterFence);
        totals[2] += ms(afterSubmit - afterInput);
        totals[3] += ms(done - afterSubmit);
        ++frames;
        if (frames == 1 || frames % 120 == 0) {
            const double divisor = frames == 1 ? 1.0 : 120.0;
            std::fprintf(stderr,
                "[real DLSS] frame %u: %dx%d -> %dx%d; CPU avg ms: "
                "fence %.3f, GL->Vk copy %.3f, NGX record+submit %.3f, "
                "Vk->GL copy %.3f (GPU commands asynchronous)\n",
                frames, w, h, ow, oh,
                totals[0] / divisor, totals[1] / divisor,
                totals[2] / divisor, totals[3] / divisor);
            if (gpuTimerPool_ && gpuSamples_) {
                std::fprintf(stderr,
                    "[real DLSS] GPU NGX dispatch avg %.3f ms "
                    "across %llu completed frames\n",
                    gpuTotalMs_ / double(gpuSamples_),
                    static_cast<unsigned long long>(gpuSamples_));
                gpuTotalMs_ = 0.0;
                gpuSamples_ = 0;
            }
            for (double& t : totals) t = 0.0;
        }
    }
    return display_;
}

void DlssGlPipeline::reset() {
    if (renderer_) SDL_RenderFlush(renderer_);
    if (SDL_GL_GetCurrentContext()) glFinish();
    if (device_) vkDeviceWaitIdle(device_);
    ngx_.releaseFeature();
    discardImages();
    ngx_.shutdown();
    if (device_) {
        if (flowDone_) vkDestroySemaphore(device_, flowDone_, nullptr);
        if (prepDone_) vkDestroySemaphore(device_, prepDone_, nullptr);
        if (opticalCostFence_)
            vkDestroyFence(device_, opticalCostFence_, nullptr);
        if (gpuTimerPool_)
            vkDestroyQueryPool(device_, gpuTimerPool_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    opticalQueue_ = VK_NULL_HANDLE;
    opticalFamily_ = UINT32_MAX;
    opticalEnabled_ = false;
    prepCmd_ = VK_NULL_HANDLE;
    prepDone_ = flowDone_ = VK_NULL_HANDLE;
    opticalCostFence_ = VK_NULL_HANDLE;
    resetForBadMotion_ = false;
    frameNumber_ = 0;
    pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    gpuTimerPool_ = VK_NULL_HANDLE;
    gpuTimestampNanoseconds_ = gpuTotalMs_ = 0.0;
    gpuSamples_ = 0;
    renderer_ = nullptr;
    activePreset_ = DlssVkPreset::K;
    activeMode_ = DlssVkMode::Quality;
    tried_ = supported_ = submitted_ = false;
}

} // namespace xyo::render
#endif
