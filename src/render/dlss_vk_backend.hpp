// Optional NVIDIA NGX DLSS Super Resolution bridge for a Vulkan renderer.
// This is the real SDK path (not the existing GL "DLSS-equivalent" filter).
// It deliberately does not own VkInstance/VkDevice or perform GL/Vk transfers.
#pragma once

#if defined(XYO_NVSDK_DLSS_VK)

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xyo::render {

struct DlssVkImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
    };
};

struct DlssVkFrame {
    DlssVkImage color;          // pre-exposed low-resolution input
    DlssVkImage depth;          // valid scene depth, not a dummy texture
    DlssVkImage motionVectors;  // pixel-space per-frame motion vectors
    DlssVkImage output;         // destination, full display resolution
    // Optional current-color bias, R8_UNORM, input resolution.
    // The NGX SDK does not guarantee equivalence to a history-reset mask.
    DlssVkImage currentColorBias;
    float jitterX = 0.0f;       // low-resolution pixel space
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;  // scale vectors to pixel space
    float motionScaleY = 1.0f;
    bool resetHistory = false;  // after scene cut, resize or camera teleport
};

// Quality-mode DLSS Super Resolution model-preset hint.
// Auto delegates model choice to the NVIDIA SDK/driver.
enum class DlssVkPreset { Auto, J, K, M };
enum class DlssVkMode { Quality, Balanced, Performance };

struct DlssVkOptimalSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t minWidth = 0;
    std::uint32_t minHeight = 0;
    std::uint32_t maxWidth = 0;
    std::uint32_t maxHeight = 0;
};

// Requires an initialized VkDevice with NVIDIA NGX's required Vulkan
// instance/device extensions. Vulkan images/layout transitions and command
// buffer submission remain the caller's responsibility.
class DlssVkBackend {
public:
    DlssVkBackend();
    ~DlssVkBackend();
    DlssVkBackend(const DlssVkBackend&) = delete;
    DlssVkBackend& operator=(const DlssVkBackend&) = delete;

    // A unique project GUID and a writable application data directory are
    // required by NGX. Uses the original NVIDIA SDK as provided by the user.
    // Call before creating VkInstance/VkDevice and append these required
    // extensions to each Vulkan create-info. Neither function creates a
    // context or a DLSS feature.
    static bool requiredInstanceExtensions(const std::string& projectId,
                                           const std::wstring& dataDirectory,
                                           std::vector<std::string>& extensions);
    static bool requiredDeviceExtensions(VkInstance instance,
                                         VkPhysicalDevice physicalDevice,
                                         const std::string& projectId,
                                         const std::wstring& dataDirectory,
                                         std::vector<std::string>& extensions);

    bool initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                    VkDevice device, const std::string& projectId,
                    const std::wstring& dataDirectory,
                    PFN_vkGetInstanceProcAddr getInstanceProc = nullptr,
                    PFN_vkGetDeviceProcAddr getDeviceProc = nullptr,
                    const std::wstring& featureDirectory = L"");

    // Call inside a recording Vulkan command buffer, then submit it before
    // trying to use the DLSS result. Recreates the NGX feature on resize.
    bool configure(VkCommandBuffer commandBuffer,
                   std::uint32_t renderWidth, std::uint32_t renderHeight,
                   std::uint32_t outputWidth, std::uint32_t outputHeight,
                   DlssVkPreset preset = DlssVkPreset::K,
                   DlssVkMode mode = DlssVkMode::Quality);

    // SDK-recommended input dimensions for the specified output and quality.
    // Informational only: the current stage renderScale remains user-selected.
    bool optimalSize(std::uint32_t outputWidth, std::uint32_t outputHeight,
                     DlssVkMode mode, DlssVkOptimalSize& result);

    // Append DLSS Super Resolution work to an open command buffer.
    // Does not fabricate unavailable depth/motion vectors.
    bool evaluate(VkCommandBuffer commandBuffer, const DlssVkFrame& frame);

    // Release any old DLSS feature before destroying its input/output Vulkan
    // images. Caller must guarantee completion of all GPU submissions.
    void releaseFeature();
    void shutdown();
    bool ready() const;
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xyo::render
#endif
