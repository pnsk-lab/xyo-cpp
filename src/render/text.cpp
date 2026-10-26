// xyo-cpp: stb_truetype によるテキスト描画。

#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../third_party/stb_truetype.h"

#include "../core/fileio.hpp"

namespace xyo {

#ifdef XYO_HAVE_EMBEDDED_FONT
// ビルド時に cmake/embed_binary.cmake が生成する（Liberation Sans, OFL）
#include "xyo_fonts.inc"
#endif

namespace {

const char* kFontCandidates[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/System/Library/Fonts/Helvetica.ttc",
};

bool loadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopenUtf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// UTF-16 → UTF-32（サロゲートペア対応）
char32_t nextCp(const std::u16string& s, size_t& i) {
    char32_t cp = s[i++];
    if (cp >= 0xD800 && cp <= 0xDBFF && i < s.size()) {
        char32_t lo = s[i];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            ++i;
            return 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
    }
    return cp;
}

} // namespace

/// TTF/TTC のバイト列から 1 フェイスを初期化する。
bool FontRenderer::initFaceBytes(Face& f, const uint8_t* data, size_t n) {
    if (!data || n == 0) return false;
    f.data.assign(data, data + n);
    for (int idx = 0; idx < 8; ++idx) {
        const int off = stbtt_GetFontOffsetForIndex(f.data.data(), idx);
        if (off < 0) break;
        auto* info = new stbtt_fontinfo;
        std::memset(info, 0, sizeof(*info));
        if (stbtt_InitFont(info, f.data.data(), off)) {
            f.info = info;
            stbtt_GetFontVMetrics(info, &f.ascent, &f.descent, &f.lineGap);
            return true;
        }
        delete info;
    }
    f.data.clear();
    return false;
}

FontRenderer::FontRenderer() {
    // 1) 同梱フォント。Scratch の font-family は
    //    "Helvetica Neue", Helvetica, Arial で、Linux では Arial 系
    //    メトリクスの Liberation Sans に解決される — これを同梱する。
#ifdef XYO_HAVE_EMBEDDED_FONT
    initFaceBytes(faces_[0], xyoFontRegular, xyoFontRegularSize);
    initFaceBytes(faces_[1], xyoFontBold, xyoFontBoldSize);
#endif
    // 2) XYO_FONT は最優先（bold は合成で近似）
    const char* env = std::getenv("XYO_FONT");
    if (env && *env) {
        std::vector<uint8_t> d;
        Face f;
        if (loadFile(env, d) && initFaceBytes(f, d.data(), d.size())) {
            faces_[0] = std::move(f);
            if (faces_[1].info) {
                delete faces_[1].info;
                faces_[1] = Face{};
            }
        }
    }
    // 3) フォールバック（同梱に無いグリフ = 日本語など）は OS フォント
    for (const char* path : kFontCandidates) {
        std::vector<uint8_t> d;
        if (!loadFile(path, d)) continue;
        if (initFaceBytes(faces_[2], d.data(), d.size())) break;
    }
    // 同梱もユーザ指定も無ければシステムフォントを primary に昇格
    if (!faces_[0].ok() && faces_[2].ok()) faces_[0] = std::move(faces_[2]);
    valid_ = faces_[0].ok();
    if (!valid_)
        std::fprintf(stderr,
                     "[xyo] warning: no usable font found; monitor/bubble "
                     "text disabled (set XYO_FONT)\n");
}

FontRenderer::~FontRenderer() {
    for (Face& f : faces_)
        if (f.info) delete f.info;
}

FontRenderer* FontRenderer::get() {
    static FontRenderer inst;
    return &inst;
}

const FontRenderer::Face& FontRenderer::primary() const {
    return faces_[0].ok() ? faces_[0] : faces_[2];
}

float FontRenderer::scaleForPx(int px) const {
    return stbtt_ScaleForPixelHeight(primary().info, static_cast<float>(px));
}

int FontRenderer::lineHeight(int px) const {
    const Face& f = primary();
    return static_cast<int>((f.ascent - f.descent + f.lineGap) *
                                scaleForPx(px) + 0.5f);
}

const FontRenderer::Glyph* FontRenderer::glyph(char32_t cp, int px, bool bold) {
    // フェイス選択: bold(同梱 Bold) → regular → fallback(CJK 等)。
    // どのフェイスにも無ければ regular の '?'。
    auto giOf = [&](int fi, int c) {
        return faces_[fi].ok()
                   ? stbtt_FindGlyphIndex(faces_[fi].info, c)
                   : 0;
    };
    int fi = -1, gi = 0;
    if (bold) { gi = giOf(1, static_cast<int>(cp)); if (gi) fi = 1; }
    if (fi < 0) { gi = giOf(0, static_cast<int>(cp)); if (gi) fi = 0; }
    if (fi < 0) { gi = giOf(2, static_cast<int>(cp)); if (gi) fi = 2; }
    if (fi < 0) {
        fi = faces_[0].ok() ? 0 : 2;
        gi = giOf(fi, '?');
    }
    if (!faces_[fi].ok()) return nullptr;

    uint64_t key = (static_cast<uint64_t>(cp) << 11) |
                   (static_cast<uint64_t>(px & 0x1FF) << 3) |
                   (bold ? 4u : 0u) | static_cast<uint64_t>(fi & 3);
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return &it->second;

    const stbtt_fontinfo* font = faces_[fi].info;
    Glyph g;
    float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(px));
    int adv = 0;
    stbtt_GetGlyphHMetrics(font, gi, &adv, nullptr);
    g.advance = adv * scale;
    uint8_t* bmp = stbtt_GetGlyphBitmap(font, scale, scale, gi, &g.w, &g.h,
                                       &g.xoff, &g.yoff);
    if (bmp && g.w > 0 && g.h > 0) {
        g.alpha.assign(bmp, bmp + static_cast<size_t>(g.w) * g.h);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    // 合成ボールド: アルファを横へ grow px 膨張させ、送りも広げる。
    // テクスチャを 2 回重ねる方式と違い二重像にならない（描画は 1 回）。
    if (bold && fi != 1 && g.w > 0 && g.h > 0) {
        const int grow = std::max(1, static_cast<int>(px / 12.0f + 0.5f));
        const int nw = g.w + grow;
        std::vector<uint8_t> a(static_cast<size_t>(nw) * g.h, 0);
        for (int y = 0; y < g.h; ++y) {
            for (int x = 0; x < g.w; ++x) {
                const uint8_t v = g.alpha[static_cast<size_t>(y) * g.w + x];
                for (int k = 0; k <= grow; ++k) {
                    uint8_t& d = a[static_cast<size_t>(y) * nw + x + k];
                    if (v > d) d = v;
                }
            }
        }
        g.alpha.swap(a);
        g.w = nw;
        g.advance += static_cast<float>(grow);
    }
    auto [res, _] = glyphs_.emplace(key, std::move(g));
    return &res->second;
}

bool FontRenderer::render(const std::u16string& s, int px, uint8_t r,
                          uint8_t g, uint8_t b, std::vector<uint8_t>& out,
                          int& w, int& h, bool bold) {
    if (!valid_ || px <= 0) return false;
    const Face& prim = primary();
    float scale = scaleForPx(px);
    int ascent = static_cast<int>(prim.ascent * scale + 0.5f);
    int descent = static_cast<int>(prim.descent * scale - 0.5f);

    // 1 パス目: 幅計算
    float x = 0;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = nextCp(s, i);
        x += glyph(cp, px, bold)->advance;
    }
    w = static_cast<int>(x + 1);
    h = ascent - descent;
    if (w <= 0 || h <= 0) { w = h = 0; return false; }

    out.assign(static_cast<size_t>(w) * h * 4, 0);
    float pen = 0;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = nextCp(s, i);
        const Glyph* gl = glyph(cp, px, bold);
        int gx = static_cast<int>(pen + 0.5f) + gl->xoff;
        int gy = ascent + gl->yoff;
        for (int yy = 0; yy < gl->h; ++yy) {
            int dy = gy + yy;
            if (dy < 0 || dy >= h) continue;
            for (int xx = 0; xx < gl->w; ++xx) {
                int dx = gx + xx;
                if (dx < 0 || dx >= w) continue;
                uint8_t a = gl->alpha[static_cast<size_t>(yy) * gl->w + xx];
                if (!a) continue;
                size_t o = (static_cast<size_t>(dy) * w + dx) * 4;
                // 単純な上書きブレンド（同一グリフ内の重なりは起きない）
                out[o + 0] = r;
                out[o + 1] = g;
                out[o + 2] = b;
                out[o + 3] = a;
            }
        }
        pen += gl->advance;
    }
    return true;
}

} // namespace xyo
