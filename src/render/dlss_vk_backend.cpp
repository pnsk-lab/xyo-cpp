#include "dlss_vk_backend.hpp"

#if defined(XYO_NVSDK_DLSS_VK)

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <utility>

namespace xyo::render {
namespace {

bool validProjectId(const std::string& id) {
    // The NVIDIA NGX project ID must be a GUID in canonical 8-4-4-4-12 form.
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' :
            !std::isxdigit(static_cast<unsigned char>(id[i])))
            return false;
    }
    return true;
}

std::string ngxError(const char* op, NVSDK_NGX_Result result) {
    char buf[180];
    std::snprintf(buf, sizeof(buf), "%s failed (NGX 0x%08x)",
                  op, static_cast<unsigned>(result));
    return buf;
}

bool valid(const DlssVkImage& image) {
    return image.image != VK_NULL_HANDLE &&
        image.view != VK_NULL_HANDLE &&
        image.format != VK_FORMAT_UNDEFINED &&
        image.width != 0 && image.height != 0 &&
        image.range.levelCount != 0 && image.range.layerCount != 0;
}

NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo(
    const std::string& projectId, const std::wstring& dataDirectory) {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType =
        NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = projectId.c_str();
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = "xyo-cpp";
    info.ApplicationDataPath = dataDirectory.c_str();
    return info;
}

NVSDK_NGX_Resource_VK ngxImage(const DlssVkImage& image, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        image.view, image.image, image.range, image.format,
        image.width, image.height, writable);
}

} // namespace

struct DlssVkBackend::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    std::uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
    bool initialized = false;
    bool resetNextFrame = true;
    DlssVkPreset activePreset = DlssVkPreset::Auto;
    DlssVkMode activeMode = DlssVkMode::Quality;
    std::string error;

    void releaseFeature() {
        if (feature) {
            NVSDK_NGX_VULKAN_ReleaseFeature(feature);
            feature = nullptr;
        }
        renderW = renderH = outputW = outputH = 0;
        resetNextFrame = true;
    }
};

bool DlssVkBackend::requiredInstanceExtensions(
    const std::string& projectId, const std::wstring& dataDirectory,
    std::vector<std::string>& extensions) {
    extensions.clear();
    if (!validProjectId(projectId) || dataDirectory.empty()) return false;
    const auto info = discoveryInfo(projectId, dataDirectory);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const NVSDK_NGX_Result result =
        NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &info, &count, &properties);
    if (NVSDK_NGX_FAILED(result) || (count && !properties)) return false;
    for (uint32_t i = 0; i < count; ++i)
        extensions.emplace_back(properties[i].extensionName);
    return true;
}

bool DlssVkBackend::requiredDeviceExtensions(
    VkInstance instance, VkPhysicalDevice physicalDevice,
    const std::string& projectId, const std::wstring& dataDirectory,
    std::vector<std::string>& extensions) {
    extensions.clear();
    if (!validProjectId(projectId) || dataDirectory.empty() ||
        instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return false;
    const auto info = discoveryInfo(projectId, dataDirectory);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const NVSDK_NGX_Result result =
        NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physicalDevice, &info, &count, &properties);
    if (NVSDK_NGX_FAILED(result) || (count && !properties)) return false;
    for (uint32_t i = 0; i < count; ++i)
        extensions.emplace_back(properties[i].extensionName);
    return true;
}

DlssVkBackend::DlssVkBackend() : impl_(std::make_unique<Impl>()) {}
DlssVkBackend::~DlssVkBackend() { shutdown(); }

bool DlssVkBackend::initialize(VkInstance instance,
                               VkPhysicalDevice physicalDevice,
                               VkDevice device,
                               const std::string& projectId,
                               const std::wstring& dataDirectory,
                               PFN_vkGetInstanceProcAddr getInstanceProc,
                               PFN_vkGetDeviceProcAddr getDeviceProc,
                               const std::wstring& featureDirectory) {
    shutdown();
    Impl& state = *impl_;
    if (!validProjectId(projectId) ||
        instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
        device == VK_NULL_HANDLE || dataDirectory.empty()) {
        state.error = "DLSS requires a valid project GUID, writable data "
                      "directory and initialized Vulkan instance/device";
        return false;
    }

    // Applications creating VkInstance/VkDevice must *first* enable NGX's
    // required extension list. This class cannot enable extensions on an
    // already-created Vulkan device.
    // NVIDIA's Linux DLSS .so may live outside the executable directory.
    // The SDK accepts an explicit list; never depend on global LD_LIBRARY_PATH.
    const wchar_t* paths[] = {featureDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo featureInfo{};
    featureInfo.PathListInfo.Path = featureDirectory.empty() ? nullptr : paths;
    featureInfo.PathListInfo.Length = featureDirectory.empty() ? 0u : 1u;
    const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        projectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, "xyo-cpp",
        dataDirectory.c_str(), instance, physicalDevice, device,
        getInstanceProc, getDeviceProc,
        featureDirectory.empty() ? nullptr : &featureInfo);
    if (NVSDK_NGX_FAILED(init)) {
        state.error = ngxError("NVSDK_NGX_VULKAN_Init_with_ProjectID", init);
        return false;
    }
    state.instance = instance;
    state.device = device;
    state.initialized = true;

    const NVSDK_NGX_Result params =
        NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.parameters);
    if (NVSDK_NGX_FAILED(params) || !state.parameters) {
        state.error = ngxError("NGX GetCapabilityParameters", params);
        shutdown();
        return false;
    }
    unsigned int available = 0;
    const NVSDK_NGX_Result capability = NVSDK_NGX_Parameter_GetUI(
        state.parameters, NVSDK_NGX_Parameter_SuperSampling_Available,
        &available);
    if (NVSDK_NGX_FAILED(capability) || !available) {
        state.error = "NGX reports DLSS Super Resolution unavailable on "
                      "this Vulkan device/driver";
        // Keep the SDK lifetime valid; user can inspect lastError().
        shutdown();
        return false;
    }
    state.error.clear();
    return true;
}

namespace {
NVSDK_NGX_PerfQuality_Value ngxMode(DlssVkMode mode) {
    switch (mode) {
        case DlssVkMode::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssVkMode::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        default:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

const char* presetParameter(DlssVkMode mode) {
    switch (mode) {
        case DlssVkMode::Performance:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        case DlssVkMode::Balanced:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        default:
            return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    }
}
} // namespace

bool DlssVkBackend::optimalSize(std::uint32_t outputW, std::uint32_t outputH,
                                DlssVkMode mode, DlssVkOptimalSize& result) {
    result = {};
    Impl& s = *impl_;
    if (!s.initialized || !s.parameters || !outputW || !outputH) return false;
    float sharpness = 0.0f;
    return NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(
        s.parameters, outputW, outputH, ngxMode(mode),
        &result.width, &result.height,
        &result.maxWidth, &result.maxHeight,
        &result.minWidth, &result.minHeight, &sharpness));
}

bool DlssVkBackend::configure(VkCommandBuffer cmd, std::uint32_t renderW,
                              std::uint32_t renderH, std::uint32_t outputW,
                              std::uint32_t outputH,
                              DlssVkPreset preset, DlssVkMode mode) {
    Impl& s = *impl_;
    if (!s.initialized || !s.parameters || cmd == VK_NULL_HANDLE ||
        renderW == 0 || renderH == 0 || outputW < renderW ||
        outputH < renderH) {
        s.error = "DLSS configure: invalid dimensions or Vulkan state";
        return false;
    }
    if (s.feature && s.renderW == renderW && s.renderH == renderH &&
        s.outputW == outputW && s.outputH == outputH &&
        s.activePreset == preset && s.activeMode == mode)
        return true;
    s.releaseFeature();

    NVSDK_NGX_DLSS_Create_Params config{};
    config.Feature.InWidth = renderW;
    config.Feature.InHeight = renderH;
    config.Feature.InTargetWidth = outputW;
    config.Feature.InTargetHeight = outputH;
    config.Feature.InPerfQualityValue = ngxMode(mode);
    config.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    // The preset is a creation-time hint. SDK 310.9.16 exposes the
    // transformer-based K preset for the Quality mode. A driver override or
    // future SDK may still choose a different effective model.
    if (preset != DlssVkPreset::Auto) {
        NVSDK_NGX_DLSS_Hint_Render_Preset hint =
            NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        switch (preset) {
            case DlssVkPreset::J:
                hint = NVSDK_NGX_DLSS_Hint_Render_Preset_J;
                break;
            case DlssVkPreset::M:
                hint = NVSDK_NGX_DLSS_Hint_Render_Preset_M;
                break;
            default:
                break;
        }
        NVSDK_NGX_Parameter_SetI(
            s.parameters, presetParameter(mode), static_cast<int>(hint));
    } else {
        NVSDK_NGX_Parameter_SetI(
            s.parameters, presetParameter(mode),
            static_cast<int>(NVSDK_NGX_DLSS_Hint_Render_Preset_Default));
    }
    // DLSS is evaluated on the caller's active Vulkan command buffer.
    const NVSDK_NGX_Result created = NGX_VULKAN_CREATE_DLSS_EXT1(
        s.device, cmd, 1u, 1u, &s.feature, s.parameters, &config);
    if (NVSDK_NGX_FAILED(created) || !s.feature) {
        s.error = ngxError("NGX Create DLSS", created);
        return false;
    }
    s.renderW = renderW;
    s.renderH = renderH;
    s.outputW = outputW;
    s.outputH = outputH;
    s.resetNextFrame = true;
    s.activePreset = preset;
    s.activeMode = mode;
    s.error.clear();
    return true;
}

bool DlssVkBackend::evaluate(VkCommandBuffer cmd, const DlssVkFrame& frame) {
    Impl& s = *impl_;
    if (!s.feature || cmd == VK_NULL_HANDLE ||
        !valid(frame.color) || !valid(frame.depth) ||
        !valid(frame.motionVectors) || !valid(frame.output) ||
        frame.color.width != s.renderW || frame.color.height != s.renderH ||
        frame.depth.width != s.renderW || frame.depth.height != s.renderH ||
        frame.motionVectors.width != s.renderW ||
        frame.motionVectors.height != s.renderH ||
        frame.output.width != s.outputW ||
        frame.output.height != s.outputH ||
        (frame.currentColorBias.image != VK_NULL_HANDLE &&
         (!valid(frame.currentColorBias) ||
          frame.currentColorBias.width != s.renderW ||
          frame.currentColorBias.height != s.renderH ||
          frame.currentColorBias.format != VK_FORMAT_R8_UNORM))) {
        s.error = "DLSS evaluate: source/depth/motion/output Vulkan images "
                  "are required with dimensions matching configured feature";
        return false;
    }

    auto color = ngxImage(frame.color, false);
    auto depth = ngxImage(frame.depth, false);
    auto motion = ngxImage(frame.motionVectors, false);
    auto output = ngxImage(frame.output, true);
    auto bias = frame.currentColorBias.image
        ? ngxImage(frame.currentColorBias, false) : NVSDK_NGX_Resource_VK{};
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.pInBiasCurrentColorMask =
        frame.currentColorBias.image ? &bias : nullptr;
    eval.InJitterOffsetX = frame.jitterX;
    eval.InJitterOffsetY = frame.jitterY;
    eval.InMVScaleX = frame.motionScaleX;
    eval.InMVScaleY = frame.motionScaleY;
    eval.InReset = (frame.resetHistory || s.resetNextFrame) ? 1 : 0;
    eval.InRenderSubrectDimensions.Width = s.renderW;
    eval.InRenderSubrectDimensions.Height = s.renderH;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(
        cmd, s.feature, s.parameters, &eval);
    if (NVSDK_NGX_FAILED(result)) {
        s.error = ngxError("NGX Evaluate DLSS", result);
        return false;
    }
    s.resetNextFrame = false;
    s.error.clear();
    return true;
}

void DlssVkBackend::releaseFeature() {
    if (impl_) impl_->releaseFeature();
}

void DlssVkBackend::shutdown() {
    if (!impl_) return;
    Impl& s = *impl_;
    releaseFeature();
    if (s.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(s.parameters);
        s.parameters = nullptr;
    }
    if (s.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(s.device);
        s.initialized = false;
    }
    s.instance = VK_NULL_HANDLE;
    s.device = VK_NULL_HANDLE;
}

bool DlssVkBackend::ready() const {
    return impl_->initialized && impl_->feature;
}
const std::string& DlssVkBackend::lastError() const {
    return impl_->error;
}

} // namespace xyo::render
#endif
