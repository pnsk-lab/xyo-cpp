// Native NVIDIA NGX DLSS Super Resolution evaluation + Vulkan/GL readback.
// Requires a local copy of NVIDIA's proprietary-licensed SDK headers and lib.
#define GL_GLEXT_PROTOTYPES
#include "../../src/render/dlss_vk_backend.hpp"
#include "../../src/render/dlss_gl_external.hpp"
#include "../../src/render/dlss_gl_vk_sync.hpp"
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "../../src/render/dlss_vk_images.hpp"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <algorithm>

using xyo::render::DlssVkBackend;
#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, #call " failed at %d\n", __LINE__); \
    return 1; } } while (0)

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL video unavailable: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow("xyo NGX GL shared output",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) return 2;
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) return 2;
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
    // Shared Vulkan images are imported into OpenGL via opaque FD.
    if (std::find(deviceExt.begin(), deviceExt.end(),
                  VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ==
        deviceExt.end())
        deviceNames.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (std::find(deviceExt.begin(), deviceExt.end(),
                  VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) ==
        deviceExt.end())
        deviceNames.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
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
    VkQueue queue{};
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = queueFamily;
    VkCommandPool pool{};
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

    xyo::render::DlssVkFrameTargets images;
    constexpr uint32_t iw=480, ih=360, ow=960, oh=720;
    if (!images.create(physical, device, iw, ih, ow, oh, true)) {
        std::fprintf(stderr, "Images: %s\n", images.lastError().c_str());
        return 4;
    }
    xyo::render::DlssGlSharedTexture sharedColor;
    if (!sharedColor.import(images, images.color(), SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "External color import: %s\n",
                     sharedColor.lastError().c_str());
        return 4;
    }
    xyo::render::DlssGlSharedTexture sharedOutput;
    if (!sharedOutput.import(images, images.output(), SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "External GL image import: %s\n",
                     sharedOutput.lastError().c_str());
        return 4;
    }
    xyo::render::DlssGlVkSync sync;
    if (!sync.initialize(device, SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "External semaphore setup: %s\n",
                     sync.lastError().c_str());
        return 4;
    }
    // The GL and Vulkan copies of this external binary semaphore are separate
    // APIs to the same GPU sync primitive. This input signal precedes the
    // VkQueueSubmit wait; the output signal is waited on in GL before FBO read.
    // Real interop: render with OpenGL directly into the externally
    // allocated Vulkan color image. The Vulkan DLSS pass consumes precisely
    // these pixels, without any readback/copy on the CPU.
    GLuint colorFbo = 0;
    glGenFramebuffers(1, &colorFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, colorFbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         sharedColor.name(), 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 4;
    glViewport(0, 0, iw, ih);
    glClearColor(.25f, .5f, .85f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &colorFbo);
    if (!sync.signalGlToVk(sharedColor.name(), GL_LAYOUT_GENERAL_EXT))
        return 4;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (!images.acquireGlColor(cmd, queueFamily)) return 4;
    if (!images.prepareDepthMotionUpload(cmd)) return 4;
    // Synthetic still-frame data ONLY for SDK validation, never used for a
    // Scratch project. This test deliberately supplies non-geometric depth and
    // zero motion and therefore says nothing about DLSS temporal quality.
    VkClearColorValue cc{};
    cc.float32[0] = .25f; cc.float32[1] = .5f;
    cc.float32[2] = .85f; cc.float32[3] = 1;
    VkClearColorValue zero{};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkClearDepthStencilValue dc{.5f, 0};
    // Color comes from the shared OpenGL image, not Vulkan clear.
    vkCmdClearColorImage(cmd, images.motion().image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &cr);
    vkCmdClearDepthStencilImage(cmd, images.depth().image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc, 1, &dr);
    if (!images.prepareEvaluate(cmd)) return 4;

    if (!backend.configure(cmd, iw, ih, ow, oh)) {
        std::fprintf(stderr, "DLSS create: %s\n",
                     backend.lastError().c_str());
        return 4;
    }
    auto vkImage = [](const xyo::render::DlssVkOwnedImage& image) {
        xyo::render::DlssVkImage dst{};
        dst.image=image.image; dst.view=image.view;
        dst.format=image.format; dst.width=image.width;
        dst.height=image.height;
        dst.range = {image.aspect, 0, 1, 0, 1};
        return dst;
    };
    xyo::render::DlssVkFrame frame{};
    frame.color=vkImage(images.color());
    frame.depth=vkImage(images.depth());
    frame.motionVectors=vkImage(images.motion());
    frame.output=vkImage(images.output());
    frame.resetHistory=true;
    const auto start = std::chrono::steady_clock::now();
    if (!backend.evaluate(cmd, frame)) {
        std::fprintf(stderr, "DLSS eval: %s\n",
                     backend.lastError().c_str());
        return 4;
    }
    if (!images.prepareReadback(cmd)) return 4;
    // Retrieve the reconstructed image to verify the GPU actually wrote
    // nonzero, finite pixels rather than merely accepting an NGX call.
    constexpr VkDeviceSize bytes = VkDeviceSize(ow) * oh * 8;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bytes;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging{};
    CHECK(vkCreateBuffer(device, &bufInfo, nullptr, &staging));
    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(device, staging, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memType = i;
            break;
        }
    if (memType == UINT32_MAX) return 4;
    VkMemoryAllocateInfo memAlloc{};
    memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAlloc.allocationSize = memReq.size;
    memAlloc.memoryTypeIndex = memType;
    VkDeviceMemory stagingMemory{};
    CHECK(vkAllocateMemory(device, &memAlloc, nullptr, &stagingMemory));
    CHECK(vkBindBufferMemory(device, staging, stagingMemory, 0));
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {ow, oh, 1};
    vkCmdCopyImageToBuffer(cmd, images.output().image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &copy);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore waitSem = sync.glToVkSemaphore();
    VkSemaphore signalSem = sync.vkToGlSemaphore();
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &waitSem;
    submit.pWaitDstStageMask = &waitStage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signalSem;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    // No CPU vkQueueWaitIdle here: the GL wait is a GPU-side dependency.
    if (!sync.waitVkToGl(sharedOutput.name(), GL_LAYOUT_TRANSFER_SRC_EXT)) {
        std::fprintf(stderr, "External semaphore GL wait: %s\n",
                     sync.lastError().c_str());
        return 4;
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end-start).count();
    std::printf("NGX DLSS evaluation SUCCESS; GPU submission+wait %.2f ms\n",
                ms);

    // Read the SAME memory object through OpenGL. GPU-to-GPU synchronization
    // was done with a GL/VK external semaphore (no glFinish/vkQueueWaitIdle
    // before this access).
    GLuint glFbo = 0;
    glGenFramebuffers(1, &glFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, glFbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         sharedOutput.name(), 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "NGX output GL FBO incomplete\n");
        return 5;
    }
    float glPixel[4]{};
    glReadPixels(ow/2, oh/2, 1, 1, GL_RGBA, GL_FLOAT, glPixel);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &glFbo);
    std::printf("OpenGL imported NGX output: %.5f %.5f %.5f %.5f\n",
                glPixel[0],glPixel[1],glPixel[2],glPixel[3]);
    if (!(glPixel[0] > .1f && glPixel[0] < .4f &&
          glPixel[1] > .3f && glPixel[1] < .7f &&
          glPixel[2] > .6f && glPixel[2] < 1.1f))
        return 5;

    // Mapping host-visible memory only after the submission completed.
    CHECK(vkQueueWaitIdle(queue));
    void* data = nullptr;
    CHECK(vkMapMemory(device, stagingMemory, 0, bytes, 0, &data));
    // Float16 values in the first pixel; inspect center as an additional
    // guard against an uninitialized output texture.
    auto sample = [&](uint32_t x, uint32_t y) {
        const auto* ptr = static_cast<const unsigned char*>(data) +
                          (VkDeviceSize(y)*ow + x) * 8;
        _Float16 h[4]{};
        std::memcpy(h, ptr, 8);
        std::printf("DLSS pixel[%u,%u]: %.5f %.5f %.5f %.5f\n",
                    x, y, float(h[0]),float(h[1]),float(h[2]),float(h[3]));
        return float(h[0])+float(h[1])+float(h[2]);
    };
    const float topLeft = sample(0, 0);
    const float center = sample(ow/2, oh/2);
    vkUnmapMemory(device, stagingMemory);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    if (!(center > .02f && center < 32.0f)) {
        std::fprintf(stderr, "DLSS output not plausibly reconstructed\n");
        return 5;
    }
    (void)topLeft;

    // All submissions are completed before releasing NGX/image resources.
    sync.destroy();
    sharedColor.destroy();
    sharedOutput.destroy();  // GL resource must be released before Vk memory
    images.destroy();
    vkDestroyCommandPool(device, pool, nullptr);
    backend.shutdown();
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return initialized ? 0 : 3;
}
