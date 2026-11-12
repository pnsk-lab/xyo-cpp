// Optional GPU-rendered current-color bias mask for native NGX DLSS.
// The mask is heuristic and does not provide pixel-perfect disocclusion.
#pragma once
#if defined(XYO_NVSDK_DLSS_VK)
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <string>

namespace xyo::render {
struct DlssBiasRect { int32_t left, top, right, bottom; };

class DlssVkBiasMask {
public:
    DlssVkBiasMask() = default;
    ~DlssVkBiasMask();
    DlssVkBiasMask(const DlssVkBiasMask&) = delete;
    DlssVkBiasMask& operator=(const DlssVkBiasMask&) = delete;

    bool create(VkPhysicalDevice physical, VkDevice device,
                uint32_t width, uint32_t height);
    // Must be recorded after GPU work on previous frame has completed.
    // This uploads only rectangles, not a CPU pixel mask.
    bool record(VkCommandBuffer cmd, std::span<const DlssBiasRect> rects,
                float strength);
    void destroy();
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return VK_FORMAT_R8_UNORM; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string& lastError() const { return error_; }
    static constexpr uint32_t kMaxRects = 64;
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer rectBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory rectMemory_ = VK_NULL_HANDLE;
    void* mappedRects_ = nullptr;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkImageLayout imageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width_ = 0, height_ = 0;
    std::string error_;
};
}
#endif
