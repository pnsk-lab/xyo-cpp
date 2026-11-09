// Probe NVIDIA's physical optical-flow engine, without NGX or proprietary libs.
// Build: g++ -std=c++20 -Ithird_party/Vulkan-Headers/include
//   tests/render/dlss_vk_optical_flow_probe.cpp -lvulkan -o /tmp/optical-probe
#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xyo optical flow probe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    VkInstance instance{};
    if (vkCreateInstance(&ic, nullptr, &instance) != VK_SUCCESS) return 1;
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    int status = 2;
    for (const VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        if (props.vendorID != 0x10DE) continue;
        std::printf("GPU: %s, Vulkan %u.%u\n", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion));
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, exts.data());
        auto has = [&](const char* name) {
            return std::any_of(exts.begin(), exts.end(), [&](const auto& e) {
                return std::string(e.extensionName) == name;
            });
        };
        if (!has(VK_NV_OPTICAL_FLOW_EXTENSION_NAME)) {
            std::puts("VK_NV_optical_flow: unavailable");
            continue;
        }
        VkPhysicalDeviceOpticalFlowFeaturesNV opticalFeature{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &opticalFeature;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        VkPhysicalDeviceOpticalFlowPropertiesNV opticalProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &opticalProps;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        std::printf("OpticalFlow feature=%u; grid mask=0x%x; dimensions "
                    "[%ux%u, %ux%u]; costs=%u, global=%u, both=%u\n",
                    opticalFeature.opticalFlow, opticalProps.supportedOutputGridSizes,
                    opticalProps.minWidth, opticalProps.minHeight,
                    opticalProps.maxWidth, opticalProps.maxHeight,
                    opticalProps.costSupported, opticalProps.globalFlowSupported,
                    opticalProps.bidirectionalFlowSupported);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, queues.data());
        uint32_t queueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i) {
            std::printf("Queue %u: flags 0x%x, count %u\n", i,
                        queues[i].queueFlags, queues[i].queueCount);
            if (queues[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV)
                queueFamily = i;
        }
        if (!opticalFeature.opticalFlow || queueFamily == UINT32_MAX)
            continue;
        const char* enabled[] = {VK_NV_OPTICAL_FLOW_EXTENSION_NAME};
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &opticalFeature;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = enabled;
        VkDevice device{};
        const auto created = vkCreateDevice(physical, &di, nullptr, &device);
        std::printf("OpticalFlow logical device create: %d\n", int(created));
        if (created != VK_SUCCESS) continue;
        auto formats = reinterpret_cast<PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceOpticalFlowImageFormatsNV"));
        auto createSession = reinterpret_cast<PFN_vkCreateOpticalFlowSessionNV>(
            vkGetDeviceProcAddr(device, "vkCreateOpticalFlowSessionNV"));
        auto destroySession = reinterpret_cast<PFN_vkDestroyOpticalFlowSessionNV>(
            vkGetDeviceProcAddr(device, "vkDestroyOpticalFlowSessionNV"));
        if (!formats || !createSession || !destroySession) {
            std::puts("OpticalFlow Vulkan function pointers missing");
            vkDestroyDevice(device, nullptr);
            continue;
        }
        auto list = [&](VkOpticalFlowUsageFlagsNV usage) {
            VkOpticalFlowImageFormatInfoNV info{
                VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV};
            info.usage = usage;
            uint32_t n = 0;
            auto r = formats(physical, &info, &n, nullptr);
            std::vector<VkOpticalFlowImageFormatPropertiesNV> result(n);
            for (auto& i : result)
                i.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV;
            if (r == VK_SUCCESS)
                r = formats(physical, &info, &n, result.data());
            std::printf("OpticalFlow usage 0x%x: query=%d count=%u formats:",
                        usage, int(r), n);
            for (auto& i : result) std::printf(" %u", unsigned(i.format));
            std::puts("");
            return result;
        };
        const auto input = list(VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV);
        const auto output = list(VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV);
        if (input.empty() || output.empty()) {
            vkDestroyDevice(device, nullptr);
            continue;
        }
        auto checkImageUsage = [&](VkFormat format,
                                   VkOpticalFlowUsageFlagsNV opticalUsage,
                                   VkImageUsageFlags vkUsage) {
            VkOpticalFlowImageFormatInfoNV opticalInfo{
                VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV};
            opticalInfo.usage = opticalUsage;
            VkPhysicalDeviceImageFormatInfo2 imageInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            imageInfo.pNext = &opticalInfo;
            imageInfo.format = format;
            imageInfo.type = VK_IMAGE_TYPE_2D;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = vkUsage;
            VkImageFormatProperties2 imageProperties{
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
                physical, &imageInfo, &imageProperties);
            std::printf("Optical image usage 0x%x, VkUsage 0x%x, format %u: %d\n",
                        opticalUsage, vkUsage, unsigned(format), int(result));
            return result == VK_SUCCESS;
        };
        checkImageUsage(VK_FORMAT_B8G8R8A8_UNORM,
                        VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT);
        checkImageUsage(VK_FORMAT_R16G16_SFIXED5_NV,
                        VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_STORAGE_BIT);
        VkOpticalFlowSessionCreateInfoNV info{
            VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV};
        info.width = 480;
        info.height = 360;
        info.imageFormat = input.front().format;
        info.flowVectorFormat = output.front().format;
        info.outputGridSize = (opticalProps.supportedOutputGridSizes &
            VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV)
            ? VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV
            : VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV;
        info.performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV;
        VkOpticalFlowSessionNV session{};
        const VkResult sr = createSession(device, &info, nullptr, &session);
        std::printf("OpticalFlow session 480x360: result=%d, queue=%u, grid=0x%x\n",
                    int(sr), queueFamily, info.outputGridSize);
        if (session) destroySession(device, session, nullptr);
        vkDestroyDevice(device, nullptr);
        status = sr == VK_SUCCESS ? 0 : 3;
        break;
    }
    vkDestroyInstance(instance, nullptr);
    return status;
}
