// Image allocation and barrier tracking for a Vulkan DLSS Super Resolution
// rendering pipeline. Independent of NVIDIA's proprietary NGX libraries.
#pragma once

#if defined(XYO_NVSDK_DLSS_VK)
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace xyo::render {

struct DlssVkOwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0;
    bool exportable = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Owns GPU-local images for (color, depth, motion, DLSS output).
// Input images are transfer destinations; their contents must be supplied
// by the renderer. Missing motion/depth is NEVER synthesized as fake data.
class DlssVkFrameTargets {
public:
    DlssVkFrameTargets() = default;
    ~DlssVkFrameTargets();

    DlssVkFrameTargets(const DlssVkFrameTargets&) = delete;
    DlssVkFrameTargets& operator=(const DlssVkFrameTargets&) = delete;

    bool create(VkPhysicalDevice physicalDevice, VkDevice device,
                uint32_t renderWidth, uint32_t renderHeight,
                uint32_t outputWidth, uint32_t outputHeight,
                bool shareColorAndOutputWithGl = false);
    void destroy();

    // Returns an owned POSIX fd (caller must close or pass to GL import,
    // which consumes it) or -1 when the image is not exportable.
    // Requires VK_KHR_external_memory_fd enabled at device creation.
    int exportOpaqueFd(const DlssVkOwnedImage& image) const;

    // Must be called while recording an otherwise valid command buffer.
    // prepareUpload -> copy all three input buffers -> prepareEvaluate ->
    // NGX evaluate -> prepareReadback -> copy output -> submit.
    // Caller must ensure that previous submissions are complete before
    // reusing these images.
    bool prepareUpload(VkCommandBuffer commandBuffer);
    // GL has written color and signaled a GL/Vk external semaphore. Call
    // these on the Vulkan submission that WAITS on that semaphore. Only the
    // depth/motion images need upload layouts; GL color must be preserved.
    bool acquireGlColor(VkCommandBuffer commandBuffer, uint32_t queueFamily);
    bool acquireGlColorForTransfer(VkCommandBuffer commandBuffer,
                                   uint32_t queueFamily);
    bool finishGlColorTransfer(VkCommandBuffer commandBuffer);
    bool acquireGlOutput(VkCommandBuffer commandBuffer, uint32_t queueFamily);
    bool releaseToGl(VkCommandBuffer commandBuffer, uint32_t queueFamily);
    bool prepareDepthMotionUpload(VkCommandBuffer commandBuffer);
    bool prepareEvaluate(VkCommandBuffer commandBuffer);
    bool prepareReadback(VkCommandBuffer commandBuffer);

    const DlssVkOwnedImage& color() const { return color_; }
    const DlssVkOwnedImage& depth() const { return depth_; }
    const DlssVkOwnedImage& motion() const { return motion_; }
    const DlssVkOwnedImage& output() const { return output_; }

    bool valid() const { return device_ != VK_NULL_HANDLE; }
    const std::string& lastError() const { return error_; }

private:
    bool allocateImage(DlssVkOwnedImage& dest, VkFormat format,
                       VkImageAspectFlags aspect, VkImageUsageFlags usage,
                       uint32_t width, uint32_t height,
                       bool exportable);
    bool transition(VkCommandBuffer cmd, DlssVkOwnedImage& image,
                    VkImageLayout nextLayout,
                    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                    VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage);
    void freeImage(DlssVkOwnedImage& image);

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    DlssVkOwnedImage color_, depth_, motion_, output_;
    std::string error_;
};

}  // namespace xyo::render
#endif
