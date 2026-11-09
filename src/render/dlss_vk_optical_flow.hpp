// VK_NV_optical_flow orchestration for two successive Vulkan frames.
// Owns dedicated queue command pool, input frames, SFIXED5 flow image,
// packed-vector transfer buffer and optical-flow session.
// Device/queues are owned by the outer DLSS pipeline, not this helper.
#pragma once
#if defined(XYO_NVSDK_DLSS_VK)
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <string>

namespace xyo::render {

class DlssVkOpticalFlow {
public:
    DlssVkOpticalFlow() = default;
    ~DlssVkOpticalFlow();
    DlssVkOpticalFlow(const DlssVkOpticalFlow&) = delete;
    DlssVkOpticalFlow& operator=(const DlssVkOpticalFlow&) = delete;

    bool create(VkPhysicalDevice physical, VkDevice device,
                uint32_t computeFamily, uint32_t opticalFamily,
                uint32_t width, uint32_t height);
    // Source is VK_FORMAT_R8G8B8A8_UNORM, in TRANSFER_SRC_OPTIMAL layout.
    // Executed on a compute/transfer-capable queue; semaphore-synchronize
    // this submission before using recordFlow on the optical-flow queue.
    void recordInput(VkCommandBuffer cmd, VkImage color, uint64_t frameIndex);
    // Executed on an optical-flow-capable queue after recordInput completes.
    // Computes motion of frameIndex -> (frameIndex-1); first frame has no flow.
    bool recordFlow(VkCommandBuffer cmd, uint64_t frameIndex);
    VkBuffer packedBuffer() const { return buffer_; }
    VkBuffer costBuffer() const { return costEnabled_ ? costBuffer_ : VK_NULL_HANDLE; }
    VkCommandBuffer opticalCommandBuffer() const { return opticalCmd_; }
    uint32_t gridSize() const { return gridSize_; }
    bool valid() const { return session_ && buffer_; }
    bool costAvailable() const { return costEnabled_; }
    struct CostStats {
        float mean = 0;
        float highFraction = 0;
        uint8_t max = 0;
    };
    // Call only after a Vulkan fence guarantees optical-flow command completion.
    // The cost buffer is small (one R8_UINT per 4x4 grid cell).
    // A high value means less reliable motion estimation.
    bool readCost(CostStats& stats, uint8_t highThreshold = 8) const;
    const std::string& lastError() const { return error_; }
    void destroy();

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    bool allocate(Image& dest, VkFormat format, VkOpticalFlowUsageFlagsNV usage,
                  VkImageUsageFlags imageUsage,
                  uint32_t width, uint32_t height,
                  bool shared);
    void releaseImage(Image& image);
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t computeFamily_ = 0, opticalFamily_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint32_t gridWidth_ = 0, gridHeight_ = 0, gridSize_ = 4;
    std::array<Image, 2> input_{};
    Image flow_{};
    Image cost_{};
    bool costEnabled_ = false;
    VkBuffer costBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory costMemory_ = VK_NULL_HANDLE;
    void* costMapped_ = nullptr;
    uint32_t costBytes_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory_ = VK_NULL_HANDLE;
    VkOpticalFlowSessionNV session_ = VK_NULL_HANDLE;
    VkCommandPool opticalPool_ = VK_NULL_HANDLE;
    VkCommandBuffer opticalCmd_ = VK_NULL_HANDLE;
    PFN_vkCreateOpticalFlowSessionNV createSession_ = nullptr;
    PFN_vkDestroyOpticalFlowSessionNV destroySession_ = nullptr;
    PFN_vkBindOpticalFlowSessionImageNV bind_ = nullptr;
    PFN_vkCmdOpticalFlowExecuteNV execute_ = nullptr;
    std::string error_;
};

} // namespace xyo::render
#endif
