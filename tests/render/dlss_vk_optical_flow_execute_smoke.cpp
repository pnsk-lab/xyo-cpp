// End-to-end VK_NV_optical_flow hardware execution with synthetic translated
// frames. Standalone, no NVIDIA proprietary SDK dependencies. Vulkan 1.3.
#include <vulkan/vulkan.h>
#include "../../src/render/dlss_vk_motion_converter.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t W = 480, H = 360, GRID = 4;
constexpr uint32_t GW = W / GRID, GH = H / GRID;
struct Image {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
};
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
};
#define CHECK(expr) do { auto result_ = (expr); if (result_ != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed: %d at %d\n", #expr, int(result_), __LINE__); \
    std::exit(3); } } while(0)

uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits,
                    VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    std::fprintf(stderr, "No compatible Vulkan memory type\n");
    std::exit(3);
}

Image allocateImage(VkPhysicalDevice physical, VkDevice device,
                    VkFormat format, VkOpticalFlowUsageFlagsNV usage,
                    VkImageUsageFlags vkUsage, uint32_t width, uint32_t height) {
    VkOpticalFlowImageFormatInfoNV optical{};
    optical.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
    optical.usage = usage;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &optical;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = vkUsage;
    Image result;
    CHECK(vkCreateImage(device, &info, nullptr, &result.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, result.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(physical, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(device, &alloc, nullptr, &result.memory));
    CHECK(vkBindImageMemory(device, result.image, result.memory, 0));
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = result.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(device, &view, nullptr, &result.view));
    return result;
}
Buffer allocateBuffer(VkPhysicalDevice physical, VkDevice device,
                      VkDeviceSize size, VkBufferUsageFlags usage,
                      uint32_t opticalFamily = UINT32_MAX,
                      uint32_t computeFamily = UINT32_MAX) {
    Buffer result;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    const uint32_t sharedFamilies[2] = {opticalFamily, computeFamily};
    if (opticalFamily != UINT32_MAX && computeFamily != UINT32_MAX &&
        opticalFamily != computeFamily) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = sharedFamilies;
    }
    CHECK(vkCreateBuffer(device, &info, nullptr, &result.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, result.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(physical, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(device, &alloc, nullptr, &result.memory));
    CHECK(vkBindBufferMemory(device, result.buffer, result.memory, 0));
    return result;
}
void freeImage(VkDevice device, Image& image) {
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}
void freeBuffer(VkDevice device, Buffer& buffer) {
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}
void barrier(VkCommandBuffer cmd, VkImage image,
             VkImageLayout oldLayout, VkImageLayout newLayout,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo deps{};
    deps.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    deps.imageMemoryBarrierCount = 1;
    deps.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &deps);
}
uint8_t pixel(int x, int y) {
    // A textured image enables motion estimation across the whole frame.
    uint32_t n = uint32_t(x + 1403) * 741103597u ^
                 uint32_t(y + 803) * 1597334677u;
    n ^= n >> 13; n *= 3266489917u; n ^= n >> 16;
    return uint8_t(50u + n % 175u);
}
} // namespace
int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "xyo OpticalFlow execute smoke";
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance{};
    CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical{};
    for (auto device : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.vendorID == 0x10de) { physical = device; break; }
    }
    if (!physical) { std::fprintf(stderr, "NVIDIA GPU required\n"); return 2; }
    VkPhysicalDeviceOpticalFlowFeaturesNV opticalFeature{};
    opticalFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = &opticalFeature;
    VkPhysicalDeviceFeatures2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    query.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    if (!opticalFeature.opticalFlow || !f13.synchronization2) {
        std::fprintf(stderr, "OpticalFlow/synchronization2 features missing\n");
        return 2;
    }
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount,
                                             families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if ((families[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) &&
            (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { family = i; break; }
    if (family == UINT32_MAX) {
        std::fprintf(stderr, "Need transfer-capable optical flow queue\n");
        return 2;
    }
    uint32_t computeFamily = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeFamily = i;
            break;
        }
    if (computeFamily == UINT32_MAX || computeFamily == family)
        return 2;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceQueueCreateInfo computeQci = qci;
    computeQci.queueFamilyIndex = computeFamily;
    const VkDeviceQueueCreateInfo queueInfos[2]{qci, computeQci};
    const char* extension = VK_NV_OPTICAL_FLOW_EXTENSION_NAME;
    f13.synchronization2 = VK_TRUE;
    opticalFeature.opticalFlow = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 2;
    dci.pQueueCreateInfos = queueInfos;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &extension;
    VkDevice device{};
    CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    VkQueue computeQueue{};
    vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);

    auto create = reinterpret_cast<PFN_vkCreateOpticalFlowSessionNV>(
        vkGetDeviceProcAddr(device, "vkCreateOpticalFlowSessionNV"));
    auto bind = reinterpret_cast<PFN_vkBindOpticalFlowSessionImageNV>(
        vkGetDeviceProcAddr(device, "vkBindOpticalFlowSessionImageNV"));
    auto execute = reinterpret_cast<PFN_vkCmdOpticalFlowExecuteNV>(
        vkGetDeviceProcAddr(device, "vkCmdOpticalFlowExecuteNV"));
    auto destroy = reinterpret_cast<PFN_vkDestroyOpticalFlowSessionNV>(
        vkGetDeviceProcAddr(device, "vkDestroyOpticalFlowSessionNV"));
    if (!create || !bind || !execute || !destroy) return 3;
    VkOpticalFlowSessionCreateInfoNV createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV;
    createInfo.width = W; createInfo.height = H;
    createInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    createInfo.flowVectorFormat = VK_FORMAT_R16G16_SFIXED5_NV;
    createInfo.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
    createInfo.performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV;
    VkOpticalFlowSessionNV session{};
    CHECK(create(device, &createInfo, nullptr, &session));

    auto current = allocateImage(physical, device, createInfo.imageFormat,
        VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, W, H);
    auto previous = allocateImage(physical, device, createInfo.imageFormat,
        VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, W, H);
    auto flow = allocateImage(physical, device, createInfo.flowVectorFormat,
        VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT, GW, GH);
    const VkDeviceSize inputBytes = VkDeviceSize(W) * H * 4;
    const VkDeviceSize resultBytes = VkDeviceSize(GW) * GH * 4;
    auto upload = allocateBuffer(physical, device, inputBytes * 2,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto download = allocateBuffer(physical, device, resultBytes,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   family, computeFamily);
    void* inputPtr{};
    CHECK(vkMapMemory(device, upload.memory, 0, inputBytes * 2, 0, &inputPtr));
    constexpr int dx = 6, dy = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t offset = (size_t(y) * W + x) * 4;
            const auto prev = pixel(int(x), int(y));
            const auto curr = pixel(int(x) - dx, int(y) - dy);
            for (unsigned c = 0; c < 3; ++c) {
                static_cast<uint8_t*>(inputPtr)[offset + c] = prev;
                static_cast<uint8_t*>(inputPtr)[inputBytes + offset + c] = curr;
            }
            static_cast<uint8_t*>(inputPtr)[offset + 3] = 255;
            static_cast<uint8_t*>(inputPtr)[inputBytes + offset + 3] = 255;
        }
    vkUnmapMemory(device, upload.memory);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool{};
    CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(vkAllocateCommandBuffers(device, &commandInfo, &cmd));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK(vkBeginCommandBuffer(cmd, &begin));

    for (const auto& img : {current.image, previous.image})
        barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (unsigned i = 0; i < 2; ++i) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = i * inputBytes;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {W, H, 1};
        // First staging frame = previous, second = current.
        vkCmdCopyBufferToImage(cmd, upload.buffer,
            i == 0 ? previous.image : current.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    for (const auto& img : {current.image, previous.image})
        barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV);
    barrier(cmd, flow.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
            VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV);
    CHECK(bind(device, session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV,
               current.view, VK_IMAGE_LAYOUT_GENERAL));
    CHECK(bind(device, session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV,
               previous.view, VK_IMAGE_LAYOUT_GENERAL));
    CHECK(bind(device, session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV,
               flow.view, VK_IMAGE_LAYOUT_GENERAL));
    VkOpticalFlowExecuteInfoNV ei{};
    ei.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV;
    ei.flags = VK_OPTICAL_FLOW_EXECUTE_DISABLE_TEMPORAL_HINTS_BIT_NV;
    execute(cmd, session, &ei);
    barrier(cmd, flow.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
            VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy read{};
    read.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    read.imageExtent = {GW, GH, 1};
    vkCmdCopyImageToBuffer(cmd, flow.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           download.buffer, 1, &read);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    void* values{};
    CHECK(vkMapMemory(device, download.memory, 0, resultBytes, 0, &values));
    const auto* f = static_cast<const int16_t*>(values);
    double avgX = 0, avgY = 0;
    unsigned samples = 0, nonzero = 0;
    for (uint32_t y = GH / 4; y < GH * 3 / 4; ++y)
        for (uint32_t x = GW / 4; x < GW * 3 / 4; ++x) {
            const size_t i = (size_t(y) * GW + x) * 2;
            avgX += f[i] / 32.0;
            avgY += f[i+1] / 32.0;
            nonzero += (f[i] != 0 || f[i+1] != 0);
            samples++;
        }
    const auto idx = size_t(GH / 2) * GW + GW / 2;
    std::printf("OpticalFlow GPU result, %ux%u -> grid %ux%u:\n",
                W, H, GW, GH);
    std::printf("  center flow (current -> previous): %.3f %.3f pixels\n",
                f[idx*2]/32.0, f[idx*2+1]/32.0);
    std::printf("  central mean flow: %.3f %.3f pixels; nonzero=%u/%u\n",
                avgX / samples, avgY / samples, nonzero, samples);
    vkUnmapMemory(device, download.memory);

    // Read the exact same packed flow buffer with a compute shader on a
    // distinct Vulkan compute queue, converting to full-frame RG16_SFLOAT.
    // This diagnostic uses vkQueueWaitIdle above to transfer ownership
    // safely; the production renderer will use Vulkan GPU semaphores.
    xyo::render::DlssVkMotionConverter converter;
    if (!converter.create(physical, device, download.buffer, W, H, GRID)) {
        std::fprintf(stderr, "Motion converter: %s\n",
                     converter.lastError().c_str());
        return 5;
    }
    auto decoded = allocateBuffer(physical, device, inputBytes,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandPoolCreateInfo computePoolInfo{};
    computePoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    computePoolInfo.queueFamilyIndex = computeFamily;
    VkCommandPool computePool{};
    CHECK(vkCreateCommandPool(device, &computePoolInfo, nullptr, &computePool));
    VkCommandBufferAllocateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ci.commandPool = computePool;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    VkCommandBuffer convertCmd{};
    CHECK(vkAllocateCommandBuffers(device, &ci, &convertCmd));
    CHECK(vkBeginCommandBuffer(convertCmd, &begin));
    converter.record(convertCmd);
    barrier(convertCmd, converter.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy decodedCopy{};
    decodedCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    decodedCopy.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(convertCmd, converter.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, decoded.buffer, 1, &decodedCopy);
    CHECK(vkEndCommandBuffer(convertCmd));
    VkSubmitInfo computeSubmit{};
    computeSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    computeSubmit.commandBufferCount = 1;
    computeSubmit.pCommandBuffers = &convertCmd;
    CHECK(vkQueueSubmit(computeQueue, 1, &computeSubmit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(computeQueue));

    void* decodedPixels{};
    CHECK(vkMapMemory(device, decoded.memory, 0, inputBytes, 0, &decodedPixels));
    const auto* components = static_cast<const _Float16*>(decodedPixels);
    const size_t mid = (size_t(H / 2) * W + W / 2) * 2;
    const float vx = float(components[mid]);
    const float vy = float(components[mid + 1]);
    std::printf("Converted DLSS per-pixel motion (center): %.3f %.3f pixels\n",
                vx, vy);
    vkUnmapMemory(device, decoded.memory);
    converter.destroy();
    vkDestroyCommandPool(device, computePool, nullptr);
    freeBuffer(device, decoded);
    const bool matches = std::abs(vx + dx) < 0.25f && std::abs(vy) < 0.25f;
    freeBuffer(device, download);
    freeBuffer(device, upload);
    destroy(device, session, nullptr);
    freeImage(device, flow);
    freeImage(device, previous);
    freeImage(device, current);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return nonzero > samples / 2 && matches ? 0 : 4;
}
