// Real Vulkan compute test for the optional DLSS R8_UNORM bias mask.
// Checks that rectangles, not the entire frame, are written on GPU.
#include "../../src/render/dlss_vk_bias_mask.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
using xyo::render::DlssVkBiasMask;
using xyo::render::DlssBiasRect;

#define CHECK_VK(expr) do { VkResult r = (expr); if (r != VK_SUCCESS) { \
    std::fprintf(stderr, "Vulkan error: %s = %d\n", #expr, int(r)); \
    return 2; } } while (0)

int main() {
    constexpr uint32_t W = 64, H = 48;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance{};
    CHECK_VK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    uint32_t count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (auto d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.vendorID == 0x10DE) { physical = d; break; }
    }
    if (!physical) return 3;
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount,
                                             families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) return 3;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device{};
    CHECK_VK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    DlssVkBiasMask mask;
    if (!mask.create(physical, device, W, H)) {
        std::fprintf(stderr, "Mask allocation failed: %s\n",
                     mask.lastError().c_str());
        return 3;
    }
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = W * H;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    CHECK_VK(vkCreateBuffer(device, &bufInfo, nullptr, &buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; break;
        }
    if (type == UINT32_MAX) return 3;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    CHECK_VK(vkAllocateMemory(device, &alloc, nullptr, &memory));
    CHECK_VK(vkBindBufferMemory(device, buffer, memory, 0));
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    CHECK_VK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cb{};
    CHECK_VK(vkAllocateCommandBuffers(device, &cbInfo, &cb));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK_VK(vkBeginCommandBuffer(cb, &begin));
    const std::array<DlssBiasRect, 2> rectangles{{{8, 6, 16, 14},
                                                   {25, 20, 37, 32}}};
    if (!mask.record(cb, rectangles, 1.0f)) return 4;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = mask.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cb, mask.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                           1, &copy);
    CHECK_VK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK_VK(vkQueueWaitIdle(queue));
    void* mapped{};
    CHECK_VK(vkMapMemory(device, memory, 0, W * H, 0, &mapped));
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    uint32_t inside = 0, outside = 0, errors = 0;
    for (uint32_t y=0; y<H; ++y)
        for (uint32_t x=0; x<W; ++x) {
            const bool expected = ((x>=8 && x<16 && y>=6 && y<14) ||
                                   (x>=25 && x<37 && y>=20 && y<32));
            const auto actual = bytes[y*W+x];
            if (expected) ++inside;
            else ++outside;
            if (actual != (expected ? 255 : 0)) ++errors;
        }
    std::printf("GPU bias mask: inside=%u, outside=%u, pixel errors=%u\n",
                inside, outside, errors);
    vkUnmapMemory(device, memory);
    vkDestroyCommandPool(device, pool, nullptr);
    mask.destroy();
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (errors) return 1;
    std::puts("PASS: GPU writes exact local rectangles to R8_UNORM NGX mask");
    return 0;
}
