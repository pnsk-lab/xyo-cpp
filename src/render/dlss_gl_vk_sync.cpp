#include "dlss_gl_vk_sync.hpp"

#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__)
#include <GL/glext.h>
#include <unistd.h>

namespace xyo::render {
namespace {
template <typename Fn>
Fn resolve(DlssGlVkSync::GetProcAddress get, const char* symbol) {
    return reinterpret_cast<Fn>(get(symbol));
}
} // namespace

DlssGlVkSync::~DlssGlVkSync() { destroy(); }

bool DlssGlVkSync::createPair(VkSemaphore& vkSem, GLuint& glSem) {
    const auto exportFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
    const auto genGl = resolve<PFNGLGENSEMAPHORESEXTPROC>(
        getProc_, "glGenSemaphoresEXT");
    const auto importGl = resolve<PFNGLIMPORTSEMAPHOREFDEXTPROC>(
        getProc_, "glImportSemaphoreFdEXT");
    const auto getError = resolve<GLenum (*)(void)>(
        getProc_, "glGetError");
    if (!exportFd || !genGl || !importGl || !getError) {
        error_ = "Vulkan/GL external semaphore extension functions missing";
        return false;
    }

    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &exportInfo;
    if (vkCreateSemaphore(device_, &createInfo, nullptr, &vkSem) != VK_SUCCESS) {
        error_ = "vkCreateSemaphore external failed";
        return false;
    }
    VkSemaphoreGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = vkSem;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (exportFd(device_, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
        error_ = "vkGetSemaphoreFdKHR failed";
        return false;
    }
    genGl(1, &glSem);
    if (!glSem) {
        ::close(fd);
        error_ = "glGenSemaphoresEXT failed";
        return false;
    }
    importGl(glSem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    if (getError() != GL_NO_ERROR) {
        // The fd is consumed only on successful import.
        ::close(fd);
        error_ = "glImportSemaphoreFdEXT failed";
        return false;
    }
    return true;
}

bool DlssGlVkSync::initialize(VkDevice device, GetProcAddress getProc) {
    destroy();
    if (!device || !getProc) {
        error_ = "Valid Vulkan device and current GL context required";
        return false;
    }
    device_ = device;
    getProc_ = getProc;
    if (!createPair(glToVkVk_, glToVkGl_) ||
        !createPair(vkToGlVk_, vkToGlGl_)) {
        const auto msg = error_;
        destroy();
        error_ = msg;
        return false;
    }
    error_.clear();
    return true;
}

void DlssGlVkSync::destroy() {
    if (getProc_) {
        auto del = resolve<PFNGLDELETESEMAPHORESEXTPROC>(
            getProc_, "glDeleteSemaphoresEXT");
        if (del) {
            if (glToVkGl_) del(1, &glToVkGl_);
            if (vkToGlGl_) del(1, &vkToGlGl_);
        }
    }
    if (device_) {
        if (glToVkVk_) vkDestroySemaphore(device_, glToVkVk_, nullptr);
        if (vkToGlVk_) vkDestroySemaphore(device_, vkToGlVk_, nullptr);
    }
    glToVkVk_ = vkToGlVk_ = VK_NULL_HANDLE;
    glToVkGl_ = vkToGlGl_ = 0;
    getProc_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

bool DlssGlVkSync::signalGlToVk(GLuint texture, GLenum finalLayout) {
    return signalGlToVk(std::span<const GLuint>(&texture, 1),
                        std::span<const GLenum>(&finalLayout, 1));
}

bool DlssGlVkSync::signalGlToVk(std::span<const GLuint> textures,
                              std::span<const GLenum> layouts) {
    if (!valid() || textures.empty() || textures.size() != layouts.size())
        return false;
    for (GLuint texture : textures) if (!texture) return false;
    auto signal = resolve<PFNGLSIGNALSEMAPHOREEXTPROC>(
        getProc_, "glSignalSemaphoreEXT");
    auto flush = resolve<void (*)(void)>(getProc_, "glFlush");
    auto getError = resolve<GLenum (*)(void)>(getProc_, "glGetError");
    if (!signal || !flush || !getError) return false;
    signal(glToVkGl_, 0, nullptr,
           static_cast<GLuint>(textures.size()),
           textures.data(), layouts.data());
    flush(); // ensure submitted GPU signal is observable by Vulkan queue
    if (getError() != GL_NO_ERROR) {
        error_ = "glSignalSemaphoreEXT failed";
        return false;
    }
    return true;
}

bool DlssGlVkSync::waitVkToGl(GLuint texture, GLenum sourceLayout) {
    return waitVkToGl(std::span<const GLuint>(&texture, 1),
                      std::span<const GLenum>(&sourceLayout, 1));
}

bool DlssGlVkSync::waitVkToGl(std::span<const GLuint> textures,
                            std::span<const GLenum> layouts) {
    if (!valid() || textures.empty() || textures.size() != layouts.size())
        return false;
    for (GLuint texture : textures) if (!texture) return false;
    auto wait = resolve<PFNGLWAITSEMAPHOREEXTPROC>(
        getProc_, "glWaitSemaphoreEXT");
    auto getError = resolve<GLenum (*)(void)>(getProc_, "glGetError");
    if (!wait || !getError) return false;
    wait(vkToGlGl_, 0, nullptr,
         static_cast<GLuint>(textures.size()),
         textures.data(), layouts.data());
    if (getError() != GL_NO_ERROR) {
        error_ = "glWaitSemaphoreEXT failed";
        return false;
    }
    return true;
}
} // namespace xyo::render
#endif
