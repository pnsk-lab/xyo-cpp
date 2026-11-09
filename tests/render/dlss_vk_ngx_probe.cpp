// NVIDIA NGX Vulkan SDK capability probe: does not run DLSS inference.
// Requires a local copy of NVIDIA's proprietary-licensed SDK headers and lib.
#include "../../src/render/dlss_vk_backend.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

using xyo::render::DlssVkBackend;
#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, #call " failed at %d\n", __LINE__); \
    return 1; } } while (0)

int main() {
    constexpr char projectId[] = "a4f4aaf4-72b9-4fe1-b6f0-6d20d129dd1a";
    const std::wstring dataDir = L"/tmp";

    std::vector<std::string> instanceExt;
    if (!DlssVkBackend::requiredInstanceExtensions(projectId, dataDir,
                                                    instanceExt)) {
        std::fprintf(stderr, "NGX instance extension discovery unavailable\n");
        return 2;
    }
    std::vector<const char*> instanceNames;
    for (const auto& ext : instanceExt) instanceNames.push_back(ext.c_str());
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xyo-ngxsdk-probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(instanceNames.size());
    ci.ppEnabledExtensionNames = instanceNames.data();
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, nullptr, &instance));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (auto d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.vendorID == 0x10DE) {
            physical = d;
            std::printf("NGX Vulkan GPU: %s\n", props.deviceName);
            break;
        }
    }
    if (!physical) return 2;

    std::vector<std::string> deviceExt;
    if (!DlssVkBackend::requiredDeviceExtensions(
            instance, physical, projectId, dataDir, deviceExt)) {
        std::fprintf(stderr, "NGX device extension discovery unavailable\n");
        return 2;
    }
    std::printf("NGX instance extensions: %zu, device extensions: %zu\n",
                instanceExt.size(), deviceExt.size());
    float priority = 1.0f;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> properties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families,
                                             properties.data());
    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) {
        if (properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) return 2;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    std::vector<const char*> deviceNames;
    for (const auto& ext : deviceExt) deviceNames.push_back(ext.c_str());
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = static_cast<uint32_t>(deviceNames.size());
    di.ppEnabledExtensionNames = deviceNames.data();
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &di, nullptr, &device));

    const char* featureDirEnv = std::getenv("XYO_DLSS_FEATURE_DIR");
    std::wstring featureDir = featureDirEnv
        ? std::filesystem::path(featureDirEnv).wstring() : L"";
    DlssVkBackend backend;
    bool initialized = backend.initialize(instance, physical, device,
                                          projectId, dataDir,
                                          vkGetInstanceProcAddr,
                                          vkGetDeviceProcAddr,
                                          featureDir);
    std::printf("NGX DLSS available: %s (%s)\n",
                initialized ? "YES" : "NO", backend.lastError().c_str());
    backend.shutdown();
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return initialized ? 0 : 3;
}
