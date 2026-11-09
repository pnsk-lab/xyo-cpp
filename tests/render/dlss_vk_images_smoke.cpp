// Standalone real-GPU smoke test for DLSS's Vulkan image targets.
// Compile with:
// g++ -std=c++20 -O2 -DXYO_NVSDK_DLSS_VK=1 \
//   -Ithird_party/Vulkan-Headers/include \
//   tests/render/dlss_vk_images_smoke.cpp src/render/dlss_vk_images.cpp \
//   -lvulkan -o /tmp/dlss-vk-images-smoke
#include "../../src/render/dlss_vk_images.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using xyo::render::DlssVkFrameTargets;
#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed at line %d\n", #call, __LINE__); \
    return 1; } } while (0)

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xyo-cpp-dlss-vk-smoke";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.vendorID != 0x10DE) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families,
                                                 properties.data());
        for (uint32_t i = 0; i < families; ++i)
            if (properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                physical = candidate;
                queueFamily = i;
                break;
            }
        if (physical) {
            std::printf("GPU: %s\n", props.deviceName);
            break;
        }
    }
    if (!physical) {
        std::fprintf(stderr, "No NVIDIA Vulkan compute GPU; skipped\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &di, nullptr, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));

    for (unsigned width : {480u, 960u}) {
        DlssVkFrameTargets targets;
        const unsigned height = width * 3 / 4;
        if (!targets.create(physical, device, width, height,
                            width * 2, height * 2)) {
            std::fprintf(stderr, "Target allocation failed: %s\n",
                         targets.lastError().c_str());
            return 1;
        }
        CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        if (!targets.prepareUpload(cmd)) {
            std::fprintf(stderr, "%s\n", targets.lastError().c_str());
            return 1;
        }
        VkClearColorValue color{};
        color.float32[0] = .4f;
        color.float32[1] = .2f;
        color.float32[2] = .7f;
        color.float32[3] = 1.0f;
        VkImageSubresourceRange colorRange{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageSubresourceRange depthRange{
            VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkClearDepthStencilValue depth{.5f, 0};
        vkCmdClearColorImage(cmd, targets.color().image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &colorRange);
        vkCmdClearColorImage(cmd, targets.motion().image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &colorRange);
        vkCmdClearDepthStencilImage(cmd, targets.depth().image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &depth, 1, &depthRange);
        if (!targets.prepareEvaluate(cmd) || !targets.prepareReadback(cmd)) {
            std::fprintf(stderr, "%s\n", targets.lastError().c_str());
            return 1;
        }
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        std::printf("PASS: Vulkan target allocation + clear + "
                    "barriers + submit %ux%u -> %ux%u\n",
                    width, height, width * 2, height * 2);
    }

    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
