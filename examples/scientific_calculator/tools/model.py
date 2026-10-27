"""Shared variable/list model and keypad layout."""
from sb3 import Model

# ------------------------------------------------------------------ keypad
NORMAL = [
    [("2nd", "inv"), ("DEG", "mode"), ("MC", "mc"), ("MR", "mr"), ("M+", "m+"), ("M−", "m-")],
    [("sin", "ins", "sin("), ("cos", "ins", "cos("), ("tan", "ins", "tan("),
     ("ln", "ins", "ln("), ("log", "ins", "log("), ("√", "ins", "sqrt(")],
    [("x²", "ins", "^2"), ("xʸ", "ins", "^"), ("(", "ins", "("), (")", "ins", ")"),
     ("π", "ins", "pi"), ("e", "ins", "e")],
    [("7", "ins", "7"), ("8", "ins", "8"), ("9", "ins", "9"), ("÷", "ins", "/"),
     ("DEL", "del"), ("AC", "ac")],
    [("4", "ins", "4"), ("5", "ins", "5"), ("6", "ins", "6"), ("×", "ins", "*"),
     ("!", "ins", "!"), ("%", "ins", "%")],
    [("1", "ins", "1"), ("2", "ins", "2"), ("3", "ins", "3"), ("−", "ins", "-"),
     ("ANS", "ans"), ("GRAPH", "graph")],
    [("0", "ins", "0"), (".", "ins", "."), ("EXP", "ins", "e"), ("+", "ins", "+"),
     ("=", "eval"), ("TYPE", "type")],
]
SECOND = [list(row) for row in NORMAL]
SECOND[0] = [("2nd", "inv"), ("DEG", "mode"), ("MC", "mc"), ("MR", "mr"), ("M+", "m+"), ("M−", "m-")]
SECOND[1] = [("asin", "ins", "asin("), ("acos", "ins", "acos("), ("atan", "ins", "atan("),
             ("e^x", "ins", "exp("), ("10^x", "ins", "10^("), ("∛", "ins", "cbrt(")]
SECOND[2] = [("x³", "ins", "^3"), ("ʸ√x", "ins", "root("), ("(", "ins", "("), (")", "ins", ")"),
             ("i", "ins", "i"), ("j", "ins", "j")]
SECOND[5] = [("1", "ins", "1"), ("2", "ins", "2"), ("3", "ins", "3"), ("−", "ins", "-"),
             ("k", "ins", "k"), ("GRAPH", "graph")]
SECOND[4] = [("4", "ins", "4"), ("5", "ins", "5"), ("6", "ins", "6"), ("×", "ins", "*"),
             ("nCr", "ins", "ncr("), ("nPr", "ins", "npr(")]

GRAPH_KEYS = [
    ("BACK", "back"), ("f(x)", "ask"), ("−", "zoomout"),
    ("+", "zoomin"), ("RESET", "reset"), ("TRACE", "trace"),
]

GLYPH_CHARS = [chr(c) for c in range(33, 127)] + ["×", "÷", "−"]
GLYPH_NAMES = ["g%d" % ord(ch) for ch in GLYPH_CHARS]

FROM_CHARS = ["π", "×", "÷", "−", "–", "—", "√", "∛", "²", "³",
              "！", "％", "（", "）", "，", "、", "。", "　",
              "＋", "－", "＊", "／", "＾", "＝", "．",
              "０", "１", "２", "３", "４", "５", "６", "７", "８", "９",
              "τ", "φ"]
TO_STRS = ["pi", "*", "/", "-", "-", "-", "sqrt", "cbrt", "^2", "^3",
           "!", "%", "(", ")", ",", ",", ".", " ",
           "+", "-", "*", "/", "^", "=", ".",
           "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
           "tau", "phi"]

LANCZOS = [
    0.99999999999980993,
    676.5203681218851,
    -1259.1392167224028,
    771.32342877765313,
    -176.61502916214059,
    12.507343278686905,
    -0.13857109526572012,
    9.9843695780195716e-6,
    1.5056327351493116e-7,
]


def build_model():
    m = Model()
    # UI / parser state
    for name, init in [
        ("expr", ""), ("ans", 0), ("mem", 0),
        ("angleMode", "DEG"), ("screen", "calc"), ("inv", 0),
        ("graphExpr", "sin(x)"), ("scale", 20),
        ("traceOn", 0), ("traceX", 0), ("traceY", 0), ("traceValid", 0),
        ("traceText", ""),
        ("err", 0), ("errMsg", ""), ("result", 0),
        ("previewVal", 0), ("previewOk", 0), ("justEval", 0),
        ("statusText", ""), ("exprText", "0"), ("resultText", ""),
        ("src", ""), ("raw", ""), ("normOut", ""),
        ("i", 0), ("n", 0), ("ch", ""), ("prev", "none"),
        ("numStr", ""), ("id", ""), ("numVal", 0), ("pOut", 0),
        ("popping", 0), ("pv", 0), ("a", 0), ("b", 0), ("r", 0),
        ("fn", 0), ("fk", 0), ("fr", 0), ("fOut", 0),
        ("gx", 0), ("gz", 0), ("ga", 0), ("gt", 0), ("gi", 0), ("gOut", 0),
        ("fx", 0), ("fe", 0), ("fm", 0), ("fd", 0), ("fs", ""), ("fmtOut", ""),
        ("tOut", ""), ("dOut", ""), ("di", 0),
        ("pa", 0), ("pb", 0), ("pOut2", 0),
        ("fi", 0), ("fv", 0), ("fac", 0), ("fargc", 0),
        ("uiCostume", "normal"),
        ("dlText", ""), ("dlY", 0), ("dlSize", 100), ("dlAlign", 0),
        ("dlLen", 0), ("dlStart", 0), ("dlX", 0), ("dlI", 0),
        ("dlCh", ""), ("dlIdx", 0), ("dlAdv", 0), ("dlMax", 0),
        ("k", 0), ("px", 0), ("py", 0), ("penWasDown", 0),
        ("glowX", 0), ("glowY", 0), ("glowSeq", 0),
        ("testIndex", 0),
        ("mx", 0), ("ty", 0), ("txs", ""), ("tys", ""),
        ("empty", 0), ("isf", 0), ("fname", ""), ("hasDot", 0), ("la", ""),
        ("cp", 0), ("tp", 0), ("sv", 0), ("cv", 0),
        ("u1", 0), ("u2", 0), ("u3", 0), ("u4", 0), ("g2Out", 0), ("es", ""),
        ("haveResult", 0), ("gs", 0), ("glog", 0),
        ("pgs", 20), ("pgx", 0), ("pgy", 0), ("pbin", ""),
        ("xval", 0), ("argc", 0), ("tok", ""), ("kind", ""), ("ins", ""),
        ("nidx", 0), ("uidx", 0), ("msg", ""), ("busy", 0), ("col", 0), ("row", 0),
        ("savedAngleMode", "DEG"),
        ("aIm", 0), ("aJ", 0), ("aK", 0),
        ("bIm", 0), ("bJ", 0), ("bK", 0),
        ("rIm", 0), ("rJ", 0), ("rK", 0),
        ("pvIm", 0), ("pvJ", 0), ("pvK", 0),
        ("numIm", 0), ("numJ", 0), ("numK", 0),
        ("resultIm", 0), ("resultJ", 0), ("resultK", 0),
        ("ansIm", 0), ("ansJ", 0), ("ansK", 0),
        ("memIm", 0), ("memJ", 0), ("memK", 0),
        ("previewIm", 0), ("previewJ", 0), ("previewK", 0),
        ("frIm", 0), ("frJ", 0), ("frK", 0),
        ("fOutIm", 0), ("fOutJ", 0), ("fOutK", 0),
        ("gOutIm", 0), ("gOutJ", 0), ("gOutK", 0),
        ("pOut2Im", 0), ("pOut2J", 0), ("pOut2K", 0),
        ("paIm", 0), ("paJ", 0), ("paK", 0),
        ("pbIm", 0), ("pbJ", 0), ("pbK", 0),
        ("t1Re", 0), ("t1Im", 0), ("t1J", 0), ("t1K", 0),
        ("t2Re", 0), ("t2Im", 0), ("t2J", 0), ("t2K", 0),
        ("t3Re", 0), ("t3Im", 0), ("t3J", 0), ("t3K", 0),
        ("qn", 0), ("qv", 0), ("qs", 0), ("qc", 0), ("qd", 0),
        ("qn2", 0), ("pn", 0), ("qtheta", 0), ("qmag", 0), ("qval", 0),
        ("qsv", 0), ("qcv", 0), ("qsh", 0), ("qch", 0), ("qshR", 0), ("qchR", 0),
        ("qea", 0), ("qcr", 0), ("qsr", 0), ("pneg", 0), ("qAny", 0),
        ("fmtValOut", ""), ("fv", ""), ("part", ""), ("comp", 0),
        ("esize", 110), ("rsize", 110),
    ]:
        m.var(name, init)
    for name, items in [
        ("out", []), ("outRe", []), ("outIm", []), ("outJ", []), ("outK", []),
        ("outKind", []), ("outArgc", []),
        ("ops", []), ("funcNames", []),
        ("parenCount", []), ("parenEmpty", []), ("parenFunc", []),
        ("vals", []), ("valsIm", []), ("valsJ", []), ("valsK", []),
        ("args", []), ("argsIm", []), ("argsJ", []), ("argsK", []),
        ("lanczos", LANCZOS),
        ("glyphChars", GLYPH_CHARS), ("glyphNames", GLYPH_NAMES),
        ("fromChars", FROM_CHARS), ("toStrs", TO_STRS),
        ("upperChars", [chr(c) for c in range(65, 91)]),
        ("lowerChars", [chr(c) for c in range(97, 123)]),
        ("digits", [str(i) for i in range(10)]),
        ("opChars", ["+", "-", "*", "/", "^", "%", "!"]),
        ("constNames", ["pi", "e", "tau", "phi", "ans", "mem", "x", "i", "j", "k"]),
        ("keyKinds", [k for row in NORMAL for _, k, *_ in row]),
        ("keyInserts", [row[2] if len(row) > 2 else "" for r in NORMAL for row in r]),
        ("keyKinds2", [k for row in SECOND for _, k, *_ in row]),
        ("keyInserts2", [row[2] if len(row) > 2 else "" for r in SECOND for row in r]),
        ("testExpr", []), ("testMode", []), ("testExpected", []),
        ("testLog", []),
    ]:
        m.list(name, items)
    return m
