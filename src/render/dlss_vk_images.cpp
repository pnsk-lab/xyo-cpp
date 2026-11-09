#include "dlss_vk_images.hpp"
#if defined(XYO_NVSDK_DLSS_VK)
#include <climits>
#include <utility>

namespace xyo::render {
DlssVkFrameTargets::~DlssVkFrameTargets() { destroy(); }

bool DlssVkFrameTargets::allocateImage(
    DlssVkOwnedImage& image, VkFormat format, VkImageAspectFlags aspect,
    VkImageUsageFlags usage, uint32_t width, uint32_t height,
    bool exportable) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        needed |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        needed |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((props.optimalTilingFeatures & needed) != needed) {
        error_ = "Required Vulkan DLSS image format not supported";
        return false;
    }
    VkExternalMemoryImageCreateInfo externalImage{};
    externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (exportable) ci.pNext = &externalImage;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &ci, nullptr, &image.image) != VK_SUCCESS) {
        error_ = "vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image.image, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
    uint32_t type = UINT32_MAX;
    for (int pass = 0; pass < 2 && type == UINT32_MAX; ++pass) {
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i))) continue;
            if (!pass && !(mem.memoryTypes[i].propertyFlags &
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        error_ = "No compatible Vulkan device memory";
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image.image;
    VkExportMemoryAllocateInfo externalAlloc{};
    externalAlloc.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    externalAlloc.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    externalAlloc.pNext = &dedicated;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    if (exportable) alloc.pNext = &externalAlloc;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &image.memory) != VK_SUCCESS) {
        error_ = "vkAllocateMemory failed";
        return false;
    }
    if (vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS) {
        error_ = "vkBindImageMemory failed";
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) {
        error_ = "vkCreateImageView failed";
        return false;
    }
    image.format = format;
    image.allocationSize = req.size;
    image.exportable = exportable;
    image.aspect = aspect;
    image.width = width;
    image.height = height;
    return true;
}

bool DlssVkFrameTargets::create(VkPhysicalDevice physical, VkDevice device,
    uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH,
    bool shareColorAndOutputWithGl) {
    destroy();
    if (physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        !renderW || !renderH || outputW < renderW || outputH < renderH ||
        outputW > 16384 || outputH > 16384) {
        error_ = "Invalid DLSS frame dimensions or Vulkan device";
        return false;
    }
    physicalDevice_ = physical;
    device_ = device;
    constexpr VkImageUsageFlags input =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageUsageFlags output =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // No synthetic motion/depth! Actual image contents must be populated
    // by the scene renderer before calling prepareEvaluate().
    if (!allocateImage(color_, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_ASPECT_COLOR_BIT, input, renderW, renderH,
                       shareColorAndOutputWithGl) ||
        !allocateImage(depth_, VK_FORMAT_D32_SFLOAT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, input, renderW, renderH,
                       false) ||
        !allocateImage(motion_, VK_FORMAT_R16G16_SFLOAT,
                       VK_IMAGE_ASPECT_COLOR_BIT, input, renderW, renderH,
                       false) ||
        !allocateImage(output_, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_ASPECT_COLOR_BIT, output, outputW, outputH,
                       shareColorAndOutputWithGl)) {
        std::string reason = error_;
        destroy();
        error_ = std::move(reason);
        return false;
    }
    error_.clear();
    return true;
}

int DlssVkFrameTargets::exportOpaqueFd(
    const DlssVkOwnedImage& image) const {
    if (!device_ || !image.memory || !image.exportable) return -1;
    const auto fn = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (!fn) return -1;
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = image.memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    return fn(device_, &info, &fd) == VK_SUCCESS ? fd : -1;
}

void DlssVkFrameTargets::freeImage(DlssVkOwnedImage& image) {
    if (device_) {
        if (image.view) vkDestroyImageView(device_, image.view, nullptr);
        if (image.image) vkDestroyImage(device_, image.image, nullptr);
        if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
    }
    image = {};
}
void DlssVkFrameTargets::destroy() {
    // Must be called after previous submissions referencing these images
    // have completed (the owner calls vkDeviceWaitIdle or waits on fences).
    freeImage(output_);
    freeImage(motion_);
    freeImage(depth_);
    freeImage(color_);
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
}

bool DlssVkFrameTargets::transition(VkCommandBuffer cmd,
    DlssVkOwnedImage& image, VkImageLayout layout, VkAccessFlags srcAccess,
    VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage) {
    if (!device_ || !cmd || !image.image) {
        error_ = "Invalid Vulkan image transition";
        return false;
    }
    if (image.layout == layout) return true;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = image.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {image.aspect, 0, 1, 0, 1};
    b.srcAccessMask = b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    image.layout = layout;
    return true;
}

bool DlssVkFrameTargets::acquireGlColor(VkCommandBuffer cmd,
                                        uint32_t queueFamily) {
    if (!device_ || !cmd || !color_.image || !color_.exportable) {
        error_ = "GL color image not exported or Vulkan command buffer invalid";
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // GL_LAYOUT_GENERAL_EXT
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = queueFamily;
    barrier.image = color_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    color_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool DlssVkFrameTargets::acquireGlColorForTransfer(
    VkCommandBuffer cmd, uint32_t queueFamily) {
    if (!device_ || !cmd || !color_.image || !color_.exportable) {
        error_ = "GL color image unavailable for Optical Flow transfer";
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = queueFamily;
    barrier.image = color_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    color_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return true;
}

bool DlssVkFrameTargets::finishGlColorTransfer(VkCommandBuffer cmd) {
    if (!device_ || !cmd ||
        color_.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        error_ = "Optical Flow source transfer layout mismatch";
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    color_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool DlssVkFrameTargets::acquireGlOutput(VkCommandBuffer cmd,
                                         uint32_t queueFamily) {
    if (!device_ || !cmd || !output_.image || !output_.exportable ||
        output_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        error_ = "GL output texture has not been released for Vulkan reuse";
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = queueFamily;
    barrier.image = output_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    output_.layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

bool DlssVkFrameTargets::releaseToGl(VkCommandBuffer cmd,
                                     uint32_t queueFamily) {
    if (!device_ || !cmd || !color_.exportable || !output_.exportable ||
        color_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        output_.layout != VK_IMAGE_LAYOUT_GENERAL) {
        error_ = "Color/output image not in DLSS evaluation layouts";
        return false;
    }
    VkImageMemoryBarrier barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = queueFamily;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstAccessMask = 0;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].image = color_.image;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = output_.image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);
    color_.layout = VK_IMAGE_LAYOUT_GENERAL;
    output_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool DlssVkFrameTargets::prepareDepthMotionUpload(VkCommandBuffer cmd) {
    return transition(cmd, depth_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT) &&
           transition(cmd, motion_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
}

bool DlssVkFrameTargets::prepareUpload(VkCommandBuffer cmd) {
    return transition(cmd, color_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT) &&
           prepareDepthMotionUpload(cmd);
}

bool DlssVkFrameTargets::prepareEvaluate(VkCommandBuffer cmd) {
    return transition(cmd, color_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
           transition(cmd, depth_,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
           transition(cmd, motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
           transition(cmd, output_, VK_IMAGE_LAYOUT_GENERAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
bool DlssVkFrameTargets::prepareReadback(VkCommandBuffer cmd) {
    return transition(cmd, output_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
}
} // namespace xyo::render
#endif
