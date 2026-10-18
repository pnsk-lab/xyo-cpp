// xyo-cpp: stb_truetype によるテキスト描画。
// SDL_ttf が無い環境向けの最小フォントレンダラ。TTC（NotoSansCJK 等）対応。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace xyo {

/// UTF-16 文字列 → RGBA8 ビットマップのレンダラ。
/// スレッドセーフではない（描画スレッドのみで使用）。
class FontRenderer {
public:
    /// 遅延シングルトン。フォント探索順: $XYO_FONT → 既知パス。
    /// 見つからなければ ok()==false で render は失敗を返す。
    static FontRenderer* get();

    bool ok() const { return valid_; }

    /// UTF-16 → RGBA8 (premult なし)。px は縦ピクセル高さ。
    /// bold は合成ボールド（アルファを横に膨張 + 送りを拡大）。
    /// 成功時 true を返し out に w*h*4 バイトを書き込む。
    bool render(const std::u16string& s, int px, uint8_t r, uint8_t g,
                uint8_t b, std::vector<uint8_t>& out, int& w, int& h,
                bool bold = false);

    /// 行の高さ（ascent-descent+linegap を px スケール）。
    int lineHeight(int px) const;

private:
    FontRenderer();
    ~FontRenderer();

    struct Glyph {
        int w = 0, h = 0;
        int xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> alpha;
    };

    /// 1 フォントフェイス。data を所有し、stb はその先頭を参照する。
    /// faces_[0]=regular（同梱 Liberation / $XYO_FONT）、
    /// faces_[1]=bold（同梱）、faces_[2]=fallback（システムの CJK 等）。
    struct Face {
        std::vector<uint8_t> data;
        struct stbtt_fontinfo* info = nullptr;
        int ascent = 0, descent = 0, lineGap = 0;
        bool ok() const { return info != nullptr; }
    };

    static bool initFaceBytes(Face& f, const uint8_t* data, size_t n);
    const Glyph* glyph(char32_t cp, int px, bool bold);
    float scaleForPx(int px) const;
    const Face& primary() const;

    Face faces_[3];
    bool valid_ = false;
    std::unordered_map<uint64_t, Glyph> glyphs_;
};

} // namespace xyo
