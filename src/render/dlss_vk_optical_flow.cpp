#include "dlss_vk_optical_flow.hpp"
#if defined(XYO_NVSDK_DLSS_VK)
#include <climits>
#include <utility>

namespace xyo::render {
namespace {
uint32_t deviceLocalMemory(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    return UINT32_MAX;
}
void imageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkImageLayout before, VkImageLayout after,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

DlssVkOpticalFlow::~DlssVkOpticalFlow() { destroy(); }

bool DlssVkOpticalFlow::allocate(
    Image& dest, VkFormat format, VkOpticalFlowUsageFlagsNV opticalUsage,
    VkImageUsageFlags imageUsage, uint32_t width, uint32_t height,
    bool shared) {
    VkOpticalFlowImageFormatInfoNV optInfo{};
    optInfo.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
    optInfo.usage = opticalUsage;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &optInfo;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = imageUsage;
    uint32_t families[] = {computeFamily_, opticalFamily_};
    if (shared && computeFamily_ != opticalFamily_) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    }
    if (vkCreateImage(device_, &ci, nullptr, &dest.image) != VK_SUCCESS) {
        error_ = "vkCreateImage failed for optical flow";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, dest.image, &req);
    const uint32_t type = deviceLocalMemory(physical_, req.memoryTypeBits);
    if (type == UINT32_MAX) {
        error_ = "optical flow image lacks device-local memory";
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &dest.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, dest.image, dest.memory, 0) != VK_SUCCESS) {
        error_ = "optical flow image memory bind failed";
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = dest.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &dest.view) != VK_SUCCESS) {
        error_ = "optical flow image view creation failed";
        return false;
    }
    return true;
}

bool DlssVkOpticalFlow::create(VkPhysicalDevice physical, VkDevice device,
    uint32_t computeFamily, uint32_t opticalFamily,
    uint32_t width, uint32_t height) {
    destroy();
    if (!physical || !device || width < 32 || height < 32 ||
        width > 8192 || height > 8192) {
        error_ = "OpticalFlow invalid device or frame dimensions";
        return false;
    }
    physical_ = physical;
    device_ = device;
    computeFamily_ = computeFamily;
    opticalFamily_ = opticalFamily;
    width_ = width;
    height_ = height;
    gridWidth_ = (width + gridSize_ - 1) / gridSize_;
    gridHeight_ = (height + gridSize_ - 1) / gridSize_;
    createSession_ = reinterpret_cast<PFN_vkCreateOpticalFlowSessionNV>(
        vkGetDeviceProcAddr(device_, "vkCreateOpticalFlowSessionNV"));
    destroySession_ = reinterpret_cast<PFN_vkDestroyOpticalFlowSessionNV>(
        vkGetDeviceProcAddr(device_, "vkDestroyOpticalFlowSessionNV"));
    bind_ = reinterpret_cast<PFN_vkBindOpticalFlowSessionImageNV>(
        vkGetDeviceProcAddr(device_, "vkBindOpticalFlowSessionImageNV"));
    execute_ = reinterpret_cast<PFN_vkCmdOpticalFlowExecuteNV>(
        vkGetDeviceProcAddr(device_, "vkCmdOpticalFlowExecuteNV"));
    if (!createSession_ || !destroySession_ || !bind_ || !execute_) {
        error_ = "VK_NV_optical_flow function pointers not available";
        return false;
    }

    VkPhysicalDeviceOpticalFlowPropertiesNV flowProperties{};
    flowProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &flowProperties;
    vkGetPhysicalDeviceProperties2(physical_, &props);
    if (flowProperties.costSupported) {
        VkOpticalFlowImageFormatInfoNV usage{};
        usage.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
        usage.usage = VK_OPTICAL_FLOW_USAGE_COST_BIT_NV;
        VkPhysicalDeviceImageFormatInfo2 query{};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        query.pNext = &usage;
        query.format = VK_FORMAT_R8_UINT;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageFormatProperties2 supported{};
        supported.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        costEnabled_ =
            vkGetPhysicalDeviceImageFormatProperties2(
                physical_, &query, &supported) == VK_SUCCESS;
    }

    VkOpticalFlowSessionCreateInfoNV sessionInfo{};
    sessionInfo.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV;
    sessionInfo.width = width;
    sessionInfo.height = height;
    sessionInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sessionInfo.flowVectorFormat = VK_FORMAT_R16G16_SFIXED5_NV;
    if (costEnabled_) {
        sessionInfo.costFormat = VK_FORMAT_R8_UINT;
        sessionInfo.flags |= VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_COST_BIT_NV;
    }
    sessionInfo.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
    sessionInfo.performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV;
    if (createSession_(device_, &sessionInfo, nullptr, &session_) != VK_SUCCESS) {
        error_ = "vkCreateOpticalFlowSessionNV failed";
        return false;
    }
    const VkImageUsageFlags inputUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (auto& input : input_) {
        if (!allocate(input, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, inputUsage,
                      width, height, true))
            return false;
    }
    if (!allocate(flow_, VK_FORMAT_R16G16_SFIXED5_NV,
                  VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  gridWidth_, gridHeight_, false))
        return false;

    if (costEnabled_) {
        if (!allocate(cost_, VK_FORMAT_R8_UINT,
                      VK_OPTICAL_FLOW_USAGE_COST_BIT_NV,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      gridWidth_, gridHeight_, false)) return false;
        costBytes_ = gridWidth_ * gridHeight_;
        VkBufferCreateInfo costInfo{};
        costInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        costInfo.size = (costBytes_ + 3u) & ~VkDeviceSize(3u);
        costInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        uint32_t sharedFamilies[2] = {computeFamily_, opticalFamily_};
        if (computeFamily_ != opticalFamily_) {
            costInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
            costInfo.queueFamilyIndexCount = 2;
            costInfo.pQueueFamilyIndices = sharedFamilies;
        }
        if (vkCreateBuffer(device_, &costInfo, nullptr, &costBuffer_) !=
            VK_SUCCESS) {
            error_ = "OpticalFlow cost readback buffer allocation failed";
            return false;
        }
        VkMemoryRequirements costReq{};
        vkGetBufferMemoryRequirements(device_, costBuffer_, &costReq);
        VkPhysicalDeviceMemoryProperties memoryTypes{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memoryTypes);
        uint32_t costType = UINT32_MAX;
        for (uint32_t i = 0; i < memoryTypes.memoryTypeCount; ++i)
            if ((costReq.memoryTypeBits & (1u << i)) &&
                (memoryTypes.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                costType = i;
                break;
            }
        if (costType == UINT32_MAX) {
            error_ = "OpticalFlow cost buffer needs host-coherent memory";
            return false;
        }
        VkMemoryAllocateInfo costAlloc{};
        costAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        costAlloc.allocationSize = costReq.size;
        costAlloc.memoryTypeIndex = costType;
        if (vkAllocateMemory(device_, &costAlloc, nullptr,
                             &costMemory_) != VK_SUCCESS ||
            vkBindBufferMemory(device_, costBuffer_, costMemory_, 0) !=
                VK_SUCCESS ||
            vkMapMemory(device_, costMemory_, 0, VK_WHOLE_SIZE, 0,
                        &costMapped_) != VK_SUCCESS) {
            error_ = "OpticalFlow cost buffer memory bind/map failed";
            return false;
        }
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = VkDeviceSize(gridWidth_) * gridHeight_ * sizeof(uint32_t);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    uint32_t families[] = {computeFamily_, opticalFamily_};
    if (computeFamily_ != opticalFamily_) {
        bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bufferInfo.queueFamilyIndexCount = 2;
        bufferInfo.pQueueFamilyIndices = families;
    }
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        error_ = "optical flow vector buffer creation failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer_, &req);
    const uint32_t type = deviceLocalMemory(physical_, req.memoryTypeBits);
    if (type == UINT32_MAX) {
        error_ = "optical flow vector buffer lacks device memory";
        return false;
    }
    VkMemoryAllocateInfo memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory.allocationSize = req.size;
    memory.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &memory, nullptr,
                         &bufferMemory_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, buffer_, bufferMemory_, 0) != VK_SUCCESS) {
        error_ = "optical flow vector buffer memory bind failed";
        return false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = opticalFamily_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device_, &pci, nullptr, &opticalPool_) !=
        VK_SUCCESS) {
        error_ = "optical flow command pool creation failed";
        return false;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = opticalPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &opticalCmd_) != VK_SUCCESS) {
        error_ = "optical flow command buffer allocation failed";
        return false;
    }
    error_.clear();
    return true;
}

void DlssVkOpticalFlow::recordInput(VkCommandBuffer cmd, VkImage color,
                                     uint64_t frame) {
    if (!valid() || !cmd || !color) return;
    auto& dst = input_[frame % 2];
    imageBarrier(cmd, dst.image, dst.layout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, 0,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    dst.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(width_),
                            static_cast<int32_t>(height_), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vkCmdBlitImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    imageBarrier(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, 0);
    dst.layout = VK_IMAGE_LAYOUT_GENERAL;
}

bool DlssVkOpticalFlow::recordFlow(VkCommandBuffer cmd, uint64_t frame) {
    if (!valid() || !cmd || !frame) {
        error_ = "OpticalFlow invalid command or frame";
        return false;
    }
    for (auto& img : input_) {
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
            VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV);
    }
    imageBarrier(cmd, flow_.image, flow_.layout,
        VK_IMAGE_LAYOUT_GENERAL,
        flow_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_NONE
            : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        flow_.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0 : VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
        VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV);
    flow_.layout = VK_IMAGE_LAYOUT_GENERAL;
    const Image& current = input_[frame % 2];
    const Image& previous = input_[(frame - 1) % 2];
    if (costEnabled_) {
        imageBarrier(cmd, cost_.image, cost_.layout,
                     VK_IMAGE_LAYOUT_GENERAL,
                     cost_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                         ? VK_PIPELINE_STAGE_2_NONE
                         : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     cost_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                         ? 0 : VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                     VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV);
        cost_.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (bind_(device_, session_, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV,
              current.view, VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS ||
        bind_(device_, session_, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV,
              previous.view, VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS ||
        bind_(device_, session_,
              VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV,
              flow_.view, VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS ||
        (costEnabled_ &&
         bind_(device_, session_, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_COST_NV,
               cost_.view, VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS)) {
        error_ = "vkBindOpticalFlowSessionImageNV rejected input/output views";
        return false;
    }
    VkOpticalFlowExecuteInfoNV ei{};
    ei.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV;
    execute_(cmd, session_, &ei);
    imageBarrier(cmd, flow_.image, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
        VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT);
    flow_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {gridWidth_, gridHeight_, 1};
    vkCmdCopyImageToBuffer(cmd, flow_.image, flow_.layout,
                           buffer_, 1, &region);
    if (costEnabled_) {
        imageBarrier(cmd, cost_.image, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                     VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        cost_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy costCopy{};
        costCopy.imageSubresource =
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        costCopy.imageExtent = {gridWidth_, gridHeight_, 1};
        vkCmdCopyImageToBuffer(cmd, cost_.image, cost_.layout,
                               costBuffer_, 1, &costCopy);
        VkBufferMemoryBarrier2 hostBarrier{};
        hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hostBarrier.buffer = costBuffer_;
        hostBarrier.size = VK_WHOLE_SIZE;
        hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo hostDep{};
        hostDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        hostDep.bufferMemoryBarrierCount = 1;
        hostDep.pBufferMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(cmd, &hostDep);
    }
    VkBufferMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = 0;
    b.buffer = buffer_;
    b.size = VK_WHOLE_SIZE;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

bool DlssVkOpticalFlow::readCost(CostStats& stats,
                                 uint8_t highThreshold) const {
    stats = {};
    if (!costEnabled_ || !costMapped_ || !costBytes_) return false;
    const auto* values = static_cast<const uint8_t*>(costMapped_);
    uint64_t sum = 0;
    uint32_t high = 0;
    uint8_t maximum = 0;
    for (uint32_t i = 0; i < costBytes_; ++i) {
        const auto v = values[i];
        sum += v;
        high += v >= highThreshold;
        if (v > maximum) maximum = v;
    }
    stats.mean = float(sum) / float(costBytes_);
    stats.highFraction = float(high) / float(costBytes_);
    stats.max = maximum;
    return true;
}

void DlssVkOpticalFlow::releaseImage(Image& img) {
    if (device_) {
        if (img.view) vkDestroyImageView(device_, img.view, nullptr);
        if (img.image) vkDestroyImage(device_, img.image, nullptr);
        if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    }
    img = {};
}
void DlssVkOpticalFlow::destroy() {
    if (device_) {
        if (opticalPool_) vkDestroyCommandPool(device_, opticalPool_, nullptr);
        if (session_ && destroySession_)
            destroySession_(device_, session_, nullptr);
        releaseImage(flow_);
        releaseImage(cost_);
        for (auto& image : input_) releaseImage(image);
        if (costMapped_) vkUnmapMemory(device_, costMemory_);
        if (costBuffer_) vkDestroyBuffer(device_, costBuffer_, nullptr);
        if (costMemory_) vkFreeMemory(device_, costMemory_, nullptr);
        if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
        if (bufferMemory_) vkFreeMemory(device_, bufferMemory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    session_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    bufferMemory_ = VK_NULL_HANDLE;
    costEnabled_ = false;
    costBytes_ = 0;
    costBuffer_ = VK_NULL_HANDLE;
    costMemory_ = VK_NULL_HANDLE;
    costMapped_ = nullptr;
    opticalPool_ = VK_NULL_HANDLE;
    opticalCmd_ = VK_NULL_HANDLE;
    createSession_ = nullptr;
    destroySession_ = nullptr;
    bind_ = nullptr;
    execute_ = nullptr;
    width_ = height_ = gridWidth_ = gridHeight_ = 0;
}

} // namespace xyo::render
#endif
