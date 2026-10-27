"""TrueType -> SVG path text (no <text>, so every renderer shows the same)."""
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen


class Font:
    def __init__(self, path):
        self.font = TTFont(path)
        self.glyphs = self.font.getGlyphSet()
        self.cmap = self.font.getBestCmap()
        self.hmtx = self.font["hmtx"]
        self.upm = self.font["head"].unitsPerEm

    def advance(self, ch, size):
        name = self.cmap.get(ord(ch))
        if name is None:
            return size * 0.6
        return self.hmtx[name][0] * size / self.upm

    def text_width(self, text, size, letter_spacing=0.0):
        return sum(self.advance(c, size) + letter_spacing for c in text)

    def text_path(self, text, size, x, y, anchor="start", letter_spacing=0.0):
        width = self.text_width(text, size, letter_spacing)
        if anchor == "middle":
            x -= width / 2
        elif anchor == "end":
            x -= width
        parts = []
        penx = x
        scale = size / self.upm
        for ch in text:
            name = self.cmap.get(ord(ch))
            if name is not None:
                pen = SVGPathPen(self.glyphs)
                tpen = TransformPen(pen, (scale, 0, 0, -scale, penx, y))
                self.glyphs[name].draw(tpen)
                d = pen.getCommands()
                if d:
                    parts.append(d)
                penx += self.hmtx[name][0] * scale + letter_spacing
            else:
                penx += size * 0.6 + letter_spacing
        return " ".join(parts), width

    def glyph_path(self, ch, size, x, y):
        name = self.cmap.get(ord(ch))
        if name is None:
            return ""
        pen = SVGPathPen(self.glyphs)
        scale = size / self.upm
        tpen = TransformPen(pen, (scale, 0, 0, -scale, x, y))
        self.glyphs[name].draw(tpen)
        return pen.getCommands()

    def svg_text(self, text, size, x, y, fill, anchor="start",
                 letter_spacing=0.0, extra=""):
        d, _ = self.text_path(text, size, x, y, anchor, letter_spacing)
        if not d:
            return ""
        return (f'<path d="{d}" fill="{fill}" {extra}/>')
