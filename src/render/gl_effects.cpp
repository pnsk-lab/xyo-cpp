#include "gl_effects.hpp"

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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xyo {
namespace {

// Single full-screen triangle. The source SDL GL texture and the destination
// FBO use the same texel orientation, so no CPU-side vertical flip is needed.
constexpr char kVertex[] = R"GLSL(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    vUv = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

constexpr char kFragment[] = R"GLSL(#version 330 core
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uSkin;
uniform vec2 uSize;
// color, fisheye, whirl, pixelate (converted Scratch values)
uniform vec4 uA;
// mosaic, brightness
uniform vec2 uB;

vec3 rgbToHsv(vec3 rgb) {
    float t1x, t1y, t1z, w;
    if (rgb.b > rgb.g) {
        t1x = rgb.b; t1y = rgb.g; t1z = -1.0 / 3.0; w = 2.0 / 3.0;
    } else {
        t1x = rgb.g; t1y = rgb.b; t1z = 0.0; w = -1.0;
    }
    float M, y, z, other;
    if (rgb.r > t1x) {
        M = rgb.r; y = t1y; z = t1z; other = t1x;
    } else {
        M = t1x; y = t1y; z = w; other = rgb.r;
    }
    float C = M - min(y, other);
    return vec3(abs(z + (other - y) / (6.0 * C + 0.001)),
                C / (M + 0.001), M);
}
vec3 hsvToRgb(vec3 hsv) {
    vec3 rgb = clamp(vec3(abs(hsv.x * 6.0 - 3.0) - 1.0,
                          2.0 - abs(hsv.x * 6.0 - 2.0),
                          2.0 - abs(hsv.x * 6.0 - 4.0)), 0.0, 1.0);
    float c = hsv.z * hsv.y;
    return rgb * c + hsv.z - c;
}
void main() {
    vec2 uv = vUv;
    if (uB.x > 1.0) uv = fract(uv * uB.x);
    if (uA.w > 0.001) {
        vec2 n = uSize / uA.w;
        uv = (floor(uv * n) + 0.5) / n;
    }
    if (uA.z != 0.0) {
        vec2 offset = uv - 0.5;
        float factor = max(1.0 - length(offset) / 0.5, 0.0);
        float angle = uA.z * factor * factor;
        float cs = cos(angle), sn = sin(angle);
        uv = vec2(cs * offset.x - sn * offset.y,
                  sn * offset.x + cs * offset.y) + 0.5;
    }
    if (uA.y != 1.0) {
        vec2 v = (uv - 0.5) * 2.0;
        float lengthV = length(v) + 1e-9;
        float radius = pow(min(lengthV, 1.0), uA.y) * max(1.0, lengthV);
        uv = 0.5 + 0.5 * radius * v / lengthV;
    }
    vec4 c = texture(uSkin, clamp(uv, 0.0, 1.0));
    if (uA.x != 0.0) {
        vec3 hsv = rgbToHsv(c.rgb);
        if (hsv.z < 0.055) hsv = vec3(0.0, 1.0, 0.055);
        else if (hsv.y < 0.09) hsv = vec3(0.0, 0.09, hsv.z);
        hsv.x = mod(hsv.x + uA.x, 1.0);
        c.rgb = hsvToRgb(hsv);
    }
    if (uB.y != 0.0) c.rgb = clamp(c.rgb + uB.y, 0.0, 1.0);
    outColor = c;
}
)GLSL";

unsigned compile(unsigned type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    if (std::getenv("XYO_FX_DEBUG")) {
        char error[1024]{};
        glGetShaderInfoLog(shader, sizeof(error), nullptr, error);
        std::fprintf(stderr, "FX shader compile: %s\n", error);
    }
    glDeleteShader(shader);
    return 0;
}

// GL_BindTexture is SDL-specific. Query the real GL name in SDL's context;
// SDL_RenderFlush must precede any raw GL operation touching its textures.
GLuint textureId(SDL_Texture* texture, float* scaleU = nullptr,
                 float* scaleV = nullptr) {
    if (!texture) return 0;
    if (SDL_GL_BindTexture(texture, scaleU, scaleV) != 0) {
        if (std::getenv("XYO_FX_DEBUG"))
            std::fprintf(stderr, "FX bind failed: %s\n", SDL_GetError());
        return 0;
    }
    GLint name = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
    SDL_GL_UnbindTexture(texture);
    return name > 0 ? static_cast<GLuint>(name) : 0;
}

} // namespace

GlEffects::~GlEffects() {
    // The owning SDL renderer/window remains alive while this object dies.
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
}

bool GlEffects::init(SDL_Renderer* renderer) {
    if (initialized_) return supported_;
    initialized_ = true;
    if (std::getenv("XYO_FX_CPU")) return false;

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
            if (std::getenv("XYO_FX_DEBUG")) {
                char error[1024]{};
                glGetProgramInfoLog(program_, sizeof(error), nullptr, error);
                std::fprintf(stderr, "FX shader link: %s\n", error);
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
        uSource_ = glGetUniformLocation(program_, "uSkin");
        uSize_ = glGetUniformLocation(program_, "uSize");
        uFxA_ = glGetUniformLocation(program_, "uA");
        uFxB_ = glGetUniformLocation(program_, "uB");
    }
    glUseProgram(oldProgram);
    glBindVertexArray(oldVao);
    supported_ = program_ && vao_ && fbo_;
    if (std::getenv("XYO_FX_DEBUG"))
        std::fprintf(stderr, "FX OpenGL %d.%d: %s\n", major, minor,
                     supported_ ? "enabled" : "unavailable");
    return supported_;
}

bool GlEffects::available(SDL_Renderer* renderer) {
    return init(renderer);
}

bool GlEffects::apply(SDL_Renderer* renderer, SDL_Texture* source,
                      SDL_Texture* destination, int width, int height,
                      const GraphicEffects& effects) {
    if (!init(renderer) || !source || !destination ||
        source == destination || width <= 0 || height <= 0)
        return false;

    // SDL may have queued drawing commands using these texture objects.
    SDL_RenderFlush(renderer);

    GLint originalActive = 0, originalTex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &originalActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &originalTex);

    float srcU = 0, srcV = 0, dstU = 0, dstV = 0;
    GLuint input = textureId(source, &srcU, &srcV);
    GLuint output = textureId(destination, &dstU, &dstV);
    glBindTexture(GL_TEXTURE_2D, originalTex);
    if (!input || !output || srcU != 1.0f || srcV != 1.0f ||
        dstU != 1.0f || dstV != 1.0f) {
        if (std::getenv("XYO_FX_DEBUG"))
            std::fprintf(stderr, "FX texture input=%u output=%u UV=(%g,%g) (%g,%g) glerr=%x\n",
                input, output, srcU, srcV, dstU, dstV, glGetError());
        glActiveTexture(originalActive);
        return false;
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

    glBindTexture(GL_TEXTURE_2D, input);
    GLint oldMin = 0, oldMag = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &oldMin);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &oldMag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, output, 0);
    const GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const bool ready = fboStatus == GL_FRAMEBUFFER_COMPLETE;
    if (!ready && std::getenv("XYO_FX_DEBUG"))
        std::fprintf(stderr, "FX FBO status: %x glerr=%x\n", fboStatus, glGetError());
    if (ready) {
        const auto get = [&](const char* name) {
            return effects.get(name);
        };
        const float color = std::fmod(get("color") / 200.0, 1.0);
        const float fisheye = std::max(0.0, (get("fisheye") + 100.0) / 100.0);
        const float whirl = -get("whirl") * 3.14159265358979323846 / 180.0;
        const float pixelate = std::abs(get("pixelate")) / 10.0;
        const float mosaic = std::clamp(
            std::round((std::abs(get("mosaic")) + 10.0) / 10.0), 1.0, 512.0);
        const float brightness = std::clamp(get("brightness"), -100.0, 100.0) / 100.0;

        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, width, height);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(program_);
        glBindVertexArray(vao_);
        glUniform1i(uSource_, 0);
        glUniform2f(uSize_, float(width), float(height));
        glUniform4f(uFxA_, color, fisheye, whirl, pixelate);
        glUniform2f(uFxB_, mosaic, brightness);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Restore the state cached by SDL's GL renderer. In particular, restoring
    // its shader/VAO and framebuffer avoids corruption of later SDL draw calls.
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
    glBindTexture(GL_TEXTURE_2D, input);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, oldMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, oldMag);
    glBindTexture(GL_TEXTURE_2D, originalTex);
    glActiveTexture(originalActive);
    return ready;
}

} // namespace xyo

#else
namespace xyo {
GlEffects::~GlEffects() = default;
bool GlEffects::init(SDL_Renderer*) { return false; }
bool GlEffects::available(SDL_Renderer*) { return false; }
bool GlEffects::apply(SDL_Renderer*, SDL_Texture*, SDL_Texture*,
                      int, int, const GraphicEffects&) { return false; }
} // namespace xyo
#endif
