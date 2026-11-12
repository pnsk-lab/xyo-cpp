// Vulkan compute conversion of NV optical-flow SFIXED5 packed RG16 vectors
// to full-resolution RG16_SFLOAT motion vectors for NVIDIA NGX DLSS.
// No proprietary SDK dependency; available only in NVIDIA Vulkan builds.
#pragma once
#if defined(XYO_NVSDK_DLSS_VK)
#include <vulkan/vulkan.h>
#include "dlss_rigid_motion.hpp"
#include <span>
#include <cstdint>
#include <string>

namespace xyo::render {

class DlssVkMotionConverter {
public:
    DlssVkMotionConverter() = default;
    ~DlssVkMotionConverter();
    DlssVkMotionConverter(const DlssVkMotionConverter&) = delete;
    DlssVkMotionConverter& operator=(const DlssVkMotionConverter&) = delete;

    // packedVectors is caller-owned VkBuffer with STORAGE_BUFFER usage,
    // containing one packed signed 16-bit X/Y vector per grid cell.
    // The buffer must remain valid while this converter exists.
    bool create(VkPhysicalDevice physical, VkDevice device,
                VkBuffer packedVectors, uint32_t width, uint32_t height,
                uint32_t gridSize = 4, VkBuffer packedCosts = VK_NULL_HANDLE,
                bool enableRigid = false);
    // Record GPU work into a compute-capable queue command buffer.
    // Input must be synchronized from the preceding optical-flow transfer.
    // Output ends in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for NGX.
    void record(VkCommandBuffer commandBuffer, uint32_t costLimit = 0,
                std::span<const DlssRigidMotionRect> rigid = {},
                float opticalJitterDeltaX = 0,
                float opticalJitterDeltaY = 0);
    void destroy();

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return VK_FORMAT_R16G16_SFLOAT; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool valid() const { return pipeline_ && image_; }
    const std::string& lastError() const { return error_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;
    uint32_t gridWidth_ = 0, gridHeight_ = 0, gridSize_ = 4;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool hasCosts_ = false;
    bool hasRigid_ = false;
    VkBuffer rigidBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory rigidMemory_ = VK_NULL_HANDLE;
    void* rigidMapped_ = nullptr;
    std::string error_;
};

} // namespace xyo::render
#endif
