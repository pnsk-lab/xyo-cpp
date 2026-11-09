#include "gl_fsr_pen.hpp"

#if defined(XYO_HAVE_SDL2) && defined(XYO_PEN_GL)

#include "fsr1_embedded.hpp"

#include <SDL.h>
#define GL_GLEXT_PROTOTYPES
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace xyo {
namespace {

constexpr char kVertex[] = R"GLSL(#version 420 core
void main() {
    vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                    gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

constexpr char kHeaderEasu[] =
    "#version 420 core\n#define A_GPU 1\n#define A_GLSL 1\n"
    "#define FSR_EASU_F 1\n";
constexpr char kHeaderRcas[] =
    "#version 420 core\n#define A_GPU 1\n#define A_GLSL 1\n"
    "#define FSR_RCAS_F 1\n";

bool debug() {
    return std::getenv("XYO_PEN_FSR_DEBUG") != nullptr;
}

GLuint compile(GLenum kind, const char* const* parts, int n) {
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, n, parts, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    if (debug()) {
        char log[8192] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "FSR pen shader compilation: %s\n", log);
    }
    glDeleteShader(shader);
    return 0;
}

GLuint makeProgram(const char* header, const char* fragment) {
    const char* vertexSources[] = {kVertex};
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSources, 1);
    const char* sources[] = {
        header, fsr_embedded::kFfxA, fsr_embedded::kFfxFsr1, fragment
    };
    GLuint fs = compile(GL_FRAGMENT_SHADER, sources, 4);
    GLuint program = 0;
    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            if (debug()) {
                char log[8192] = {};
                glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                std::fprintf(stderr, "FSR pen program linking: %s\n", log);
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return program;
}

GLuint textureId(SDL_Texture* tex) {
    if (!tex || SDL_GL_BindTexture(tex, nullptr, nullptr) != 0)
        return 0;
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    SDL_GL_UnbindTexture(tex);
    return id > 0 ? static_cast<GLuint>(id) : 0;
}

unsigned bits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

} // namespace

GlFsrPen::~GlFsrPen() {
    reset();
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    for (unsigned program : programs_)
        if (program) glDeleteProgram(program);
}

void GlFsrPen::reset() {
    if (easuTarget_) SDL_DestroyTexture(easuTarget_);
    if (outputTarget_) SDL_DestroyTexture(outputTarget_);
    easuTarget_ = outputTarget_ = nullptr;
    outputWidth_ = outputHeight_ = 0;
    cached_ = false;
    cachedSource_ = cachedOutput_ = nullptr;
}

bool GlFsrPen::initialize(SDL_Renderer* renderer) {
    if (initialized_) return supported_;
    initialized_ = true;
    SDL_RendererInfo info{};
    if (!renderer || SDL_GetRendererInfo(renderer, &info) != 0 ||
        !info.name || std::strcmp(info.name, "opengl") != 0 ||
        !SDL_GL_GetCurrentContext()) return false;

    // Spatial reconstruction at multi-megapixel resolutions is prohibitively
    // slow on CPU OpenGL (e.g. SDL offscreen + llvmpipe). Fail closed instead
    // of regressing interactive projects to sub-1 FPS. Explicit override is
    // available for offline visual testing.
    const char* rendererName =
        reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (!std::getenv("XYO_PEN_FSR_ALLOW_SOFTWARE") && rendererName &&
        (std::strstr(rendererName, "llvmpipe") ||
         std::strstr(rendererName, "softpipe") ||
         std::strstr(rendererName, "Software Rasterizer") ||
         std::strstr(rendererName, "SwiftShader"))) {
        if (debug())
            std::fprintf(stderr, "FSR pen: software OpenGL (%s); using native display\n",
                         rendererName);
        return false;
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    // FSR EASU needs textureGather (GLSL 4.00 core).
    if (major < 4 || (major == 4 && minor < 2)) {
        if (debug()) std::fprintf(stderr,
            "FSR pen requires GL 4.2, found %d.%d\n", major, minor);
        return false;
    }

    GLint prevProgram = 0, prevVao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    programs_[0] = makeProgram(kHeaderEasu, fsr_embedded::kEasu);
    programs_[1] = makeProgram(kHeaderRcas, fsr_embedded::kRcas);
    if (programs_[0] && programs_[1]) {
        glGenVertexArrays(1, &vao_);
        glGenFramebuffers(1, &fbo_);
    }
    glUseProgram(prevProgram);
    glBindVertexArray(prevVao);
    supported_ = programs_[0] && programs_[1] && vao_ && fbo_;
    if (debug()) std::fprintf(stderr, "FSR pen GL %d.%d %s\n",
                               major, minor, supported_ ? "enabled" : "unavailable");
    return supported_;
}

bool GlFsrPen::available(SDL_Renderer* renderer) {
    return initialize(renderer);
}

bool GlFsrPen::ensureTargets(SDL_Renderer* renderer, int width, int height) {
    if (easuTarget_ && outputTarget_ &&
        width == outputWidth_ && height == outputHeight_) return true;
    reset();
    if (width < 1 || height < 1) return false;
    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(renderer, &info) != 0 ||
        (info.max_texture_width > 0 && width > static_cast<int>(info.max_texture_width)) ||
        (info.max_texture_height > 0 && height > static_cast<int>(info.max_texture_height)))
        return false;
    easuTarget_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_TARGET, width, height);
    outputTarget_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET, width, height);
    if (!easuTarget_ || !outputTarget_) {
        reset();
        return false;
    }
    // EASU may be the final output when RCAS is disabled.
    // Keep alpha compositing correct over the backdrop.
    SDL_SetTextureBlendMode(easuTarget_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(outputTarget_, SDL_BLENDMODE_BLEND);
    outputWidth_ = width;
    outputHeight_ = height;
    return true;
}

SDL_Texture* GlFsrPen::apply(SDL_Renderer* renderer, SDL_Texture* source,
                            int outputWidth, int outputHeight,
                            bool sourcePremultiplied, float rcasSharpness,
                            bool sourceChanged, bool enableRcas) {
    if (!renderer || !source || outputWidth < 1 || outputHeight < 1 ||
        !available(renderer) ||
        !ensureTargets(renderer, outputWidth, outputHeight))
        return nullptr;
    // Static pen often persists for hundreds of screen frames. Reuse the
    // previous output unless its source, options or dimensions have changed.
    if (!sourceChanged && cached_ && cachedSource_ == source &&
        cachedPremult_ == sourcePremultiplied && cachedRcas_ == enableRcas &&
        cachedSharpness_ == rcasSharpness)
        return cachedOutput_;
    int srcWidth = 0, srcHeight = 0;
    if (SDL_QueryTexture(source, nullptr, nullptr, &srcWidth, &srcHeight) != 0 ||
        srcWidth < 1 || srcHeight < 1 ||
        outputWidth < srcWidth || outputHeight < srcHeight)
        return nullptr;

    // Complete pending SDL work before raw GL writes/reads to its textures.
    SDL_RenderFlush(renderer);
    GLint prevActive = 0, prevTex0 = 0, prevFramebuffer = 0;
    GLint prevProgram = 0, prevVao = 0, prevDrawBuffer = 0;
    GLint prevViewport[4]{};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
    GLuint input = textureId(source);
    GLuint easu = textureId(easuTarget_);
    GLuint output = textureId(outputTarget_);
    glBindTexture(GL_TEXTURE_2D, prevTex0);
    if (!input || !easu || !output) {
        glActiveTexture(prevActive);
        return nullptr;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_DRAW_BUFFER, &prevDrawBuffer);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull = glIsEnabled(GL_CULL_FACE);

    glBindTexture(GL_TEXTURE_2D, input);
    GLint oldMin = 0, oldMag = 0, oldWrapS = 0, oldWrapT = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &oldMin);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &oldMag);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &oldWrapS);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &oldWrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, outputWidth, outputHeight);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(vao_);

    const float inW = static_cast<float>(srcWidth);
    const float inH = static_cast<float>(srcHeight);
    const float outW = static_cast<float>(outputWidth);
    const float outH = static_cast<float>(outputHeight);
    const float rx = inW / outW;
    const float ry = inH / outH;
    const float invX = 1.0f / inW;
    const float invY = 1.0f / inH;
    const unsigned con0[4] = {bits(rx), bits(ry),
                             bits(0.5f * rx - 0.5f), bits(0.5f * ry - 0.5f)};
    const unsigned con1[4] = {bits(invX), bits(invY),
                             bits(invX), bits(-invY)};
    const unsigned con2[4] = {bits(-invX), bits(2.0f * invY),
                             bits(invX), bits(2.0f * invY)};
    const unsigned con3[4] = {bits(0.0f), bits(4.0f * invY), 0, 0};
    auto uniform = [](unsigned prog, const char* name) {
        return glGetUniformLocation(prog, name);
    };

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, easu, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        GLuint prog = programs_[0];
        glUseProgram(prog);
        glBindTexture(GL_TEXTURE_2D, input);
        glUniform1i(uniform(prog, "uSource"), 0);
        glUniform2i(uniform(prog, "uInputSize"), srcWidth, srcHeight);
        glUniform2i(uniform(prog, "uOutputSize"), outputWidth, outputHeight);
        glUniform1i(uniform(prog, "uSourcePremult"), sourcePremultiplied ? 1 : 0);
        glUniform1i(uniform(prog, "uStraightOutput"), enableRcas ? 0 : 1);
        glUniform4uiv(uniform(prog, "uCon0"), 1, con0);
        glUniform4uiv(uniform(prog, "uCon1"), 1, con1);
        glUniform4uiv(uniform(prog, "uCon2"), 1, con2);
        glUniform4uiv(uniform(prog, "uCon3"), 1, con3);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (enableRcas) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, output, 0);
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (ok && enableRcas) {
            prog = programs_[1];
            glUseProgram(prog);
            glBindTexture(GL_TEXTURE_2D, easu);
            glUniform1i(uniform(prog, "uSource"), 0);
            glUniform2i(uniform(prog, "uOutputSize"), outputWidth, outputHeight);
            const unsigned sharp[4] = {
                bits(std::exp2(-std::clamp(rcasSharpness, 0.0f, 2.0f))), 0, 0, 0
            };
            glUniform4uiv(uniform(prog, "uCon"), 1, sharp);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    // Reestablish GL state tracked by SDL's renderer.
    glBindFramebuffer(GL_FRAMEBUFFER, prevFramebuffer);
    glDrawBuffer(prevDrawBuffer);
    glBindVertexArray(prevVao);
    glUseProgram(prevProgram);
    glViewport(prevViewport[0], prevViewport[1],
               prevViewport[2], prevViewport[3]);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, input);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, oldMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, oldMag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, oldWrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, oldWrapT);
    glBindTexture(GL_TEXTURE_2D, prevTex0);
    glActiveTexture(prevActive);
    if (!ok && debug()) std::fprintf(stderr, "FSR pen: incomplete GL FBO\n");
    if (!ok) {
        cached_ = false;
        return nullptr;
    }
    cached_ = true;
    cachedSource_ = source;
    cachedPremult_ = sourcePremultiplied;
    cachedRcas_ = enableRcas;
    cachedSharpness_ = rcasSharpness;
    cachedOutput_ = enableRcas ? outputTarget_ : easuTarget_;
    return cachedOutput_;
}

} // namespace xyo

#else

namespace xyo {
GlFsrPen::~GlFsrPen() = default;
void GlFsrPen::reset() {}
bool GlFsrPen::initialize(SDL_Renderer*) { return false; }
bool GlFsrPen::available(SDL_Renderer*) { return false; }
bool GlFsrPen::ensureTargets(SDL_Renderer*, int, int) { return false; }
SDL_Texture* GlFsrPen::apply(SDL_Renderer*, SDL_Texture*, int, int, bool, float, bool, bool) {
    return nullptr;
}
} // namespace xyo

#endif
