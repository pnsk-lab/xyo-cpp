#include "gl_upscale.hpp"

#if defined(XYO_HAVE_SDL2) && defined(XYO_PEN_GL)

#include <SDL.h>

#define GL_GLEXT_PROTOTYPES
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xyo {
namespace {

constexpr char kVertex[] = R"GLSL(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    vUv = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Catmull-Rom reconstruction + unsharp mask + optional temporal blend.
// "DLSS equivalent" here means: run the scene at an internal resolution and
// reconstruct with a high-order filter instead of bilinear stretch. The
// temporal part reuses previous frames, clamped to the local neighborhood so
// that moving sprites do not smear (variance clipping, as in TAA/DLSS).
constexpr char kFragment[] = R"GLSL(#version 330 core
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uSource;
uniform sampler2D uHistory;
uniform vec2 uSrcSize;
uniform float uSharpness;
uniform float uHistoryWeight;

vec4 crWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t,
                 1.5 * t3 - 2.5 * t2 + 1.0,
                -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                 0.5 * t3 - 0.5 * t2);
}

void main() {
    vec2 pos = vUv * uSrcSize - 0.5;
    vec2 base = floor(pos);
    vec2 f = pos - base;
    vec4 wx = crWeights(f.x);
    vec4 wy = crWeights(f.y);
    ivec2 maxc = ivec2(uSrcSize) - 1;
    vec3 acc = vec3(0.0);
    vec3 lo = vec3(1e9);
    vec3 hi = vec3(-1e9);
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            ivec2 c = clamp(ivec2(base) + ivec2(i - 1, j - 1),
                            ivec2(0), maxc);
            vec3 s = texelFetch(uSource, c, 0).rgb;
            float w = wx[i] * wy[j];
            acc += s * w;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    ivec2 b = ivec2(base);
    vec3 s00 = texelFetch(uSource, clamp(b, ivec2(0), maxc), 0).rgb;
    vec3 s10 = texelFetch(uSource, clamp(b + ivec2(1, 0), ivec2(0), maxc), 0).rgb;
    vec3 s01 = texelFetch(uSource, clamp(b + ivec2(0, 1), ivec2(0), maxc), 0).rgb;
    vec3 s11 = texelFetch(uSource, clamp(b + ivec2(1, 1), ivec2(0), maxc), 0).rgb;
    vec3 blur = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
    vec3 c = clamp(acc + uSharpness * (acc - blur), 0.0, 1.0);
    if (uHistoryWeight > 0.0) {
        vec3 h = texture(uHistory, vUv).rgb;
        h = clamp(h, lo, hi);
        c = mix(c, h, uHistoryWeight);
    }
    outColor = vec4(c, 1.0);
}
)GLSL";

unsigned compile(unsigned type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    if (std::getenv("XYO_DLSS_DEBUG")) {
        char error[1024]{};
        glGetShaderInfoLog(shader, sizeof(error), nullptr, error);
        std::fprintf(stderr, "DLSS shader compile: %s\n", error);
    }
    glDeleteShader(shader);
    return 0;
}

// GL_BindTexture is SDL-specific. The real GL name is only reachable while
// SDL's context is current, and SDL_RenderFlush must precede raw GL work.
GLuint textureId(SDL_Texture* texture) {
    if (!texture) return 0;
    if (SDL_GL_BindTexture(texture, nullptr, nullptr) != 0) {
        if (std::getenv("XYO_DLSS_DEBUG"))
            std::fprintf(stderr, "DLSS bind failed: %s\n", SDL_GetError());
        return 0;
    }
    GLint name = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
    SDL_GL_UnbindTexture(texture);
    return name > 0 ? static_cast<GLuint>(name) : 0;
}

} // namespace

GlUpscale::~GlUpscale() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    reset();
}

void GlUpscale::reset() {
    for (SDL_Texture*& t : target_) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
    targetW_ = targetH_ = 0;
    history_ = -1;
}

bool GlUpscale::init(SDL_Renderer* renderer) {
    if (initialized_) return supported_;
    initialized_ = true;
    if (std::getenv("XYO_DLSS_CPU")) return false;

    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name ||
        std::strcmp(info.name, "opengl") != 0 ||
        !SDL_GL_GetCurrentContext()) return false;

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor < 3)) return false;

    GLint oldProgram = 0, oldVao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);

    GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (vs && fs) {
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        GLint linked = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) {
            if (std::getenv("XYO_DLSS_DEBUG")) {
                char error[1024]{};
                glGetProgramInfoLog(program_, sizeof(error), nullptr, error);
                std::fprintf(stderr, "DLSS shader link: %s\n", error);
            }
            glDeleteProgram(program_);
            program_ = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (program_) {
        glGenVertexArrays(1, &vao_);
        glGenFramebuffers(1, &fbo_);
        uSource_ = glGetUniformLocation(program_, "uSource");
        uHistory_ = glGetUniformLocation(program_, "uHistory");
        uSrcSize_ = glGetUniformLocation(program_, "uSrcSize");
        uSharpness_ = glGetUniformLocation(program_, "uSharpness");
        uHistoryWeight_ = glGetUniformLocation(program_, "uHistoryWeight");
    }
    glUseProgram(oldProgram);
    glBindVertexArray(oldVao);
    supported_ = program_ && vao_ && fbo_;
    if (std::getenv("XYO_DLSS_DEBUG"))
        std::fprintf(stderr, "DLSS OpenGL %d.%d: %s\n", major, minor,
                     supported_ ? "enabled" : "unavailable");
    return supported_;
}

bool GlUpscale::available(SDL_Renderer* renderer) {
    return renderer && init(renderer);
}

bool GlUpscale::ensureTargets(SDL_Renderer* renderer, int w, int h) {
    if (target_[0] && target_[1] && targetW_ == w && targetH_ == h) return true;
    reset();
    for (SDL_Texture*& t : target_) {
        t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                              SDL_TEXTUREACCESS_TARGET, w, h);
        if (!t) {
            reset();
            return false;
        }
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    }
    targetW_ = w;
    targetH_ = h;
    return true;
}

SDL_Texture* GlUpscale::apply(SDL_Renderer* renderer, SDL_Texture* source,
                              int outW, int outH, float sharpness,
                              float historyWeight) {
    if (!source || outW <= 0 || outH <= 0) return nullptr;
    if (!init(renderer) || !ensureTargets(renderer, outW, outH))
        return nullptr;

    int srcW = 0, srcH = 0;
    if (SDL_QueryTexture(source, nullptr, nullptr, &srcW, &srcH) != 0 ||
        srcW <= 0 || srcH <= 0)
        return nullptr;

    const bool temporal =
        historyWeight > 0.0f && history_ >= 0 && history_ < 2;
    const int dst = temporal ? (1 - history_) : 0;
    SDL_Texture* destination = target_[dst];
    // Never sample the texture being rendered into (GL feedback loop), even
    // when the temporal branch is disabled by a zero history weight.
    SDL_Texture* historyTex = target_[1 - dst];

    // SDL may have queued draw commands that use these texture objects.
    SDL_RenderFlush(renderer);

    GLint originalActive = 0, originalTex0 = 0, originalTex1 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &originalActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &originalTex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &originalTex1);

    glActiveTexture(GL_TEXTURE0);
    GLuint input = textureId(source);
    glActiveTexture(GL_TEXTURE1);
    GLuint history = textureId(historyTex);
    glActiveTexture(GL_TEXTURE0);
    GLuint output = textureId(destination);
    if (!input || !output || (temporal && !history)) {
        if (std::getenv("XYO_DLSS_DEBUG"))
            std::fprintf(stderr,
                         "DLSS texture input=%u history=%u output=%u\n",
                         input, history, output);
        glBindTexture(GL_TEXTURE_2D, originalTex0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, originalTex1);
        glActiveTexture(originalActive);
        return nullptr;
    }

    GLint originalFbo = 0, originalProgram = 0, originalVao = 0;
    GLint originalViewport[4]{}, originalDrawBuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &originalFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &originalProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &originalVao);
    glGetIntegerv(GL_VIEWPORT, originalViewport);
    glGetIntegerv(GL_DRAW_BUFFER, &originalDrawBuffer);
    const bool wasBlend = glIsEnabled(GL_BLEND) == GL_TRUE;
    const bool wasScissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool wasDepth = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool wasCull = glIsEnabled(GL_CULL_FACE) == GL_TRUE;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, history);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, output, 0);
    const GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const bool ready = fboStatus == GL_FRAMEBUFFER_COMPLETE;
    if (!ready && std::getenv("XYO_DLSS_DEBUG"))
        std::fprintf(stderr, "DLSS FBO status: %x glerr=%x\n", fboStatus,
                     glGetError());
    if (ready) {
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, outW, outH);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(program_);
        glBindVertexArray(vao_);
        glUniform1i(uSource_, 0);
        glUniform1i(uHistory_, 1);
        glUniform2f(uSrcSize_, float(srcW), float(srcH));
        glUniform1f(uSharpness_, std::max(0.0f, sharpness));
        glUniform1f(uHistoryWeight_,
                    temporal ? std::clamp(historyWeight, 0.0f, 0.95f)
                             : 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindVertexArray(originalVao);
    glUseProgram(originalProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, originalFbo);
    glDrawBuffer(originalDrawBuffer);
    glViewport(originalViewport[0], originalViewport[1],
               originalViewport[2], originalViewport[3]);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, originalTex0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, originalTex1);
    glActiveTexture(originalActive);
    if (!ready) return nullptr;

    history_ = dst;
    if (std::getenv("XYO_DLSS_DEBUG")) {
        // Once per second would need a clock; the caller already has stats.
        static int frames = 0;
        if (++frames % 120 == 0)
            std::fprintf(stderr, "DLSS %dx%d -> %dx%d temporal=%d\n",
                         targetW_, targetH_, outW, outH, int(temporal));
    }
    return destination;
}

} // namespace xyo

#else
namespace xyo {
GlUpscale::~GlUpscale() = default;
void GlUpscale::reset() {}
bool GlUpscale::init(SDL_Renderer*) { return false; }
bool GlUpscale::available(SDL_Renderer*) { return false; }
bool GlUpscale::ensureTargets(SDL_Renderer*, int, int) { return false; }
SDL_Texture* GlUpscale::apply(SDL_Renderer*, SDL_Texture*, int, int, float,
                              float) {
    return nullptr;
}
} // namespace xyo
#endif
