#!/usr/bin/env python3
"""Build the Ultimate Scientific Calculator .sb3."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))

from model import build_model, GLYPH_CHARS, NORMAL, SECOND
from sb3 import Builder, write_sb3, costume
from evaluator import Evaluator
import assets


def glyph_name(ch):
    return "g%d" % ord(ch)


def set_local(b, name, vid, value):
    return b._add("data_setvariableto", {"VALUE": value},
                  {"VARIABLE": [name, vid]})


def local_var(b, name, vid):
    return b._add("data_variable", fields={"VARIABLE": [name, vid]})


def flag_hat(b, body, x, y):
    h = b._add("event_whenflagclicked", top=True, x=x, y=y)
    b.stack(h, body)
    b.blocks[h]["next"] = body[0]
    return h


def click_hat(b, body, x, y):
    h = b._add("event_whenthisspriteclicked", top=True, x=x, y=y)
    b.stack(h, body)
    b.blocks[h]["next"] = body[0]
    return h


def build(path):
    m = build_model()
    for name in ("testExpr", "testMode", "testExpected", "testLog"):
        m.lists.pop(name, None)
        m.list_values.pop(name, None)

    b = Builder(m)
    ev = Evaluator(b)
    p = ev.p

    # --------------------------------------------------------------- main init
    init = [
        b.setv("screen", b.vtext("calc")),
        b.setv("angleMode", b.vtext("DEG")),
        b.setv("expr", b.vtext("")),
        b.setv("ans", b.vnum(0)),
        b.setv("mem", b.vnum(0)),
        b.setv("inv", b.vnum(0)),
        b.setv("graphExpr", b.vtext("sin(x)")),
        b.setv("scale", b.vnum(20)),
        b.setv("traceOn", b.vnum(0)),
        b.setv("traceValid", b.vnum(0)),
        b.setv("justEval", b.vnum(0)),
        b.setv("err", b.vnum(0)),
        b.setv("errMsg", b.vtext("")),
        b.setv("previewOk", b.vnum(0)),
        b.setv("xval", b.vnum(0)),
        b.setv("busy", b.vnum(0)),
        b.setv("glowSeq", b.vnum(0)),
        b.setv("uiCostume", b.vtext("normal")),
        b.op("looks_switchbackdropto", {"BACKDROP": b.vmenu(b.menu_backdrop("calc"))}),
        b.op("looks_switchcostumeto", {"COSTUME": b.vmenu(b.menu_costume("normal"))}),
        b.op("motion_gotoxy", {"X": b.vnum(0), "Y": b.vnum(0)}),
        b.op("looks_setsizeto", {"SIZE": b.vnum(100)}),
        b.op("looks_seteffectto", {"VALUE": b.vnum(0)}, {"EFFECT": ["ghost", None]}),
        b.op("looks_show"),
        b.op("pen_clear"),
        b.call(p["drawDisplay"], []),
    ]
    flag_hat(b, init, 0, 0)

    # -------------------------------------------------------------- click logic
    def mx():
        return b.op("sensing_mousex")

    def my():
        return b.op("sensing_mousey")

    graph_click = [
        b.if_(b.and_(b.not_(b.gt(my(), b.vnum(-141))),
                     b.not_(b.lt(my(), b.vnum(-174)))), [
            b.setv("col", b.mathop("floor", b.div(b.add(mx(), b.vnum(234)), b.vnum(78)))),
            b.if_(b.and_(b.not_(b.lt(b.vvar("col"), b.vnum(0))),
                         b.not_(b.gt(b.vvar("col"), b.vnum(5)))), [
                b.setv("glowX", b.add(b.vnum(-195), b.mul(b.vvar("col"), b.vnum(78)))),
                b.setv("glowY", b.vnum(-157)),
                b.changev("glowSeq", b.vnum(1)),
                b.call(p["handleGraphKey"], [b.vblock(b.add(b.vvar("col"), b.vnum(1)))]),
            ]),
        ]),
    ]
    calc_click = [
        b.if_(b.and_(
            b.and_(b.not_(b.gt(my(), b.vnum(86))),
                   b.not_(b.lt(my(), b.vnum(-176)))),
            b.and_(b.not_(b.lt(mx(), b.vnum(-234))),
                   b.not_(b.gt(mx(), b.vnum(234))))), [
            b.setv("col", b.mathop("floor", b.div(b.add(mx(), b.vnum(234)), b.vnum(78)))),
            b.setv("row", b.mathop("floor", b.div(b.sub(b.vnum(83.5), my()), b.vnum(37)))),
            b.if_(b.and_(
                b.and_(b.not_(b.lt(b.vvar("col"), b.vnum(0))),
                       b.not_(b.gt(b.vvar("col"), b.vnum(5)))),
                b.and_(b.not_(b.lt(b.vvar("row"), b.vnum(0))),
                       b.not_(b.gt(b.vvar("row"), b.vnum(6))))), [
                b.setv("glowX", b.add(b.vnum(-195), b.mul(b.vvar("col"), b.vnum(78)))),
                b.setv("glowY", b.sub(b.vnum(65), b.mul(b.vvar("row"), b.vnum(37)))),
                b.changev("glowSeq", b.vnum(1)),
                b.call(p["handleKeyIndex"], [b.vblock(b.add(
                    b.mul(b.vvar("row"), b.vnum(6)),
                    b.add(b.vvar("col"), b.vnum(1))))]),
            ]),
        ]),
    ]
    onClick = b.defproc("onClick", [], True)
    onClick.body([
        b.if_(b.eqn("busy", 0), [
            b.setv("busy", b.vnum(1)),
            b.ifelse(b.eqv("screen", "graph"), graph_click, calc_click),
            b.setv("busy", b.vnum(0)),
        ]),
    ])
    click_hat(b, [b.call(onClick, [])], 0, 400)

    # -------------------------------------------------------------- trace loop
    def join2(a, c):
        return b.vblock(b.join(a, c))

    trace = [
        b.ifelse(b.and_(b.eqv("screen", "graph"),
                        b.and_(b.eqn("traceOn", 1), b.eqn("busy", 0))), [
            b.setv("mx", mx()),
            b.if_(b.lt(b.vvar("mx"), b.vnum(-235)), [b.setv("mx", b.vnum(-235))]),
            b.if_(b.gt(b.vvar("mx"), b.vnum(235)), [b.setv("mx", b.vnum(235))]),
            b.setv("traceX", b.vvar("mx")),
            b.setv("xval", b.div(b.vvar("mx"), b.vvar("scale"))),
            b.call(p["evaluate"], [b.vblock(b.var("graphExpr"))]),
            b.call(p["fmt"], [b.vblock(b.div(b.vvar("mx"), b.vvar("scale")))]),
            b.setv("txs", b.vvar("fmtOut")),
            b.ifelse(b.eqn("err", 0), [
                b.setv("ty", b.mul(b.vvar("result"), b.vvar("scale"))),
                b.ifelse(b.and_(b.not_(b.lt(b.vvar("ty"), b.vnum(-135))),
                                b.not_(b.gt(b.vvar("ty"), b.vnum(150)))),
                         [b.setv("traceY", b.vvar("ty")),
                          b.setv("traceValid", b.vnum(1))],
                         [b.setv("traceValid", b.vnum(0))]),
                b.call(p["fmt"], [b.vblock(b.var("result"))]),
                b.setv("tys", b.vvar("fmtOut")),
                b.setv("traceText", join2(
                    b.vtext("x = "),
                    join2(b.vvar("txs"),
                          join2(b.vtext(",  y = "), b.vvar("tys"))))),
            ], [
                b.setv("traceValid", b.vnum(0)),
                b.setv("traceText", join2(
                    b.vtext("x = "),
                    join2(b.vvar("txs"), b.vtext(",  y = undefined")))),
            ]),
            b.setv("err", b.vnum(0)),
            b.setv("errMsg", b.vtext("")),
        ], [
            b.setv("traceValid", b.vnum(0)),
        ]),
        b.wait(b.vnum(0.05)),
    ]
    flag_hat(b, [b.forever(trace)], 0, 800)

    # ---------------------------------------------------------------- costumes
    keypad_n = assets.keypad_svg(NORMAL)
    keypad_s = assets.keypad_svg(SECOND)
    graphui = assets.graphui_svg()
    glyphs = {glyph_name(ch): assets.glyph_svg(ch) for ch in GLYPH_CHARS}
    calc_costumes = [
        costume("normal", keypad_n, 240, 180),
        costume("second", keypad_s, 240, 180),
        costume("graphui", graphui, 240, 180),
    ] + [costume(glyph_name(ch), glyphs[glyph_name(ch)], 6, 11)
         for ch in GLYPH_CHARS]
    calc_bg = assets.calc_backdrop_svg()
    graph_bg = assets.graph_backdrop_svg()
    stage = b.stage_json([
        costume("calc", calc_bg, 240, 180),
        costume("graph", graph_bg, 240, 180),
    ])
    calc = b.sprite_json(
        "Calculator", calc_costumes, b.blocks, layer=1,
        visible=True, size=100, direction=90, rotation_style="don't rotate")

    # -------------------------------------------------------------- KeyGlow
    glow_svg = assets.key_glow_svg()
    gb = Builder(m)
    g_seen = lambda: local_var(gb, "seen", "v_seen")
    g_body = [
        gb.op("looks_hide"),
        set_local(gb, "seen", "v_seen", gb.vnum(0)),
        gb.forever([
            gb.if_(gb.not_(gb.eq(g_seen(), gb.vvar("glowSeq"))), [
                set_local(gb, "seen", "v_seen", gb.vvar("glowSeq")),
                gb.op("motion_gotoxy", {"X": gb.vvar("glowX"), "Y": gb.vvar("glowY")}),
                gb.op("looks_show"),
                gb.wait(gb.vnum(0.06)),
                gb.op("looks_hide"),
            ]),
        ]),
    ]
    flag_hat(gb, g_body, 0, 0)
    glow = gb.sprite_json("KeyGlow", [costume("glow", glow_svg, 37, 17)],
                          gb.blocks, layer=2, visible=False,
                          variables={"v_seen": ["seen", 0]})

    # ------------------------------------------------------------- Crosshair
    cross_svg = assets.crosshair_svg()
    cb = Builder(m)
    c_body = [
        cb.op("looks_hide"),
        cb.forever([
            cb.ifelse(cb.and_(cb.eqv("screen", "graph"), cb.eqn("traceOn", 1)), [
                cb.op("motion_gotoxy", {"X": cb.vvar("traceX"), "Y": cb.vnum(7.5)}),
                cb.op("looks_show"),
            ], [cb.op("looks_hide")]),
        ]),
    ]
    flag_hat(cb, c_body, 0, 0)
    cross = cb.sprite_json("Crosshair", [costume("line", cross_svg, 1, 142.5)],
                           cb.blocks, layer=3, visible=False)

    # --------------------------------------------------------------- TraceDot
    dot_svg = assets.dot_svg()
    db = Builder(m)
    d_body = [
        db.op("looks_hide"),
        db.forever([
            db.ifelse(db.and_(db.and_(db.eqv("screen", "graph"),
                                      db.eqn("traceOn", 1)),
                              db.eqn("traceValid", 1)), [
                db.op("motion_gotoxy", {"X": db.vvar("traceX"), "Y": db.vvar("traceY")}),
                db.op("looks_show"),
            ], [db.op("looks_hide")]),
        ]),
    ]
    flag_hat(db, d_body, 0, 0)
    dot = db.sprite_json("TraceDot", [costume("dot", dot_svg, 5, 5)],
                         db.blocks, layer=4, visible=False)

    # ------------------------------------------------------------- TraceLabel
    mark_svg = assets.trace_dot_svg()
    tb = Builder(m)
    t_body = [
        tb.op("looks_say", {"MESSAGE": tb.vtext("")}),
        tb.forever([
            tb.ifelse(tb.and_(tb.eqv("screen", "graph"), tb.eqn("traceOn", 1)),
                      [tb.op("looks_say", {"MESSAGE": tb.vblock(tb.var("traceText"))})],
                      [tb.op("looks_say", {"MESSAGE": tb.vtext("")})]),
            tb.wait(tb.vnum(0.1)),
        ]),
    ]
    flag_hat(tb, t_body, 0, 0)
    trace_lbl = tb.sprite_json("TraceLabel", [costume("mark", mark_svg, 3, 3)],
                               tb.blocks, layer=5, visible=True, x=-215, y=152)

    project = {
        "targets": [stage, calc, glow, cross, dot, trace_lbl],
        "monitors": [],
        "extensions": ["pen"],
        "meta": {"semver": "3.0.0", "vm": "0.2.0",
                 "agent": "xyo-calculator-generator"},
    }
    amap = {
        "calc_bg": calc_bg,
        "graph_bg": graph_bg,
        "keypad": keypad_n,
        "keypad2": keypad_s,
        "graphui": graphui,
        "glow": glow_svg,
        "cross": cross_svg,
        "dot": dot_svg,
        "mark": mark_svg,
    }
    amap.update(glyphs)
    write_sb3(path, project, amap)
    print("wrote", path, "main blocks:", len(calc["blocks"]),
          "glyphs:", len(glyphs), "size:", os.path.getsize(path))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "scientific_calculator.sb3"
    build(out)
