#include "font.hpp"

#include <SDL.h>
#include "../../third_party/stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <unordered_map>
#include <vector>

namespace xyo::ui {
namespace {

// Consume at least one byte, never read beyond the terminating NUL, reject
// overlong encodings, surrogate code points and values above Unicode's limit.
uint32_t nextCodepoint(const char*& text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    const unsigned char first = *p++;
    ++text;
    if (first < 0x80) return first;
    int count;
    uint32_t cp, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 1; cp = first & 31; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 2; cp = first & 15; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 3; cp = first & 7; minimum = 0x10000;
    } else return 0xfffd;
    for (int i = 0; i < count; ++i) {
        if (!*p || (*p & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (*p++ & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0xfffd;
    text = reinterpret_cast<const char*>(p);
    return cp;
}

uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}

// Check the table directory before calling stb. Font data is trusted embedded
// or system data; stb_truetype is not a validator for hostile font files.
bool initFace(stbtt_fontinfo& info, const void* bytes, size_t length) {
    if (!bytes || length < 12) return false;
    const auto* data = static_cast<const unsigned char*>(bytes);
    size_t offset = 0;
    if (be32(data) == 0x74746366u) {
        if (length < 16 || be32(data + 8) == 0) return false;
        offset = be32(data + 12);
    }
    if (offset > length - 12 || offset > 0x7fffffffu) return false;
    const unsigned char* header = data + offset;
    const size_t tables = (size_t(header[4]) << 8) | header[5];
    if (!tables || tables > (length - offset - 12) / 16) return false;
    for (size_t i = 0; i < tables; ++i) {
        const unsigned char* table = header + 12 + i * 16;
        const size_t start = be32(table + 8), size = be32(table + 12);
        if (start > length || size > length - start) return false;
    }
    return stbtt_InitFont(&info, data, static_cast<int>(offset)) != 0;
}

} // namespace

struct Font::Impl {
    struct Glyph {
        int face = 0, index = 0;
        float advance = 0;
        int xoff = 0, yoff = 0, width = 0, height = 0;
        SDL_Texture* texture = nullptr;
        Vec2 uv0{}, uv1{};
    };
    struct Page {
        SDL_Texture* texture = nullptr;
        int width = 0, height = 0, x = 0, y = 0, rowHeight = 0;
    };
    SDL_Renderer* renderer = nullptr;
    stbtt_fontinfo faces[2]{};
    float scales[2]{};
    void* fallbackBytes = nullptr;
    bool fallbackLoaded = false, error = false;
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::vector<Page> pages;

    ~Impl() {
        for (const Page& p : pages) SDL_DestroyTexture(p.texture);
        SDL_free(fallbackBytes);
    }

    Glyph& glyph(uint32_t cp) {
        Glyph g;
        g.index = stbtt_FindGlyphIndex(&faces[0], static_cast<int>(cp));
        if (!g.index && fallbackLoaded) {
            g.index = stbtt_FindGlyphIndex(&faces[1], static_cast<int>(cp));
            if (g.index) g.face = 1;
        }
        if (!g.index) g.index = stbtt_FindGlyphIndex(&faces[0], '?');
        const uint32_t key = uint32_t(g.index) | (uint32_t(g.face) << 31);
        auto found = glyphs.find(key);
        if (found != glyphs.end()) return found->second;
        if (g.index) {
            int advance;
            stbtt_GetGlyphHMetrics(&faces[g.face], g.index, &advance, nullptr);
            // ImGui's baked advance is rounded at the native bake size.
            g.advance = std::floor(advance * scales[g.face] + 0.5f);
            int x1, y1;
            stbtt_GetGlyphBitmapBox(&faces[g.face], g.index, scales[g.face],
                                   scales[g.face], &g.xoff, &g.yoff, &x1, &y1);
            g.width = x1 - g.xoff;
            g.height = y1 - g.yoff;
        }
        return glyphs.emplace(key, g).first->second;
    }

    bool rasterize(Glyph& g) {
        if (g.texture || !g.index || g.width <= 0 || g.height <= 0) return true;
        // Keep already-recorded UVs and texture IDs valid: pages are never
        // resized, repacked, cleared or destroyed until reset().
        const int width = g.width + 2, height = g.height + 2;
        if (width > 4096 || height > 4096) { error = true; return false; }
        Page* page = pages.empty() ? nullptr : &pages.back();
        if (page && page->x + width > page->width) {
            page->x = 0;
            page->y += page->rowHeight;
            page->rowHeight = 0;
        }
        if (!page || width > page->width || page->y + height > page->height) {
            Page fresh;
            fresh.width = std::max(512, width);
            fresh.height = std::max(512, height);
            fresh.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC, fresh.width, fresh.height);
            if (!fresh.texture) { error = true; return false; }
            std::vector<unsigned char> pixels(size_t(fresh.width) * fresh.height * 4, 255);
            for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0;
            if (SDL_SetTextureBlendMode(fresh.texture, SDL_BLENDMODE_BLEND) != 0 ||
                SDL_UpdateTexture(fresh.texture, nullptr, pixels.data(), fresh.width * 4) != 0) {
                SDL_DestroyTexture(fresh.texture); error = true; return false;
            }
#if SDL_VERSION_ATLEAST(2, 0, 12)
            if (SDL_SetTextureScaleMode(fresh.texture, SDL_ScaleModeLinear) != 0) {
                SDL_DestroyTexture(fresh.texture); error = true; return false;
            }
#endif
            pages.push_back(fresh);
            page = &pages.back();
        }
        std::vector<unsigned char> alpha(size_t(g.width) * g.height);
        stbtt_MakeGlyphBitmap(&faces[g.face], alpha.data(), g.width, g.height,
                             g.width, scales[g.face], scales[g.face], g.index);
        std::vector<unsigned char> pixels(size_t(width) * height * 4, 255);
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0;
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
                pixels[(size_t(y + 1) * width + x + 1) * 4 + 3] =
                    alpha[size_t(y) * g.width + x];
        const SDL_Rect rect{page->x, page->y, width, height};
        if (SDL_UpdateTexture(page->texture, &rect, pixels.data(), width * 4) != 0) {
            error = true; return false;
        }
        g.texture = page->texture;
        g.uv0 = {float(page->x + 1) / page->width, float(page->y + 1) / page->height};
        g.uv1 = {float(page->x + 1 + g.width) / page->width,
                 float(page->y + 1 + g.height) / page->height};
        page->x += width;
        page->rowHeight = std::max(page->rowHeight, height);
        return true;
    }
};

Font::Font() noexcept = default;
Font::~Font() { reset(); }
void Font::reset() noexcept {
    delete impl_;
    impl_ = nullptr;
    FontSize = Ascent = 0;
}
bool Font::IsLoaded() const noexcept { return impl_ != nullptr; }
bool Font::HasError() const noexcept { return !impl_ || impl_->error; }

bool Font::init(SDL_Renderer* renderer, const void* ttf, int length, float bakeSize) {
    reset();
    if (!renderer || length <= 0 || !std::isfinite(bakeSize) ||
        bakeSize < 1 || bakeSize > 256) return false;
    auto* impl = new (std::nothrow) Impl;
    if (!impl) return false;
    if (!initFace(impl->faces[0], ttf, static_cast<size_t>(length))) {
        delete impl; return false;
    }
    impl->renderer = renderer;
    impl->scales[0] = stbtt_ScaleForPixelHeight(&impl->faces[0], bakeSize);
    if (!std::isfinite(impl->scales[0]) || impl->scales[0] <= 0) {
        delete impl; return false;
    }
    const char* candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc"
    };
    for (const char* path : candidates) {
        size_t size = 0;
        void* bytes = SDL_LoadFile(path, &size);
        if (!bytes) continue;
        if (initFace(impl->faces[1], bytes, size)) {
            const float scale = stbtt_ScaleForPixelHeight(&impl->faces[1], bakeSize);
            if (std::isfinite(scale) && scale > 0) {
                impl->fallbackBytes = bytes;
                impl->scales[1] = scale;
                impl->fallbackLoaded = true;
                break;
            }
        }
        SDL_free(bytes);
    }
    int ascent;
    stbtt_GetFontVMetrics(&impl->faces[0], &ascent, nullptr, nullptr);
    FontSize = bakeSize;
    Ascent = std::floor(ascent * impl->scales[0] + 0.5f);
    impl_ = impl;
    return true;
}

Vec2 Font::CalcTextSizeA(float size, float maxWidth, float wrapWidth, const char* text) {
    (void)wrapWidth; // Existing callers request unwrapped text.
    if (!impl_ || !text || !std::isfinite(size) || size <= 0) return {0, 0};
    const float scale = size / FontSize;
    float x = 0, width = 0, height = 0;
    while (*text) {
        const uint32_t cp = nextCodepoint(text);
        if (cp == '\r') continue;
        if (cp == '\n') {
            width = std::max(width, x); x = 0; height += size; continue;
        }
        const float advance = impl_->glyph(cp == '\t' ? ' ' : cp).advance *
                              scale * (cp == '\t' ? 4 : 1);
        if (x + advance >= maxWidth) break;
        x += advance;
    }
    width = std::max(width, x);
    if (x > 0 || height == 0) height += size;
    return {width, height};
}

void DrawList::AddText(Font* font, float size, Vec2 position,
                       PackedColor color, const char* text) {
    if (!font || !font->impl_ || !text || !std::isfinite(size) || size <= 0 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) || !(color >> 24)) return;
    const float scale = size / font->FontSize;
    const float startX = std::floor(position.x);
    float x = startX, y = std::floor(position.y);
    while (*text) {
        const uint32_t cp = nextCodepoint(text);
        if (cp == '\r') continue;
        if (cp == '\n') { x = startX; y += size; continue; }
        auto& g = font->impl_->glyph(cp == '\t' ? ' ' : cp);
        if (cp != '\t' && font->impl_->rasterize(g) && g.texture) {
            const Vec2 p0{x + g.xoff * scale,
                          y + (font->Ascent + g.yoff) * scale};
            const Vec2 p1{p0.x + g.width * scale, p0.y + g.height * scale};
            AddImage(reinterpret_cast<TextureID>(g.texture), p0, p1, g.uv0, g.uv1, color);
        }
        x += g.advance * scale * (cp == '\t' ? 4 : 1);
    }
}

} // namespace xyo::ui
