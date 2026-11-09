// Linux GL_EXT_memory_object_fd import for a Vulkan-owned image.
// Requires a current OpenGL context and VK_KHR_external_memory_fd.
#pragma once

#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__)
#include "dlss_vk_images.hpp"
#include <GL/gl.h>
#include <string>

namespace xyo::render {

class DlssGlSharedTexture {
public:
    using GetProcAddress = void* (*)(const char*);

    DlssGlSharedTexture() = default;
    ~DlssGlSharedTexture();
    DlssGlSharedTexture(const DlssGlSharedTexture&) = delete;
    DlssGlSharedTexture& operator=(const DlssGlSharedTexture&) = delete;

    // The image retains ownership of VkDeviceMemory. OpenGL owns the imported
    // memory object reference; Linux OPAQUE_FD is consumed by import.
    // The Vulkan allocation must outlive this GL texture.
    bool import(const DlssVkFrameTargets& targets,
                const DlssVkOwnedImage& image, GetProcAddress getProc);
    void destroy();
    GLuint name() const { return texture_; }
    bool valid() const { return texture_ != 0; }
    const std::string& lastError() const { return error_; }

private:
    GLuint memory_ = 0;
    GLuint texture_ = 0;
    GetProcAddress getProc_ = nullptr;
    std::string error_;
};

} // namespace xyo::render
#endif
