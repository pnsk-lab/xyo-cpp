#include "dlss_vk_motion_converter.hpp"
#if defined(XYO_NVSDK_DLSS_VK)

#include "dlss_optical_vectors_embedded.hpp"
#include <array>
#include <algorithm>
#include <climits>
#include <utility>

namespace xyo::render {
namespace {
struct Push {
    int32_t outputWidth, outputHeight, gridWidth, gridHeight, gridSize;
    int32_t costLimit;
};
}
DlssVkMotionConverter::~DlssVkMotionConverter() { destroy(); }

bool DlssVkMotionConverter::create(
    VkPhysicalDevice physical, VkDevice device, VkBuffer packedVectors,
    uint32_t width, uint32_t height, uint32_t gridSize,
    VkBuffer packedCosts) {
    destroy();
    if (!physical || !device || !packedVectors || !width || !height ||
        !gridSize || width > 16384 || height > 16384) {
        error_ = "invalid device, buffer, dimensions or grid size";
        return false;
    }
    device_ = device;
    hasCosts_ = packedCosts != VK_NULL_HANDLE;
    width_ = width;
    height_ = height;
    gridSize_ = gridSize;
    gridWidth_ = (width + gridSize - 1) / gridSize;
    gridHeight_ = (height + gridSize - 1) / gridSize;

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format(), &props);
    const VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((props.optimalTilingFeatures & needed) != needed) {
        error_ = "VK_FORMAT_R16G16_SFLOAT lacks storage/sampled/transfer support";
        destroy();
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format();
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        error_ = "vkCreateImage for DLSS motion failed";
        destroy();
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image_, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        error_ = "no suitable GPU memory for DLSS motion output";
        destroy();
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
        error_ = "motion image memory allocation/bind failed";
        destroy();
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format();
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &view_) != VK_SUCCESS) {
        error_ = "motion output image view failed";
        destroy();
        return false;
    }

    constexpr VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 3;
    setInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
                                     &descriptorLayout_) != VK_SUCCESS) {
        error_ = "motion shader descriptor layout failed";
        destroy();
        return false;
    }
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    }};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &pool, nullptr,
                               &descriptorPool_) != VK_SUCCESS) {
        error_ = "motion shader descriptor pool failed";
        destroy();
        return false;
    }
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descriptorPool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &descriptorLayout_;
    if (vkAllocateDescriptorSets(device_, &setAlloc,
                                 &descriptorSet_) != VK_SUCCESS) {
        error_ = "motion descriptor allocation failed";
        destroy();
        return false;
    }
    VkDescriptorBufferInfo storageBuffer{};
    storageBuffer.buffer = packedVectors;
    storageBuffer.offset = 0;
    storageBuffer.range = VkDeviceSize(gridWidth_) * gridHeight_ * sizeof(uint32_t);
    VkDescriptorBufferInfo costBuffer{};
    costBuffer.buffer = hasCosts_ ? packedCosts : packedVectors;
    costBuffer.offset = 0;
    costBuffer.range = hasCosts_ ?
        VkDeviceSize((gridWidth_ * gridHeight_ + 3u) & ~3u) :
        VkDeviceSize(gridWidth_) * gridHeight_ * sizeof(uint32_t);
    VkDescriptorImageInfo storageImage{};
    storageImage.imageView = view_;
    storageImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[3]{};
    for (auto& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &storageBuffer;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &storageImage;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &costBuffer;
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(Push);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                                &pipelineLayout_) != VK_SUCCESS) {
        error_ = "motion pipeline layout failed";
        destroy();
        return false;
    }
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kDlssOpticalVectorsSpv);
    shaderInfo.pCode = kDlssOpticalVectorsSpv;
    if (vkCreateShaderModule(device_, &shaderInfo, nullptr,
                             &shader_) != VK_SUCCESS) {
        error_ = "embedded SPIR-V motion shader rejected";
        destroy();
        return false;
    }
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader_;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                 nullptr, &pipeline_) != VK_SUCCESS) {
        error_ = "motion conversion compute pipeline failed";
        destroy();
        return false;
    }
    error_.clear();
    return true;
}

void DlssVkMotionConverter::record(VkCommandBuffer cmd, uint32_t costLimit) {
    if (!valid() || !cmd) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = layout_;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    layout_ = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    const Push push{static_cast<int32_t>(width_),
                    static_cast<int32_t>(height_),
                    static_cast<int32_t>(gridWidth_),
                    static_cast<int32_t>(gridHeight_),
                    static_cast<int32_t>(gridSize_),
                    static_cast<int32_t>(hasCosts_ ? std::min(costLimit, 255u) : 0u)};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);
    VkImageMemoryBarrier read{};
    read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    read.image = image_;
    read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &read);
    layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void DlssVkMotionConverter::destroy() {
    if (device_) {
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (shader_) vkDestroyShaderModule(device_, shader_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        if (view_) vkDestroyImageView(device_, view_, nullptr);
        if (image_) vkDestroyImage(device_, image_, nullptr);
        if (memory_) vkFreeMemory(device_, memory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    width_ = height_ = gridWidth_ = gridHeight_ = 0;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    hasCosts_ = false;
}
} // namespace xyo::render
#endif
