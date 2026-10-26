#include "ui/font.hpp"

#include <SDL.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#ifndef NATIVE_UI_TEST_FONT_PATH
#error Define NATIVE_UI_TEST_FONT_PATH as the absolute LiberationSans-Regular.ttf path
#endif

using namespace xyo::ui;

// Keep regression checks active in Release builds as well.
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s failed (SDL: %s)\n", \
                 __FILE__, __LINE__, #condition, SDL_GetError()); \
    std::abort(); \
} } while (false)

namespace {
constexpr int width = 160, height = 80;

std::string utf8(unsigned cp) {
    std::string result;
    if (cp < 0x80) result += char(cp);
    else if (cp < 0x800) {
        result += char(0xc0 | (cp >> 6));
        result += char(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        result += char(0xe0 | (cp >> 12));
        result += char(0x80 | ((cp >> 6) & 63));
        result += char(0x80 | (cp & 63));
    } else {
        result += char(0xf0 | (cp >> 18));
        result += char(0x80 | ((cp >> 12) & 63));
        result += char(0x80 | ((cp >> 6) & 63));
        result += char(0x80 | (cp & 63));
    }
    return result;
}

void clear(SDL_Renderer* renderer) {
    CHECK(SDL_RenderSetClipRect(renderer, nullptr) == 0);
    CHECK(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) == 0);
    CHECK(SDL_RenderClear(renderer) == 0);
}

std::vector<unsigned char> snapshot(SDL_Renderer* renderer) {
    std::vector<unsigned char> pixels(width * height * 4);
    CHECK(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                               pixels.data(), width * 4) == 0);
    return pixels;
}

void metrics(Font& font) {
    CHECK(font.IsLoaded() && !font.HasError());
    CHECK(font.FontSize == 32 && font.Ascent > 0 && font.Ascent < font.FontSize);
    // Metrics must work immediately, before any draw or atlas upload.
    const Vec2 a = font.CalcTextSizeA(16, FLT_MAX, 0, "Hello world");
    const Vec2 b = font.CalcTextSizeA(32, FLT_MAX, 0, "Hello world");
    CHECK(a.x > 0 && a.y == 16 && b.x == a.x * 2 && b.y == 32);
    CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, "").y == 16);
    CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, "x\nx").y == 32);
    CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, "x\r\nx").y == 32);
    CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, "\t").x ==
          font.CalcTextSizeA(16, FLT_MAX, 0, "    ").x);
    CHECK(font.CalcTextSizeA(16, 1, 0, "Hello").x == 0);
    CHECK(font.CalcTextSizeA(0, FLT_MAX, 0, "Hello").x == 0);
    CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, nullptr).x == 0);
    CHECK(font.CalcTextSizeA(std::numeric_limits<float>::quiet_NaN(),
                              FLT_MAX, 0, "Hello").x == 0);
    // Unicode noncharacters must resolve to the shared '?' fallback or skip.
    const Vec2 missing = font.CalcTextSizeA(16, FLT_MAX, 0, utf8(0x10ffff).c_str());
    const Vec2 fallback = font.CalcTextSizeA(16, FLT_MAX, 0, "?");
    CHECK(missing.x == fallback.x || missing.x == 0);
}

void unicode(Font& font, SDL_Renderer* renderer) {
    const char* samples[] = {
        "日本語 café \xf0\x9f\x98\x80",
        "\x80", "\xff", "\xc0\xaf", "\xed\xa0\x80", // invalid/surrogate
        "\xf4\x90\x80\x80", "\xf0\x80\x80\x80", // out of range/overlong
        "\xc2", "\xe3\x81", "\xf0\x9f\x98", // truncated at NUL
        "\xe3" "A", "\xf0\x9f" "B"
    };
    DrawList draw;
    for (const char* text : samples) {
        const Vec2 size = font.CalcTextSizeA(20, FLT_MAX, 0, text);
        CHECK(std::isfinite(size.x) && size.x >= 0 && size.y == 20);
        draw.begin(width, height);
        draw.AddText(&font, 20, {4, 4}, rgba(255, 255, 255), text);
        clear(renderer);
        CHECK(draw.render(renderer));
        CHECK(!font.HasError());
    }
}

void persistentAtlas(Font& font, SDL_Renderer* renderer) {
    DrawList retained;
    retained.begin(width, height);
    retained.AddText(&font, 32, {8, 8}, rgba(255, 255, 255), "Hello");
    CHECK(retained.getVertexCount() > 0);
    clear(renderer);
    CHECK(retained.render(renderer));
    const auto before = snapshot(renderer);
    size_t lit = 0, transparentBackground = 0;
    for (size_t i = 0; i < before.size(); i += 4) {
        if (before[i] || before[i + 1] || before[i + 2]) ++lit;
        else ++transparentBackground;
    }
    CHECK(lit > 100 && transparentBackground > 100);

    // Populate far more than one page while the first draw list retains its
    // original atlas texture/UVs. Only Liberation Sans is required for paging;
    // additional system CJK coverage is optional and changes the page count.
    DrawList many;
    many.begin(width, height);
    for (unsigned cp = 33; cp < 14000; ++cp) {
        const auto text = utf8(cp);
        many.AddText(&font, 16, {8, 8}, rgba(255, 255, 255), text.c_str());
    }
    CHECK(!font.HasError());
    const DrawStats first = many.stats();
    CHECK(first.vertices > 4000 && first.batches > 1);
    clear(renderer);
    CHECK(retained.render(renderer));
    CHECK(snapshot(renderer) == before); // old glyphs survived page growth

    // A second pass must reuse every cached glyph and preserve page/batch
    // identity. No string-specific texture may be created on repeated draws.
    many.begin(width, height);
    for (unsigned cp = 33; cp < 14000; ++cp) {
        const auto text = utf8(cp);
        many.AddText(&font, 16, {8, 8}, rgba(255, 255, 255), text.c_str());
    }
    const DrawStats second = many.stats();
    CHECK(second.vertices == first.vertices && second.indices == first.indices &&
          second.batches == first.batches && !font.HasError());
    clear(renderer);
    CHECK(many.render(renderer));
    CHECK(!font.HasError());
    std::printf("font atlas: %zu vertices, %zu stable texture batches\n",
                first.vertices, first.batches);
}
} // namespace

int main() {
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    CHECK(surface);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    CHECK(renderer);
    size_t length = 0;
    void* bytes = SDL_LoadFile(NATIVE_UI_TEST_FONT_PATH, &length);
    CHECK(bytes && length > 12 && length <= size_t(std::numeric_limits<int>::max()));
    {
        Font font;
        CHECK(!font.IsLoaded());
        CHECK(!font.init(nullptr, bytes, int(length)));
        CHECK(!font.init(renderer, nullptr, int(length)));
        CHECK(!font.init(renderer, bytes, 4));
        CHECK(!font.init(renderer, bytes, int(length), 0));
        CHECK(!font.init(renderer, bytes, int(length),
                          std::numeric_limits<float>::infinity()));
        CHECK(!font.init(renderer, bytes, int(length), 257));
        CHECK(font.init(renderer, bytes, int(length)));
        metrics(font);
        unicode(font, renderer);
        persistentAtlas(font, renderer);
        CHECK(SDL_RenderFlush(renderer) == 0);
        font.reset();
        CHECK(!font.IsLoaded() && font.FontSize == 0 && font.Ascent == 0);
        CHECK(font.CalcTextSizeA(16, FLT_MAX, 0, "unloaded").x == 0);
        font.reset(); // idempotent
        CHECK(font.init(renderer, bytes, int(length), 24));
        CHECK(font.FontSize == 24 && font.Ascent > 0 && !font.HasError());
        DrawList draw;
        draw.begin(width, height);
        draw.AddText(&font, 24, {4, 4}, rgba(255, 255, 255), "Destructor cleanup");
        clear(renderer);
        CHECK(draw.render(renderer));
        CHECK(SDL_RenderFlush(renderer) == 0);
        // Font destructor releases the reinitialized atlas before the renderer.
    }
    SDL_free(bytes);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    SDL_Quit();
    std::puts("native font tests passed");
}
