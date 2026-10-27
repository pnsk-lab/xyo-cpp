"""SVG assets for the calculator (all text is converted to paths)."""
import os
import subprocess

from fontsvg import Font

# --- fonts -----------------------------------------------------------------
def _font_path(pattern, fallback):
    try:
        out = subprocess.check_output(["fc-match", "-f", "%{file}", pattern],
                                      text=True).strip()
        if out and os.path.exists(out):
            return out
    except Exception:
        pass
    return fallback


BOLD = _font_path("DejaVu Sans:bold",
                  "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf")
MONO = _font_path("DejaVu Sans Mono",
                  "/usr/share/fonts/TTF/DejaVuSansMono.ttf")

COL = {
    "digit": ("#2a3346", "#f2f7ff", "#3b4862"),
    "op": ("#35291c", "#ffb454", "#4a3a28"),
    "fn": ("#1c3540", "#7fe3ff", "#2b4d5c"),
    "mem": ("#2a2440", "#c9b6ff", "#3d3560"),
    "misc": ("#2a3346", "#c8d4ea", "#3b4862"),
    "del": ("#3a2626", "#ffb0a0", "#543535"),
    "ac": ("#461f24", "#ff8f8f", "#632b32"),
    "eq": ("#1e4633", "#7dffb0", "#2c6448"),
    "graph": ("#1c3540", "#7fe3ff", "#2b4d5c"),
    "type": ("#2a2440", "#c9b6ff", "#3d3560"),
}


def category(label, kind):
    if kind == "eval":
        return "eq"
    if kind == "ac":
        return "ac"
    if kind == "del":
        return "del"
    if kind in ("m+", "m-", "mc", "mr"):
        return "mem"
    if kind == "graph":
        return "graph"
    if kind == "type":
        return "type"
    if label in ("2nd", "DEG", "ANS", "EXP"):
        return "misc"
    if label in ("+", "−", "×", "÷", "^", "x²", "x³", "xʸ", "!", "%"):
        return "op"
    if label in ("0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "."):
        return "digit"
    return "fn"


def label_size(label):
    if label in ("GRAPH", "TYPE", "RESET", "TRACE", "BACK"):
        return 12
    if label in ("2nd", "DEG", "ANS", "EXP", "MC", "MR", "M+", "M−", "DEL", "AC"):
        return 13
    if len(label) >= 4:
        return 13
    if len(label) == 3:
        return 14
    if label in ("x²", "x³", "xʸ", "ʸ√x", "π", "√", "∛", "e"):
        return 15
    return 17


# --- keypad -----------------------------------------------------------------
def keypad_svg(rows):
    f = Font(BOLD)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">']
    out.append('<rect x="0" y="92" width="480" height="268" fill="#161b25"/>')
    out.append('<rect x="0" y="92" width="480" height="2" fill="#2a3346"/>')
    for r, row in enumerate(rows):
        for c, (label, kind, *_) in enumerate(row):
            x = 8 + c * 78
            y = 98 + r * 37
            w, h = 74, 34
            bg, fg, bd = COL[category(label, kind)]
            out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="7" '
                       f'fill="{bg}" stroke="{bd}" stroke-width="1.2"/>')
            out.append(f'<rect x="{x+2}" y="{y+2}" width="{w-4}" height="12" rx="5" '
                       f'fill="#ffffff" fill-opacity="0.035"/>')
            s = label_size(label)
            out.append(f.svg_text(label, s, x + w / 2, y + h / 2 + 5.2, fg,
                                  anchor="middle"))
    out.append('</svg>')
    return '\n'.join(out)


def graphui_svg():
    f = Font(BOLD)
    keys = [
        ("BACK", "#2a3346", "#c8d4ea", "#3b4862"),
        ("f(x)", "#1c3540", "#7fe3ff", "#2b4d5c"),
        ("−", "#35291c", "#ffb454", "#4a3a28"),
        ("+", "#35291c", "#ffb454", "#4a3a28"),
        ("RESET", "#3a2626", "#ffb0a0", "#543535"),
        ("TRACE", "#1e4633", "#7dffb0", "#2c6448"),
    ]
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">']
    out.append('<rect x="0" y="318" width="480" height="42" fill="#161b25"/>')
    out.append('<rect x="0" y="318" width="480" height="2" fill="#2a3346"/>')
    for i, (label, bg, fg, bd) in enumerate(keys):
        x, y, w, h = 8 + i * 78, 322, 74, 30
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" '
                   f'fill="{bg}" stroke="{bd}" stroke-width="1.2"/>')
        out.append(f'<rect x="{x+2}" y="{y+2}" width="{w-4}" height="10" rx="4" '
                   f'fill="#ffffff" fill-opacity="0.035"/>')
        s = 12 if label in ("BACK", "RESET", "TRACE") else (17 if label in ("−", "+") else 13)
        out.append(f.svg_text(label, s, x + w / 2, y + h / 2 + 4.8, fg,
                              anchor="middle"))
    out.append('</svg>')
    return '\n'.join(out)


# --- backdrops --------------------------------------------------------------
def calc_backdrop_svg():
    f = Font(BOLD)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">']
    out.append('<rect width="480" height="360" fill="#0b0e14"/>')
    out.append('<rect x="0" y="0" width="480" height="30" fill="#0d1119"/>')
    out.append(f.svg_text("ULTIMATE SCIENTIFIC CALCULATOR", 12, 16, 20, "#7f95b8",
                          letter_spacing=0.4))
    out.append(f.svg_text("SCRATCH", 9, 390, 20, "#4a5a75",
                          anchor="end", letter_spacing=0.5))
    out.append('<rect x="0" y="29" width="480" height="1" fill="#1a2233"/>')
    out.append('<rect x="14" y="32" width="452" height="54" rx="8" fill="#050b12" '
               'stroke="#1e3350" stroke-width="1.5"/>')
    out.append('<rect x="16" y="34" width="448" height="50" rx="6" fill="none" '
               'stroke="#0d1a2a" stroke-width="1"/>')
    out.append('<rect x="14" y="86" width="452" height="1" fill="#1a2233" fill-opacity="0.6"/>')
    out.append('</svg>')
    return '\n'.join(out)


def graph_backdrop_svg():
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">']
    out.append('<rect width="480" height="360" fill="#080b11"/>')
    out.append('<rect x="0" y="0" width="480" height="30" fill="#0d1119"/>')
    out.append('<rect x="0" y="29" width="480" height="1" fill="#1a2233"/>')
    out.append('<rect x="4" y="30" width="472" height="286" fill="#0a1220" '
               'stroke="#1a2a44" stroke-width="1"/>')
    out.append('<rect x="4" y="316" width="472" height="1" fill="#1a2a44"/>')
    out.append('<rect x="0" y="318" width="480" height="42" fill="#161b25"/>')
    out.append('<rect x="0" y="318" width="480" height="2" fill="#2a3346"/>')
    out.append('</svg>')
    return '\n'.join(out)


# --- glyphs ----------------------------------------------------------------
def glyph_svg(ch, size=16, color="#c8f4ff"):
    f = Font(MONO)
    d = f.glyph_path(ch, size, 1.2, 17.0)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="12" height="22" '
            'viewBox="0 0 12 22">'
            '<rect width="12" height="22" fill="#000" fill-opacity="0"/>'
            f'<path d="{d}" fill="{color}"/></svg>')


# --- small sprites ----------------------------------------------------------
def key_glow_svg():
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="74" height="34" '
            'viewBox="0 0 74 34">'
            '<rect x="1" y="1" width="72" height="32" rx="7" fill="#ffffff" '
            'fill-opacity="0.18" stroke="#7fe3ff" stroke-opacity="0.75" stroke-width="1.4"/>'
            '<rect x="4" y="4" width="66" height="26" rx="5" fill="none" '
            'stroke="#7fe3ff" stroke-opacity="0.18" stroke-width="3"/></svg>')


def crosshair_svg():
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="285" '
            'viewBox="0 0 2 285"><rect width="2" height="285" fill="#7fe3ff" '
            'fill-opacity="0.55"/></svg>')


def dot_svg():
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" '
            'viewBox="0 0 10 10"><circle cx="5" cy="5" r="4" fill="#ffe066" '
            'stroke="#ffffff" stroke-width="1"/></svg>')


def trace_dot_svg():
    # invisible anchor for the trace bubble (the bubble itself is the readout)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2" '
            'viewBox="0 0 2 2"><rect width="2" height="2" fill="#000" '
            'fill-opacity="0"/></svg>')
