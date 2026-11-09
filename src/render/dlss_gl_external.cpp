#include "dlss_gl_external.hpp"

#if defined(XYO_NVSDK_DLSS_VK) && defined(__linux__)

#include <GL/glext.h>
#include <unistd.h>
#include <utility>

namespace xyo::render {
namespace {
template <class T>
T proc(DlssGlSharedTexture::GetProcAddress get, const char* name) {
    return reinterpret_cast<T>(get(name));
}
GLenum glFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return GL_RGBA8;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return GL_RGBA16F;
        default: return GL_NONE;
    }
}
} // namespace

DlssGlSharedTexture::~DlssGlSharedTexture() { destroy(); }

void DlssGlSharedTexture::destroy() {
    if (getProc_ && texture_) {
        if (const auto del = proc<void (*)(GLsizei, const GLuint*)>(
                getProc_, "glDeleteTextures"))
            del(1, &texture_);
    }
    if (getProc_ && memory_) {
        if (const auto del = proc<PFNGLDELETEMEMORYOBJECTSEXTPROC>(
                getProc_, "glDeleteMemoryObjectsEXT"))
            del(1, &memory_);
    }
    texture_ = 0;
    memory_ = 0;
    getProc_ = nullptr;
}

bool DlssGlSharedTexture::import(
    const DlssVkFrameTargets& targets, const DlssVkOwnedImage& image,
    GetProcAddress get) {
    destroy();
    error_.clear();
    if (!get || !targets.valid() || !image.exportable || !image.memory ||
        !image.width || !image.height || glFormat(image.format) == GL_NONE) {
        error_ = "Unsupported Vulkan image or missing active GL context";
        return false;
    }

    const auto createMem = proc<PFNGLCREATEMEMORYOBJECTSEXTPROC>(
        get, "glCreateMemoryObjectsEXT");
    const auto memoryParams = proc<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(
        get, "glMemoryObjectParameterivEXT");
    const auto importFd = proc<PFNGLIMPORTMEMORYFDEXTPROC>(
        get, "glImportMemoryFdEXT");
    const auto createTex = proc<PFNGLCREATETEXTURESPROC>(
        get, "glCreateTextures");
    const auto storage = proc<PFNGLTEXTURESTORAGEMEM2DEXTPROC>(
        get, "glTextureStorageMem2DEXT");
    const auto getError = proc<GLenum (*)(void)>(get, "glGetError");
    if (!createMem || !memoryParams || !importFd || !createTex ||
        !storage || !getError) {
        error_ = "OpenGL external memory object functions unavailable";
        return false;
    }

    getProc_ = get;
    int fd = targets.exportOpaqueFd(image);
    if (fd < 0) {
        error_ = "vkGetMemoryFdKHR failed (VK_KHR_external_memory_fd needed)";
        destroy();
        return false;
    }
    // Drain any prior GL errors before checking our imported resources.
    while (getError() != GL_NO_ERROR) {}
    createMem(1, &memory_);
    if (!memory_) {
        ::close(fd);
        error_ = "glCreateMemoryObjectsEXT failed";
        destroy();
        return false;
    }
    const GLint dedicated = GL_TRUE;
    memoryParams(memory_, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    if (getError() != GL_NO_ERROR) {
        ::close(fd);
        error_ = "glMemoryObjectParameterivEXT failed";
        destroy();
        return false;
    }

    importFd(memory_, image.allocationSize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    // GL_EXT_memory_object_fd consumes the file descriptor on successful
    // import; do not close it a second time.
    if (getError() != GL_NO_ERROR) {
        error_ = "glImportMemoryFdEXT rejected Vulkan allocation";
        destroy();
        return false;
    }
    createTex(GL_TEXTURE_2D, 1, &texture_);
    if (!texture_) {
        error_ = "glCreateTextures failed";
        destroy();
        return false;
    }
    storage(texture_, 1, glFormat(image.format),
            image.width, image.height, memory_, 0);
    if (getError() != GL_NO_ERROR) {
        error_ = "glTextureStorageMem2DEXT failed";
        destroy();
        return false;
    }
    return true;
}
} // namespace xyo::render
#endif
