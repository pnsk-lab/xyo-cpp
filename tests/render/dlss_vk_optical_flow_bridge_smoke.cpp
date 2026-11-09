// OpticalFlow -> RG16F motion vectors across the dedicated OF and compute
// Vulkan queues. Uses binary GPU semaphores and no CPU readback between queues.
// This is a standalone synthetic smoke test, NOT yet the xyo play runtime.
#include "../../src/render/dlss_vk_optical_flow.hpp"
#include "../../src/render/dlss_vk_motion_converter.hpp"
#include "../../src/render/dlss_optical_confidence.hpp"
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace {
constexpr uint32_t W = 480, H = 360;
constexpr VkDeviceSize Pixels = VkDeviceSize(W) * H * 4;
#define VERIFY(expr) do { auto r = (expr); if (r != VK_SUCCESS) { \
    std::fprintf(stderr, "VK error %d: %s (%d)\n", int(r), #expr, __LINE__); \
    std::exit(3); } } while (0)
uint32_t memType(VkPhysicalDevice physical, uint32_t mask,
                 VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((mask & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    std::fprintf(stderr, "No suitable memory type\n");
    std::exit(3);
}
struct Buffer {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void create(VkPhysicalDevice physical, VkDevice d, VkDeviceSize size,
                VkBufferUsageFlags usage) {
        device = d;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        VERIFY(vkCreateBuffer(device, &info, nullptr, &buffer));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memType(physical, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VERIFY(vkAllocateMemory(device, &alloc, nullptr, &memory));
        VERIFY(vkBindBufferMemory(device, buffer, memory, 0));
    }
    void destroy() {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = {};
        memory = {};
    }
};
struct Source {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    void create(VkPhysicalDevice physical, VkDevice d) {
        device = d;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {W,H,1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VERIFY(vkCreateImage(d, &info, nullptr, &image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(d, image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memType(physical, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VERIFY(vkAllocateMemory(d, &alloc, nullptr, &memory));
        VERIFY(vkBindImageMemory(d, image, memory, 0));
    }
    void destroy() {
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        image = {};
        memory = {};
    }
};
void imageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.image = image;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
void submit(VkQueue queue, VkCommandBuffer cmd,
            VkSemaphore wait = VK_NULL_HANDLE,
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VkSemaphore signal = VK_NULL_HANDLE, VkFence fence = VK_NULL_HANDLE) {
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wi.semaphore = wait;
    wi.stageMask = stage;
    VkSemaphoreSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    si.semaphore = signal;
    si.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    info.waitSemaphoreInfoCount = wait ? 1u : 0u;
    info.pWaitSemaphoreInfos = wait ? &wi : nullptr;
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &ci;
    info.signalSemaphoreInfoCount = signal ? 1u : 0u;
    info.pSignalSemaphoreInfos = signal ? &si : nullptr;
    VERIFY(vkQueueSubmit2(queue, 1, &info, fence));
}
void copyFrame(VkCommandBuffer cmd, Source& source, Buffer& upload,
               uint32_t n, xyo::render::DlssVkOpticalFlow& optical) {
    imageBarrier(cmd, source.image,
        n ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = VkDeviceSize(n) * Pixels;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {W,H,1};
    vkCmdCopyBufferToImage(cmd, upload.buffer, source.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    optical.recordInput(cmd, source.image, n);
}
uint8_t pattern(int x, int y) {
    uint32_t v = uint32_t(x + 1403) * 741103597u ^
                 uint32_t(y + 803) * 1597334677u;
    v ^= v >> 13; v *= 3266489917u; v ^= v >> 16;
    return uint8_t(50u + v % 175u);
}
}
int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo = &app;
    VkInstance instance{};
    VERIFY(vkCreateInstance(&ic, nullptr, &instance));
    uint32_t count{};
    VERIFY(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VERIFY(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical{};
    for (auto device : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(device, &p);
        if (p.vendorID == 0x10DE) { physical = device; break; }
    }
    if (!physical) return 2;
    uint32_t qCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, families.data());
    uint32_t computeFamily = UINT32_MAX, opticalFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (computeFamily == UINT32_MAX &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) computeFamily = i;
        if (opticalFamily == UINT32_MAX &&
            (families[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV))
            opticalFamily = i;
    }
    if (computeFamily == UINT32_MAX || opticalFamily == UINT32_MAX ||
        computeFamily == opticalFamily) return 2;
    VkPhysicalDeviceOpticalFlowFeaturesNV opticalFeature{};
    opticalFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    opticalFeature.opticalFlow = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = &opticalFeature;
    f13.synchronization2 = VK_TRUE;
    float priority = 1.f;
    VkDeviceQueueCreateInfo queues[2]{};
    for (int i = 0; i < 2; ++i) {
        queues[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[i].queueFamilyIndex = i ? opticalFamily : computeFamily;
        queues[i].queueCount = 1;
        queues[i].pQueuePriorities = &priority;
    }
    const char* ext = VK_NV_OPTICAL_FLOW_EXTENSION_NAME;
    VkDeviceCreateInfo dc{};
    dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.pNext = &f13;
    dc.queueCreateInfoCount = 2;
    dc.pQueueCreateInfos = queues;
    dc.enabledExtensionCount = 1;
    dc.ppEnabledExtensionNames = &ext;
    VkDevice device{};
    VERIFY(vkCreateDevice(physical, &dc, nullptr, &device));
    VkQueue computeQueue{}, opticalQueue{};
    vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);
    vkGetDeviceQueue(device, opticalFamily, 0, &opticalQueue);
    xyo::render::DlssVkOpticalFlow optical;
    if (!optical.create(physical, device, computeFamily, opticalFamily, W, H)) {
        std::fprintf(stderr, "OpticalFlow: %s\n", optical.lastError().c_str());
        return 3;
    }
    xyo::render::DlssVkMotionConverter converter;
    if (!converter.create(physical, device, optical.packedBuffer(), W, H, 4,
                          optical.costBuffer())) {
        std::fprintf(stderr, "Converter: %s\n", converter.lastError().c_str());
        return 3;
    }
    Buffer upload, readback;
    upload.create(physical, device, Pixels * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    readback.create(physical, device, Pixels, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Source source;
    source.create(physical, device);
    void* mapped{};
    VERIFY(vkMapMemory(device, upload.memory, 0, Pixels * 2, 0, &mapped));
    const int dx = std::getenv("XYO_OPTICAL_SMOKE_SHIFT")
        ? std::atoi(std::getenv("XYO_OPTICAL_SMOKE_SHIFT")) : 6;
    const bool cut = std::getenv("XYO_OPTICAL_SMOKE_CUT") != nullptr;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const auto offset = (size_t(y) * W + x) * 4;
            const uint8_t a = pattern(int(x), int(y));
            const uint8_t b = cut
                ? pattern(int(x) * 13 + 19, int(y) * 17 + 23)
                : pattern(int(x) - dx, int(y));
            for (int c = 0; c < 3; ++c) {
                static_cast<uint8_t*>(mapped)[offset + c] = a;
                static_cast<uint8_t*>(mapped)[Pixels + offset + c] = b;
            }
            static_cast<uint8_t*>(mapped)[offset + 3] = 255;
            static_cast<uint8_t*>(mapped)[Pixels + offset + 3] = 255;
        }
    vkUnmapMemory(device, upload.memory);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = computeFamily;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    VERIFY(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 3;
    VkCommandBuffer cmds[3]{};
    VERIFY(vkAllocateCommandBuffers(device, &ai, cmds));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VERIFY(vkBeginCommandBuffer(cmds[0], &begin));
    copyFrame(cmds[0], source, upload, 0, optical);
    VERIFY(vkEndCommandBuffer(cmds[0]));
    submit(computeQueue, cmds[0]);
    VERIFY(vkQueueWaitIdle(computeQueue)); // initial-frame bootstrap only
    VERIFY(vkBeginCommandBuffer(cmds[1], &begin));
    copyFrame(cmds[1], source, upload, 1, optical);
    VERIFY(vkEndCommandBuffer(cmds[1]));
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore ready{}, finished{};
    VERIFY(vkCreateSemaphore(device, &si, nullptr, &ready));
    VERIFY(vkCreateSemaphore(device, &si, nullptr, &finished));
    submit(computeQueue, cmds[1], VK_NULL_HANDLE,
           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ready);

    VkCommandBuffer ofCmd = optical.opticalCommandBuffer();
    VERIFY(vkBeginCommandBuffer(ofCmd, &begin));
    optical.recordFlow(ofCmd, 1);
    VERIFY(vkEndCommandBuffer(ofCmd));
    submit(opticalQueue, ofCmd, ready,
           VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV |
           VK_PIPELINE_STAGE_2_TRANSFER_BIT, finished);

    VERIFY(vkBeginCommandBuffer(cmds[2], &begin));
    const bool clipCosts =
        std::getenv("XYO_OPTICAL_SMOKE_COST_CLIP") != nullptr;
    const bool costSupported = optical.costAvailable();
    converter.record(cmds[2], clipCosts ? 8 : 0);
    imageBarrier(cmds[2], converter.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy read{};
    read.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    read.imageExtent = {W,H,1};
    vkCmdCopyImageToBuffer(cmds[2], converter.image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &read);
    VERIFY(vkEndCommandBuffer(cmds[2]));
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence{};
    VERIFY(vkCreateFence(device, &fi, nullptr, &fence));
    submit(computeQueue, cmds[2], finished,
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_NULL_HANDLE, fence);
    VERIFY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    xyo::render::DlssVkOpticalFlow::CostStats costs{};
    bool costPass = true;
    if (optical.readCost(costs, 8)) {
        const bool unreliable = xyo::render::dlssMotionUnreliable(
            costs.mean, costs.highFraction);
        std::printf("Optical Flow cost: mean %.2f, high>=8: %.3f, "
                    "max=%u, resetHistory=%d\n",
                    costs.mean, costs.highFraction, unsigned(costs.max),
                    int(unreliable));
        // The opt-in synthetic cut and known 6px translation must be
        // discriminated. Other distances remain exploratory benchmarks.
        if (cut) costPass = unreliable;
        else if (dx == 6) costPass = !unreliable;
    } else
        std::puts("Optical Flow cost: unsupported, skip confidence assertion");
    void* values{};
    VERIFY(vkMapMemory(device, readback.memory, 0, Pixels, 0, &values));
    const auto* comps = static_cast<const _Float16*>(values);
    const size_t center = (size_t(H / 2) * W + W / 2) * 2;
    const float vx = float(comps[center]);
    const float vy = float(comps[center+1]);
    std::printf("GPU semaphore optical queue %u -> compute queue %u\n",
                opticalFamily, computeFamily);
    std::printf("Converted 480x360 RG16F DLSS motion: (%.3f, %.3f)\n", vx, vy);
    vkUnmapMemory(device, readback.memory);
    VERIFY(vkDeviceWaitIdle(device));
    vkDestroyFence(device, fence, nullptr);
    vkDestroySemaphore(device, finished, nullptr);
    vkDestroySemaphore(device, ready, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    converter.destroy();
    optical.destroy();
    source.destroy();
    upload.destroy();
    readback.destroy();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    const bool clippedCut = clipCosts && cut && costSupported;
    const bool pass = costPass &&
        (clippedCut ? (std::abs(vx) < 0.1f && std::abs(vy) < 0.1f) :
        (cut || std::abs(dx) != 6 ||
        (std::abs(vx + dx) < 0.25f && std::abs(vy) < 0.25f)));
    std::puts(pass ? "PASS: GPU optical flow cross-queue semaphores"
                   : "FAIL: optical flow conversion or queue sync");
    return pass ? 0 : 4;
}
