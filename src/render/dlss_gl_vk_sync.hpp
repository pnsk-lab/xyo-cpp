// Linux Vulkan <-> OpenGL external binary semaphore interop.
// Separate from NVIDIA's NGX SDK; only requires Vulkan and a GL context.
#pragma once
#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__)
#include <vulkan/vulkan.h>
#include <GL/gl.h>
#include <span>
#include <string>

namespace xyo::render {

class DlssGlVkSync {
public:
    using GetProcAddress = void* (*)(const char*);
    DlssGlVkSync() = default;
    ~DlssGlVkSync();
    DlssGlVkSync(const DlssGlVkSync&) = delete;
    DlssGlVkSync& operator=(const DlssGlVkSync&) = delete;

    // Requires VK_KHR_external_semaphore_fd enabled on VkDevice and current GL
    // context with GL_EXT_semaphore_fd. The VkDevice must outlive this object.
    bool initialize(VkDevice device, GetProcAddress getProc);
    void destroy();

    // GL writes -> Vulkan reads: signal GL first, flush GL, then submit Vulkan
    // commands waiting on glToVkSemaphore().
    bool signalGlToVk(GLuint texture, GLenum finalLayout);
    bool signalGlToVk(std::span<const GLuint> textures,
                      std::span<const GLenum> layouts);

    // Vulkan signals vkToGlSemaphore() on queue completion; GL waits before
    // reading the shared texture. No CPU fence / glFinish / vkQueueWaitIdle.
    bool waitVkToGl(GLuint texture, GLenum sourceLayout);
    bool waitVkToGl(std::span<const GLuint> textures,
                    std::span<const GLenum> layouts);

    VkSemaphore glToVkSemaphore() const { return glToVkVk_; }
    VkSemaphore vkToGlSemaphore() const { return vkToGlVk_; }
    bool valid() const { return glToVkVk_ && vkToGlVk_; }
    const std::string& lastError() const { return error_; }

private:
    bool createPair(VkSemaphore& semaphoreVk, GLuint& semaphoreGl);
    VkDevice device_ = VK_NULL_HANDLE;
    GetProcAddress getProc_ = nullptr;
    VkSemaphore glToVkVk_ = VK_NULL_HANDLE;
    VkSemaphore vkToGlVk_ = VK_NULL_HANDLE;
    GLuint glToVkGl_ = 0;
    GLuint vkToGlGl_ = 0;
    std::string error_;
};

} // namespace xyo::render
#endif
