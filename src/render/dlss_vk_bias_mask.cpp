#include "dlss_vk_bias_mask.hpp"
#if defined(XYO_NVSDK_DLSS_VK)
#include "dlss_bias_mask_embedded.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace xyo::render {
namespace {
struct Push { int32_t width, height, count; float strength; };
uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits,
                    VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return UINT32_MAX;
}
}

DlssVkBiasMask::~DlssVkBiasMask() { destroy(); }

bool DlssVkBiasMask::create(VkPhysicalDevice physical, VkDevice device,
                            uint32_t w, uint32_t h) {
    destroy();
    if (!physical || !device || !w || !h || w > 16384 || h > 16384) {
        error_ = "invalid dimensions or device for DLSS bias mask";
        return false;
    }
    device_ = device;
    width_ = w;
    height_ = h;
    auto fail = [this](const char* why) {
        destroy();
        error_ = why;
        return false;
    };

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format(), &props);
    constexpr VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((props.optimalTilingFeatures & needed) != needed)
        return fail("R8_UNORM storage/sampled image not supported");

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format();
    imageInfo.extent = {w, h, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS)
        return fail("bias mask image allocation failed");
    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(device_, image_, &imageReq);
    uint32_t type = memoryType(physical, imageReq.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return fail("no device-local memory for bias mask");
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = imageReq.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &imageMemory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, image_, imageMemory_, 0) != VK_SUCCESS)
        return fail("bias mask image bind failed");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format();
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &view_) != VK_SUCCESS)
        return fail("bias mask image view failed");

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(DlssBiasRect) * kMaxRects;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &rectBuffer_) != VK_SUCCESS)
        return fail("bias mask rectangle SSBO allocation failed");
    VkMemoryRequirements bufferReq{};
    vkGetBufferMemoryRequirements(device_, rectBuffer_, &bufferReq);
    type = memoryType(physical, bufferReq.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
        return fail("no host-coherent memory for bias rectangles");
    alloc.allocationSize = bufferReq.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &rectMemory_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, rectBuffer_, rectMemory_, 0) != VK_SUCCESS ||
        vkMapMemory(device_, rectMemory_, 0, bufferReq.size, 0,
                    &mappedRects_) != VK_SUCCESS)
        return fail("bias rectangle SSBO mapping failed");

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
                                    &descriptorLayout_) != VK_SUCCESS)
        return fail("bias descriptor layout failed");

    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = uint32_t(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr,
                               &descriptorPool_) != VK_SUCCESS)
        return fail("bias descriptor pool failed");
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descriptorPool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &descriptorLayout_;
    if (vkAllocateDescriptorSets(device_, &setAlloc,
                                 &descriptorSet_) != VK_SUCCESS)
        return fail("bias descriptor set failed");

    VkDescriptorImageInfo maskImage{};
    maskImage.imageView = view_;
    maskImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo rectBuffer{};
    rectBuffer.buffer = rectBuffer_;
    rectBuffer.offset = 0;
    rectBuffer.range = bufferInfo.size;
    VkWriteDescriptorSet writes[2]{};
    for (auto& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &maskImage;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &rectBuffer;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(Push);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                               &pipelineLayout_) != VK_SUCCESS)
        return fail("bias pipeline layout failed");

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kDlssBiasMaskSpv);
    shaderInfo.pCode = kDlssBiasMaskSpv;
    if (vkCreateShaderModule(device_, &shaderInfo, nullptr,
                             &shader_) != VK_SUCCESS)
        return fail("bias SPIR-V shader rejected");
    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = shader_;
    shaderStage.pName = "main";
    VkComputePipelineCreateInfo compute{};
    compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute.stage = shaderStage;
    compute.layout = pipelineLayout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &compute,
                                 nullptr, &pipeline_) != VK_SUCCESS)
        return fail("bias compute pipeline failed");
    error_.clear();
    return true;
}

bool DlssVkBiasMask::record(VkCommandBuffer cmd,
                            std::span<const DlssBiasRect> rects,
                            float strength) {
    if (!ready() || !cmd || rects.size() > kMaxRects ||
        !std::isfinite(strength) || strength < 0 || strength > 1)
        return false;
    if (!rects.empty())
        std::memcpy(mappedRects_, rects.data(),
                    rects.size() * sizeof(DlssBiasRect));

    VkImageMemoryBarrier enter{};
    enter.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    enter.oldLayout = imageLayout_;
    enter.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    enter.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    enter.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    enter.image = image_;
    enter.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    enter.srcAccessMask = imageLayout_ == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0 : VK_ACCESS_SHADER_READ_BIT;
    enter.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        imageLayout_ == VK_IMAGE_LAYOUT_UNDEFINED ?
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &enter);

    const Push push{int32_t(width_), int32_t(height_), int32_t(rects.size()),
                    strength};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    VkImageMemoryBarrier exit{};
    exit.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    exit.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    exit.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    exit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    exit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    exit.image = image_;
    exit.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    exit.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    exit.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &exit);
    imageLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void DlssVkBiasMask::destroy() {
    if (device_) {
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (shader_) vkDestroyShaderModule(device_, shader_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        if (rectMemory_ && mappedRects_) vkUnmapMemory(device_, rectMemory_);
        if (rectBuffer_) vkDestroyBuffer(device_, rectBuffer_, nullptr);
        if (rectMemory_) vkFreeMemory(device_, rectMemory_, nullptr);
        if (view_) vkDestroyImageView(device_, view_, nullptr);
        if (image_) vkDestroyImage(device_, image_, nullptr);
        if (imageMemory_) vkFreeMemory(device_, imageMemory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    imageMemory_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    rectBuffer_ = VK_NULL_HANDLE;
    rectMemory_ = VK_NULL_HANDLE;
    mappedRects_ = nullptr;
    descriptorLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    imageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    width_ = height_ = 0;
}

} // namespace xyo::render
#endif
