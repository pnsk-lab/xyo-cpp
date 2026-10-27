"""Scratch implementation of the expression evaluator (shared by app + tests)."""
import math

from sb3 import Model  # noqa: F401
from qmath import QMathMixin


class Evaluator(QMathMixin):
    def __init__(self, b):
        self.b = b
        self.m = b.m
        self.p = {}
        self._define()
        self._bodies()

    # ------------------------------------------------------------- utilities
    def V(self, name):
        return self.b.vvar(name)

    def B(self, name):
        return self.b.vblock(self.b.var(name))

    def T(self, s):
        return self.b.vtext(s)

    def N(self, n):
        return self.b.vnum(n)

    def eqv(self, name, value):
        return self.b.eq(self.V(name), self.T(value))

    def eqn(self, name, value):
        return self.b.eq(self.V(name), self.N(value))

    def is_value(self):
        b = self.b
        return b.or_(b.or_(self.eqv("prev", "num"), self.eqv("prev", "const")),
                     b.or_(self.eqv("prev", "rparen"), self.eqv("prev", "postfix")))

    def last(self, name):
        return self.b.vblock(self.b.list_item(name, self.b.vblock(self.b.list_len(name))))

    def len_is_zero(self, name):
        return self.b.eq(self.b.vblock(self.b.list_len(name)), self.N(0))

    def digit(self, ch):
        return self.b.gt(self.b.vblock(self.b.list_itemnum("digits", ch)), self.N(0))

    def alpha(self, ch):
        return self.b.contains(self.T("abcdefghijklmnopqrstuvwxyz"), ch)

    def alnum(self, chf):
        b = self.b
        return b.or_(b.or_(self.alpha(chf()), self.digit(chf())),
                     b.eq(chf(), self.T("_")))

    def call(self, proc, args):
        return self.b.call(proc, args)

    def setv(self, name, val):
        return self.b.setv(name, val)

    def changev(self, name, val):
        return self.b.changev(name, val)

    def err(self, msg):
        return [self.setv("err", self.N(1)), self.setv("errMsg", self.T(msg))]

    def result(self, val):
        return [self.setv("fr", val),
                self.setv("frIm", self.N(0)),
                self.setv("frJ", self.N(0)),
                self.setv("frK", self.N(0)),
                self.setv("haveResult", self.N(1))]

    # ------------------------------------------------------------- procedures
    def _define(self):
        b = self.b
        p = self.p
        p["normalize"] = b.defproc("normalize", ["rawIn"], True)
        p["prec"] = b.defproc("prec", ["op"], True)
        p["pushOut"] = b.defproc("pushOut", ["tok", "knd", "cnt"], True)
        p["pushNumber"] = b.defproc("pushNumber", ["re", "im", "jj", "kk"], True)
        p["pushOp"] = b.defproc("pushOp", ["op"], True)
        p["pushParen"] = b.defproc("pushParen", ["isf"], True)
        p["pushFunc"] = b.defproc("pushFunc", ["fn"], True)
        p["popOp"] = b.defproc("popOp", [], True)
        p["pushConst"] = b.defproc("pushConst", ["nm"], True)
        p["popVal"] = b.defproc("popVal", [], True)
        p["closeParen"] = b.defproc("closeParen", [], True)
        p["pushBinary"] = b.defproc("pushBinary", ["op"], True)
        p["powr"] = b.defproc("powr", ["aa", "bb"], True)
        p["gcd2"] = b.defproc("gcd2", ["aa", "bb"], True)
        p["fact"] = b.defproc("fact", ["n"], True)
        p["gamma"] = b.defproc("gamma", ["x"], True)
        p["dropLast"] = b.defproc("dropLast", ["s"], True)
        p["trim"] = b.defproc("trim", ["s"], True)
        p["fmt"] = b.defproc("fmt", ["x"], True)
        p["fmtVal"] = b.defproc("fmtVal", ["xr", "xi", "xj", "xk"], True)
        p["addPart"] = b.defproc("addPart", ["unit"], True)
        p["applyOp"] = b.defproc("applyOp", ["op"], True)
        p["applyFunc"] = b.defproc("applyFunc", ["fn", "ac"], True)
        p["evaluate"] = b.defproc("evaluate", ["source"], True)
        p["preview"] = b.defproc("preview", [], True)
        p["eval"] = b.defproc("eval", [], True)
        p["insertText"] = b.defproc("insertText", ["t"], True)
        p["delLast"] = b.defproc("delLast", [], True)
        p["clearAll"] = b.defproc("clearAll", [], True)
        p["toggleMode"] = b.defproc("toggleMode", [], True)
        p["toggleInv"] = b.defproc("toggleInv", [], True)
        p["memClear"] = b.defproc("memClear", [], True)
        p["memRecall"] = b.defproc("memRecall", [], True)
        p["memAdd"] = b.defproc("memAdd", ["sign"], True)
        p["graphFromExpr"] = b.defproc("graphFromExpr", [], True)
        p["typeAsk"] = b.defproc("typeAsk", [], False)
        p["switchToGraph"] = b.defproc("switchToGraph", [], True)
        p["switchToCalc"] = b.defproc("switchToCalc", [], True)
        p["graphAsk"] = b.defproc("graphAsk", [], False)
        p["handleGraphKey"] = b.defproc("handleGraphKey", ["idx"], True)
        p["plotGraph"] = b.defproc("plotGraph", [], True)
        p["drawLine"] = b.defproc("drawLine", ["text", "yy", "sz", "al"], True)
        p["drawDisplay"] = b.defproc("drawDisplay", [], True)
        p["handleKeyIndex"] = b.defproc("handleKeyIndex", ["idx"], True)
        self._qdef()

    def _bodies(self):
        self.gen_normalize()
        self.gen_prec()
        self.gen_push_out()
        self.gen_push_number()
        self.gen_push_op()
        self.gen_push_paren()
        self.gen_push_func()
        self.gen_pop_op()
        self.gen_push_const()
        self.gen_pop_val()
        self.gen_close_paren()
        self.gen_push_binary()
        self.gen_powr()
        self.gen_gcd2()
        self.gen_fact()
        self.gen_gamma()
        self.gen_drop_last()
        self.gen_trim()
        self.gen_fmt()
        self.gen_fmt_val()
        self._qgen()
        self.gen_apply_op()
        self.gen_apply_func()
        self.gen_evaluate()
        self.gen_preview()
        self.gen_eval()
        self.gen_insert_text()
        self.gen_del_last()
        self.gen_clear_all()
        self.gen_toggles()
        self.gen_memory()
        self.gen_switch()
        self.gen_plot()
        self.gen_draw_line()
        self.gen_draw_display()
        self.gen_handle_key()
        self.gen_graph_keys()

    # ------------------------------------------------------------- normalize
    def gen_normalize(self):
        b = self.b
        p = self.p["normalize"]
        body = [
            self.setv("normOut", self.T("")),
            self.setv("i", self.N(1)),
        ]
        loop = [
            self.setv("ch", b.vblock(b.letter(self.V("i"), b.vblock(b.arg("rawIn"))))),
            self.setv("nidx", b.vblock(b.list_itemnum("fromChars", self.V("ch")))),
            self.setv("uidx", b.vblock(b.list_itemnum("upperChars", self.V("ch")))),
            b.ifelse(
                b.gt(self.V("nidx"), self.N(0)),
                [self.setv("normOut", b.join(
                    self.V("normOut"),
                    b.vblock(b.list_item("toStrs", self.V("nidx")))))],
                [b.ifelse(
                    b.gt(self.V("uidx"), self.N(0)),
                    [self.setv("normOut", b.join(
                        self.V("normOut"),
                        b.vblock(b.list_item("lowerChars", self.V("uidx")))))],
                    [self.setv("normOut", b.join(self.V("normOut"), self.V("ch")))],
                )],
            ),
            self.changev("i", self.N(1)),
        ]
        body.append(b.repeat(b.vblock(b.length(b.vblock(b.arg("rawIn")))), loop))
        p.body(body)

    # ------------------------------------------------------------------ prec
    def gen_prec(self):
        b = self.b
        p = self.p["prec"]
        ops = [("+", 1), ("-", 1), ("*", 2), ("/", 2),
               ("u-", 3), ("^", 4), ("!", 5), ("%", 5)]
        cur = [self.setv("pOut", self.N(0))]
        for op, val in reversed(ops):
            cur = [b.ifelse(b.eq(self.b.vblock(self.b.arg("op")), self.T(op)),
                            [self.setv("pOut", self.N(val))], cur)]
        p.body(cur)

    # --------------------------------------------------------------- pushes
    def gen_push_out(self):
        b = self.b
        p = self.p["pushOut"]
        body = [
            b.list_add("out", self.b.vblock(self.b.arg("tok"))),
            b.list_add("outRe", self.b.vblock(self.b.arg("tok"))),
            b.list_add("outIm", self.T("0")),
            b.list_add("outJ", self.T("0")),
            b.list_add("outK", self.T("0")),
            b.list_add("outKind", self.b.vblock(self.b.arg("knd"))),
            b.list_add("outArgc", self.b.vblock(self.b.arg("cnt"))),
            b.if_(b.gt(self.b.vblock(b.list_len("parenEmpty")), self.N(0)), [
                b.list_replace("parenEmpty", self.b.vblock(b.list_len("parenEmpty")), self.N(0))
            ]),
        ]
        p.body(body)

    def gen_push_number(self):
        b = self.b
        p = self.p["pushNumber"]
        body = [
            b.list_add("out", self.b.vblock(self.b.arg("re"))),
            b.list_add("outRe", self.b.vblock(self.b.arg("re"))),
            b.list_add("outIm", self.b.vblock(self.b.arg("im"))),
            b.list_add("outJ", self.b.vblock(self.b.arg("jj"))),
            b.list_add("outK", self.b.vblock(self.b.arg("kk"))),
            b.list_add("outKind", self.T("n")),
            b.list_add("outArgc", self.N(0)),
            b.if_(b.gt(self.b.vblock(b.list_len("parenEmpty")), self.N(0)), [
                b.list_replace("parenEmpty", self.b.vblock(b.list_len("parenEmpty")), self.N(0))
            ]),
        ]
        p.body(body)

    def gen_push_op(self):
        b = self.b
        p = self.p["pushOp"]
        p.body([
            b.list_add("ops", self.b.vblock(self.b.arg("op"))),
            b.list_add("funcNames", self.T("")),
        ])

    def gen_push_paren(self):
        b = self.b
        p = self.p["pushParen"]
        p.body([
            b.list_add("ops", self.T("(")),
            b.list_add("funcNames", self.T("")),
            b.list_add("parenCount", self.b.vblock(self.b.arg("isf"))),
            b.list_add("parenEmpty", self.N(1)),
            b.list_add("parenFunc", self.b.vblock(self.b.arg("isf"))),
        ])

    def gen_push_func(self):
        b = self.b
        p = self.p["pushFunc"]
        p.body([
            b.list_add("ops", self.T("f")),
            b.list_add("funcNames", self.b.vblock(self.b.arg("fn"))),
        ])

    def gen_pop_op(self):
        b = self.b
        p = self.p["popOp"]
        tok = self.last("ops")
        body = [
            b.if_(b.gt(self.b.vblock(b.list_len("ops")), self.N(0)), [
                self.setv("tok", tok),
                b.list_delete("ops", self.b.vblock(b.list_len("ops"))),
                b.list_delete("funcNames", self.b.vblock(b.list_len("funcNames"))),
                self.call(self.p["pushOut"], [self.B("tok"), self.T("o"), self.N(0)]),
            ])
        ]
        p.body(body)

    def gen_push_const(self):
        b = self.b
        p = self.p["pushConst"]
        nm_is = lambda name: b.eq(b.vblock(b.arg("nm")), self.T(name))
        cases = [
            ("pi", "3.141592653589793"),
            ("e", "2.718281828459045"),
            ("tau", "6.283185307179586"),
            ("phi", "1.618033988749895"),
        ]
        cur = [*self.err("Unknown name")]
        for name, val in reversed(cases):
            cur = [b.ifelse(nm_is(name), [
                self.call(self.p["pushNumber"], [self.T(val), self.T("0"), self.T("0"), self.T("0")])
            ], cur)]
        for name, comps in (("i", ("0", "1", "0")), ("j", ("0", "0", "1")),
                            ("k", ("0", "0", "0"))):
            cur = [b.ifelse(nm_is(name), [
                self.call(self.p["pushNumber"], [self.T(comps[0]), self.T(comps[1]),
                                                 self.T(comps[2]),
                                                 self.T("1" if name == "k" else "0")])
            ], cur)]
        # ans / mem / x use the current values
        for name, var in (("x", "xval"), ("mem", "mem"), ("ans", "ans")):
            cur = [b.ifelse(nm_is(name), [
                self.call(self.p["pushNumber"], [b.join(self.V(var), self.T("")), self.T("0"), self.T("0"), self.T("0")])
            ], cur)]
        p.body(cur)

    def gen_pop_val(self):
        b = self.b
        p = self.p["popVal"]
        p.body([
            b.ifelse(b.eq(self.b.vblock(b.list_len("vals")), self.N(0)),
                     [*self.err("Stack"), self.setv("pv", self.N(0))],
                     [
                         self.setv("pv", self.last("vals")),
                         self.setv("pvIm", self.last("valsIm")),
                         self.setv("pvJ", self.last("valsJ")),
                         self.setv("pvK", self.last("valsK")),
                         b.list_delete("vals", self.b.vblock(b.list_len("vals"))),
                         b.list_delete("valsIm", self.b.vblock(b.list_len("valsIm"))),
                         b.list_delete("valsJ", self.b.vblock(b.list_len("valsJ"))),
                         b.list_delete("valsK", self.b.vblock(b.list_len("valsK"))),
                     ])
        ])

    # ------------------------------------------------------------- closeParen
    def gen_close_paren(self):
        b = self.b
        p = self.p["closeParen"]
        ops_len = lambda: self.b.vblock(b.list_len("ops"))
        last_op = self.last("ops")
        # pop until "("
        pop_until = b.repeat_until(
            b.or_(b.eq(ops_len(), self.N(0)), b.eq(last_op, self.T("("))),
            [self.call(self.p["popOp"], [])],
        )
        body = [
            b.if_(self.eqn("err", 0), [
                b.ifelse(b.eq(self.b.vblock(b.list_len("parenCount")), self.N(0)),
                         self.err("Unmatched )"),
                         [
                             pop_until,
                             b.ifelse(b.eq(ops_len(), self.N(0)),
                                      self.err("Unmatched )"),
                                      [
                                          b.list_delete("ops", ops_len()),
                                          b.list_delete("funcNames", self.b.vblock(b.list_len("funcNames"))),
                                          self.setv("argc", self.last("parenCount")),
                                          self.setv("empty", self.last("parenEmpty")),
                                          self.setv("isf", self.last("parenFunc")),
                                          b.list_delete("parenCount", self.b.vblock(b.list_len("parenCount"))),
                                          b.list_delete("parenEmpty", self.b.vblock(b.list_len("parenEmpty"))),
                                          b.list_delete("parenFunc", self.b.vblock(b.list_len("parenFunc"))),
                                          b.ifelse(self.eqn("isf", 1),
                                                   [
                                                       b.ifelse(b.or_(b.eq(ops_len(), self.N(0)),
                                                                      b.not_(b.eq(self.last("ops"), self.T("f")))),
                                                                self.err("Bad call"),
                                                                [
                                                                    self.setv("fname", self.last("funcNames")),
                                                                    b.list_delete("ops", ops_len()),
                                                                    b.list_delete("funcNames", self.b.vblock(b.list_len("funcNames"))),
                                                                    b.if_(self.eqn("empty", 1), [self.setv("argc", self.N(0))]),
                                                                    self.call(self.p["pushOut"], [self.B("fname"), self.T("f"), self.V("argc")]),
                                                                    self.setv("prev", self.T("rparen")),
                                                                ]),
                                                   ],
                                                   [
                                                       b.ifelse(self.b.not_(self.is_value()),
                                                                self.err("Empty ()"),
                                                                [self.setv("prev", self.T("rparen"))]),
                                                   ]),
                                          b.if_(b.and_(self.eqn("err", 0),
                                                       b.gt(self.b.vblock(b.list_len("parenEmpty")), self.N(0))),
                                                [b.list_replace("parenEmpty", self.b.vblock(b.list_len("parenEmpty")), self.N(0))]),
                                      ]),
                         ]),
            ])
        ]
        p.body(body)

    # ------------------------------------------------------------ pushBinary
    def gen_push_binary(self):
        b = self.b
        p = self.p["pushBinary"]
        ops_len = lambda: self.b.vblock(b.list_len("ops"))
        body = [
            self.setv("pbin", self.b.vblock(self.b.arg("op"))),
            self.call(self.p["prec"], [self.B("pbin")]),
            self.setv("cp", self.V("pOut")),
            self.setv("popping", self.N(1)),
            b.repeat_until(self.eqn("popping", 0), [
                b.ifelse(b.or_(b.or_(b.eq(ops_len(), self.N(0)),
                                     b.eq(self.last("ops"), self.T("("))),
                               b.eq(self.last("ops"), self.T("f"))),
                         [self.setv("popping", self.N(0))],
                         [
                             self.call(self.p["prec"], [self.last("ops")]),
                             self.setv("tp", self.V("pOut")),
                             b.ifelse(b.or_(b.gt(self.V("tp"), self.V("cp")),
                                            b.and_(b.eq(self.V("tp"), self.V("cp")),
                                                   b.not_(self.eqv("pbin", "^")))),
                                      [self.call(self.p["popOp"], [])],
                                      [self.setv("popping", self.N(0))]),
                         ]),
            ]),
            self.call(self.p["pushOp"], [self.B("pbin")]),
        ]
        p.body(body)

    # ------------------------------------------------------------------ powr
    def gen_powr(self):
        b = self.b
        p = self.p["powr"]
        # integer test and parity helpers
        integer = b.eq(self.V("pb"), b.vblock(b.round_(self.V("pb"))))
        even = b.eq(b.mul(b.mathop("floor", b.div(self.V("pb"), self.N(2))), self.N(2)), self.V("pb"))
        body = [
            self.setv("pa", self.b.vblock(self.b.arg("aa"))),
            self.setv("pb", self.b.vblock(self.b.arg("bb"))),
            self.setv("pOut2Im", self.N(0)),
            self.setv("pOut2J", self.N(0)),
            self.setv("pOut2K", self.N(0)),
            b.ifelse(self.eqn("pb", 0),
                     [self.setv("pOut2", self.N(1))],
                     [b.ifelse(self.eqn("pa", 0),
                               [b.ifelse(b.lt(self.V("pb"), self.N(0)),
                                         self.err("Domain"),
                                         [self.setv("pOut2", self.N(0))])],
                               [b.ifelse(b.gt(self.V("pa"), self.N(0)),
                                         [
                                             self.setv("pOut2", b.mathop("e ^", b.mul(self.V("pb"), b.mathop("ln", self.V("pa"))))),
                                             b.if_(b.gt(b.mathop("abs", self.V("pOut2")), self.N(1e308)),
                                                   self.err("Overflow")),
                                         ],
                                         [
                                             b.ifelse(b.not_(integer),
                                                      self.err("Domain"),
                                                      [
                                                          self.setv("pOut2", b.mathop("e ^", b.mul(self.V("pb"), b.mathop("ln", b.mathop("abs", self.V("pa")))))),
                                                          b.if_(b.not_(even), [self.setv("pOut2", b.sub(self.N(0), self.V("pOut2")))]),
                                                          b.if_(b.gt(b.mathop("abs", self.V("pOut2")), self.N(1e308)),
                                                                self.err("Overflow")),
                                                      ]),
                                         ])])]),
        ]
        p.body(body)

    # ------------------------------------------------------------------ gcd2
    def gen_gcd2(self):
        b = self.b
        p = self.p["gcd2"]
        body = [
            self.setv("u1", b.mathop("abs", self.b.vblock(self.b.arg("aa")))),
            self.setv("u2", b.mathop("abs", self.b.vblock(self.b.arg("bb")))),
            b.repeat_until(self.eqn("u2", 0), [
                self.setv("u3", b.mod(self.V("u1"), self.V("u2"))),
                self.setv("u1", self.V("u2")),
                self.setv("u2", self.V("u3")),
            ]),
            self.setv("g2Out", self.V("u1")),
        ]
        p.body(body)

    # ------------------------------------------------------------------ fact
    def gen_fact(self):
        b = self.b
        p = self.p["fact"]
        call_gamma = lambda: self.call(self.p["gamma"], [
            b.vblock(b.add(self.V("fn"), self.N(1)))])
        body = [
            self.setv("fn", self.b.vblock(self.b.arg("n"))),
            self.setv("fOutIm", self.N(0)),
            self.setv("fOutJ", self.N(0)),
            self.setv("fOutK", self.N(0)),
            b.ifelse(b.lt(self.V("fn"), self.N(0)),
                     [call_gamma(), b.if_(self.eqn("err", 0), [self.setv("fOut", self.V("gOut"))])],
                     [b.ifelse(b.eq(self.V("fn"), b.vblock(b.round_(self.V("fn")))),
                               [b.ifelse(b.gt(self.V("fn"), self.N(170)),
                                         [call_gamma(), b.if_(self.eqn("err", 0), [self.setv("fOut", self.V("gOut"))])],
                                         [b.ifelse(b.gt(self.V("fn"), self.N(1)),
                                                   [
                                                       self.setv("fr", self.N(1)),
                                                       self.setv("fk", self.N(2)),
                                                       b.repeat(b.sub(self.V("fn"), self.N(1)), [
                                                           self.setv("fr", b.mul(self.V("fr"), self.V("fk"))),
                                                           self.changev("fk", self.N(1)),
                                                       ]),
                                                       self.setv("fOut", self.V("fr")),
                                                   ],
                                                   [self.setv("fOut", self.N(1))])])],
                               [call_gamma(), b.if_(self.eqn("err", 0), [self.setv("fOut", self.V("gOut"))])])]),
        ]
        p.body(body)

    # ----------------------------------------------------------------- gamma
    def gen_gamma(self):
        b = self.b
        p = self.p["gamma"]
        pi = 3.141592653589793
        lanc = lambda i: b.vblock(b.list_item("lanczos", i))
        gx = lambda: self.V("gx")
        neg_or_zero = b.and_(b.eq(gx(), b.vblock(b.round_(gx()))),
                             b.not_(b.gt(gx(), self.N(0))))
        refl = [
            self.call(self.p["gamma"], [b.vblock(b.sub(self.N(1), gx()))]),
            b.if_(self.eqn("err", 0), [
                self.setv("gs", b.mathop("sin", b.mul(gx(), self.N(180)))),
                b.ifelse(self.eqn("gs", 0),
                         self.err("Domain"),
                         [
                             self.setv("gOut", b.div(self.N(pi), b.mul(self.V("gs"), self.V("gOut")))),
                             b.if_(b.gt(b.mathop("abs", self.V("gOut")), self.N(1e308)),
                                   self.err("Overflow")),
                         ]),
            ]),
        ]
        direct = [
            self.setv("gz", b.sub(gx(), self.N(1))),
            self.setv("ga", lanc(1)),
            self.setv("gt", b.add(self.V("gz"), self.N(7.5))),
            self.setv("gi", self.N(1)),
            b.repeat(self.N(8), [
                self.setv("ga", b.add(self.V("ga"),
                                      b.div(lanc(b.add(self.V("gi"), self.N(1))),
                                            b.add(self.V("gz"), self.V("gi"))))),
                self.changev("gi", self.N(1)),
            ]),
            self.setv("glog", b.add(
                b.add(b.mul(self.N(0.5), b.mathop("ln", self.N(2 * pi))),
                      b.mul(b.add(self.V("gz"), self.N(0.5)), b.mathop("ln", self.V("gt")))),
                b.add(b.sub(self.N(0), self.V("gt")),
                      b.mathop("ln", self.V("ga"))))),
            self.setv("gOut", b.mathop("e ^", self.V("glog"))),
            b.if_(b.gt(b.mathop("abs", self.V("gOut")), self.N(1e308)),
                  self.err("Overflow")),
        ]
        body = [
            self.setv("gx", self.b.vblock(self.b.arg("x"))),
            self.setv("gOutIm", self.N(0)),
            self.setv("gOutJ", self.N(0)),
            self.setv("gOutK", self.N(0)),
            b.ifelse(neg_or_zero, self.err("Domain"),
                     [b.ifelse(b.lt(gx(), self.N(0.5)), refl, direct)]),
        ]
        p.body(body)

    # -------------------------------------------------------------- dropLast
    def gen_drop_last(self):
        b = self.b
        p = self.p["dropLast"]
        body = [
            self.setv("dOut", self.T("")),
            self.setv("di", self.N(1)),
            b.repeat(b.sub(b.vblock(b.length(self.b.vblock(self.b.arg("s")))), self.N(1)), [
                self.setv("dOut", b.join(self.V("dOut"),
                                         self.b.vblock(self.b.letter(self.V("di"), self.b.vblock(self.b.arg("s")))))),
                self.changev("di", self.N(1)),
            ]),
        ]
        p.body(body)

    def gen_trim(self):
        b = self.b
        p = self.p["trim"]
        sarg = self.b.vblock(self.b.arg("s"))
        lastch = lambda: self.b.vblock(self.b.letter(self.b.vblock(b.length(self.V("tOut"))), self.V("tOut")))
        body = [
            self.setv("tOut", sarg),
            b.if_(b.contains(self.V("tOut"), self.T(".")), [
                b.repeat_until(b.or_(b.eq(self.b.vblock(b.length(self.V("tOut"))), self.N(0)),
                                     b.not_(b.eq(lastch(), self.T("0")))),
                               [
                                   self.call(self.p["dropLast"], [self.B("tOut")]),
                                   self.setv("tOut", self.V("dOut")),
                               ]),
                b.if_(b.and_(b.gt(self.b.vblock(b.length(self.V("tOut"))), self.N(0)),
                             b.eq(lastch(), self.T("."))),
                      [
                          self.call(self.p["dropLast"], [self.B("tOut")]),
                          self.setv("tOut", self.V("dOut")),
                      ]),
            ]),
        ]
        p.body(body)

    # ------------------------------------------------------------------- fmt
    def gen_fmt(self):
        b = self.b
        p = self.p["fmt"]
        absx = lambda: b.mathop("abs", self.V("fx"))
        pow10 = lambda e: b.mathop("10 ^", e)
        fmt_dec = [
            self.setv("fd", b.sub(self.N(11), self.V("fe"))),
            b.if_(b.lt(self.V("fd"), self.N(0)), [self.setv("fd", self.N(0))]),
            self.setv("fr", b.div(b.vblock(b.round_(b.mul(self.V("fx"), b.vblock(pow10(self.V("fd")))))),
                                  b.vblock(pow10(self.V("fd"))))),
            self.setv("fs", b.join(self.V("fr"), self.T(""))),
            self.call(self.p["trim"], [self.B("fs")]),
            self.setv("fmtOut", self.V("tOut")),
        ]
        fmt_sci = [
            self.setv("fm", b.div(self.V("fx"), b.vblock(pow10(self.V("fe"))))),
            self.setv("fm", b.div(b.vblock(b.round_(b.mul(self.V("fm"), self.N(1e11)))), self.N(1e11))),
            b.if_(b.not_(b.lt(absx(), self.N(10))), [
                self.setv("fm", b.div(self.V("fm"), self.N(10))),
                self.changev("fe", self.N(1)),
            ]),
            self.setv("fs", b.join(self.V("fm"), self.T(""))),
            self.call(self.p["trim"], [self.B("fs")]),
            b.ifelse(b.not_(b.lt(self.V("fe"), self.N(0))),
                     [self.setv("es", self.T("+"))],
                     [self.setv("es", self.T(""))]),
            self.setv("fmtOut", b.join(self.V("tOut"),
                                       b.join(self.T("E"),
                                              b.join(self.V("es"), b.join(self.V("fe"), self.T("")))))),
        ]
        body = [
            self.setv("fx", self.b.vblock(self.b.arg("x"))),
            b.ifelse(self.eqn("fx", 0),
                     [self.setv("fmtOut", self.T("0"))],
                     [b.ifelse(b.not_(b.eq(self.V("fx"), self.V("fx"))),
                               [self.setv("fmtOut", self.T("NaN"))],
                               [b.ifelse(b.gt(absx(), self.N(1e308)),
                                         [self.setv("fmtOut", self.T("Infinity"))],
                                         [b.ifelse(b.lt(absx(), self.N(1e-320)),
                                                   [self.setv("fmtOut", self.T("0"))],
                                                   [
                                                       self.setv("fe", b.mathop("floor",
                                                                                b.div(b.mathop("ln", absx()),
                                                                                      b.mathop("ln", self.N(10))))),
                                                       b.if_(b.not_(b.lt(absx(), b.sub(b.vblock(pow10(b.add(self.V("fe"), self.N(1)))),
                                                                                       b.mul(self.N(1e-12), absx())))),
                                                             [self.changev("fe", self.N(1))]),
                                                       b.if_(b.lt(absx(), b.vblock(pow10(self.V("fe")))),
                                                             [self.changev("fe", self.N(-1))]),
                                                       b.ifelse(b.or_(b.not_(b.lt(self.V("fe"), self.N(12))),
                                                                      b.not_(b.gt(self.V("fe"), self.N(-7)))),
                                                                fmt_sci, fmt_dec),
                                                   ])])])]),
        ]
        p.body(body)

    def gen_fmt_val(self):
        b = self.b
        # append one imaginary component (comp) to fv
        p = self.p["addPart"]
        body = [
            b.if_(b.gt(b.mathop("abs", self.V("comp")), self.N(1e-12)), [
                b.ifelse(b.eq(b.mathop("abs", self.V("comp")), self.N(1)),
                         [self.setv("part", b.vblock(b.arg("unit")))],
                         [self.call(self.p["fmt"], [b.vblock(b.mathop("abs", self.V("comp")))]),
                          self.setv("part", b.join(self.V("fmtOut"), b.vblock(b.arg("unit"))))]),
                b.ifelse(self.eqv("fv", ""),
                         [b.ifelse(b.lt(self.V("comp"), self.N(0)),
                                   [self.setv("fv", b.join(self.T("-"), self.V("part")))],
                                   [self.setv("fv", self.V("part"))])],
                         [b.ifelse(b.lt(self.V("comp"), self.N(0)),
                                   [self.setv("fv", b.join(self.V("fv"), b.join(self.T("-"), self.V("part"))))],
                                   [self.setv("fv", b.join(self.V("fv"), b.join(self.T("+"), self.V("part"))))])]),
            ]),
        ]
        p.body(body)

        p = self.p["fmtVal"]
        body = [
            self.setv("fv", self.T("")),
            b.if_(b.or_(b.gt(b.mathop("abs", self.b.vblock(self.b.arg("xr"))), self.N(1e-12)),
                        b.and_(b.and_(b.not_(b.gt(b.mathop("abs", self.b.vblock(self.b.arg("xi"))), self.N(1e-12))),
                                      b.not_(b.gt(b.mathop("abs", self.b.vblock(self.b.arg("xj"))), self.N(1e-12)))),
                                b.not_(b.gt(b.mathop("abs", self.b.vblock(self.b.arg("xk"))), self.N(1e-12))))), [
                self.call(self.p["fmt"], [self.b.vblock(self.b.arg("xr"))]),
                self.setv("fv", self.V("fmtOut")),
            ]),
            self.setv("comp", self.b.vblock(self.b.arg("xi"))),
            self.call(self.p["addPart"], [self.T("i")]),
            self.setv("comp", self.b.vblock(self.b.arg("xj"))),
            self.call(self.p["addPart"], [self.T("j")]),
            self.setv("comp", self.b.vblock(self.b.arg("xk"))),
            self.call(self.p["addPart"], [self.T("k")]),
            b.if_(self.eqv("fv", ""), [self.setv("fv", self.T("0"))]),
            self.setv("fmtValOut", self.V("fv")),
        ]
        p.body(body)

    # --------------------------------------------------------------- applyOp
    def gen_apply_op(self):
        b = self.b
        p = self.p["applyOp"]
        A, AI, AJ, AK = self.V("a"), self.V("aIm"), self.V("aJ"), self.V("aK")
        B, BI, BJ, BK = self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")
        op_is = lambda name: b.eq(b.vblock(b.arg("op")), self.T(name))
        a_nonreal = lambda: b.or_(
            b.or_(b.not_(b.eq(AI, self.N(0))), b.not_(b.eq(AJ, self.N(0)))),
            b.not_(b.eq(AK, self.N(0))))
        b_nonreal = lambda: b.or_(
            b.or_(b.not_(b.eq(BI, self.N(0))), b.not_(b.eq(BJ, self.N(0)))),
            b.not_(b.eq(BK, self.N(0))))
        unary = b.or_(b.or_(op_is("u-"), op_is("!")), op_is("%"))

        def pop_a():
            return [
                self.call(self.p["popVal"], []),
                self.setv("a", self.V("pv")), self.setv("aIm", self.V("pvIm")),
                self.setv("aJ", self.V("pvJ")), self.setv("aK", self.V("pvK")),
            ]

        def pop_b():
            return [
                self.call(self.p["popVal"], []),
                self.setv("b", self.V("pv")), self.setv("bIm", self.V("pvIm")),
                self.setv("bJ", self.V("pvJ")), self.setv("bK", self.V("pvK")),
            ]

        def zero():
            return [self.setv("rIm", self.N(0)), self.setv("rJ", self.N(0)),
                    self.setv("rK", self.N(0))]

        def real_unary():
            return [
                b.ifelse(op_is("u-"), [self.setv("r", b.sub(self.N(0), A)), *zero()],
                         [b.ifelse(op_is("!"), [
                             self.call(self.p["fact"], [self.B("a")]),
                             b.if_(self.eqn("err", 0), [
                                 self.setv("r", self.V("fOut")),
                                 self.setv("rIm", self.V("fOutIm")),
                                 self.setv("rJ", self.V("fOutJ")),
                                 self.setv("rK", self.V("fOutK")),
                             ])],
                             [b.ifelse(op_is("%"), [
                                 self.setv("r", b.div(A, self.N(100))),
                                 self.setv("rIm", b.div(AI, self.N(100))),
                                 self.setv("rJ", b.div(AJ, self.N(100))),
                                 self.setv("rK", b.div(AK, self.N(100))),
                             ], self.err("Bad op"))])]),
            ]

        def real_binary():
            return [
                b.ifelse(op_is("+"), [self.setv("r", b.add(A, B)), *zero()],
                         [b.ifelse(op_is("-"), [self.setv("r", b.sub(A, B)), *zero()],
                                   [b.ifelse(op_is("*"), [self.setv("r", b.mul(A, B)), *zero()],
                                             [b.ifelse(op_is("/"),
                                                       [b.ifelse(self.eqn("b", 0), self.err("Division by zero"),
                                                                 [self.setv("r", b.div(A, B)), *zero()])],
                                                       [b.ifelse(op_is("^"), [
                                                           self.call(self.p["powr"], [self.B("a"), self.B("b")]),
                                                           b.if_(self.eqn("err", 0), [
                                                               self.setv("r", self.V("pOut2")),
                                                               self.setv("rIm", self.V("pOut2Im")),
                                                               self.setv("rJ", self.V("pOut2J")),
                                                               self.setv("rK", self.V("pOut2K")),
                                                           ]),
                                                       ], self.err("Bad op"))])])])]),
            ]

        real_neg_frac = b.and_(op_is("^"),
                               b.and_(b.lt(A, self.N(0)),
                                      b.not_(b.eq(B, b.vblock(b.round_(B))))))
        needs_q = b.or_(b.or_(a_nonreal(), b_nonreal()), real_neg_frac)
        body = [
            b.if_(self.eqn("err", 0), [
                b.ifelse(unary, [
                    *pop_a(),
                    b.if_(self.eqn("err", 0), [
                        b.ifelse(a_nonreal(),
                                 [self.call(self.p["applyOpQ"], [b.vblock(b.arg("op"))])],
                                 real_unary()),
                    ]),
                ], [
                    *pop_b(), *pop_a(),
                    b.if_(self.eqn("err", 0), [
                        b.ifelse(needs_q,
                                 [self.call(self.p["applyOpQ"], [b.vblock(b.arg("op"))])],
                                 real_binary()),
                    ]),
                ]),
                b.if_(self.eqn("err", 0), [
                    b.if_(b.or_(b.or_(b.not_(b.eq(self.V("r"), self.V("r"))),
                                     b.not_(b.eq(self.V("rIm"), self.V("rIm")))),
                               b.or_(b.not_(b.eq(self.V("rJ"), self.V("rJ"))),
                                     b.not_(b.eq(self.V("rK"), self.V("rK"))))),
                          self.err("Domain")),
                    b.if_(b.and_(self.eqn("err", 0),
                                 b.or_(b.or_(b.gt(b.mathop("abs", self.V("r")), self.N(1e308)),
                                             b.gt(b.mathop("abs", self.V("rIm")), self.N(1e308))),
                                       b.or_(b.gt(b.mathop("abs", self.V("rJ")), self.N(1e308)),
                                             b.gt(b.mathop("abs", self.V("rK")), self.N(1e308))))),
                          self.err("Overflow")),
                    b.if_(self.eqn("err", 0), [
                        b.list_add("vals", self.V("r")),
                        b.list_add("valsIm", self.V("rIm")),
                        b.list_add("valsJ", self.V("rJ")),
                        b.list_add("valsK", self.V("rK")),
                    ]),
                ]),
            ])
        ]
        p.body(body)

    # ------------------------------------------------------------- applyFunc
    def iff(self, cond, then, other=None):
        return self.b.ifelse(cond, then, other or [])

    def gen_apply_func(self):
        b = self.b
        p = self.p
        branches = []
        A = lambda: self.V("a")
        Bv = lambda: self.V("b")
        arg = lambda i: b.vblock(b.list_item("args", self.N(i)))
        fixed = lambda n: b.eq(self.V("fargc"), self.N(n))
        variadic = lambda: b.gt(self.V("fargc"), self.N(0))
        res = lambda v: self.result(v)
        bad = lambda: self.err("Bad args")
        dom = lambda m: self.err(m)
        iff = self.iff
        deg2rad = math.pi / 180.0
        rad2deg = 180.0 / math.pi
        integer = lambda v: b.eq(v, b.vblock(b.round_(v)))
        even = lambda v: b.eq(b.mul(b.mathop("floor", b.div(v, self.N(2))), self.N(2)), v)

        def one(name, body):
            branches.append((name, [iff(fixed(1), body, bad())]))

        # ---- trig
        one("sin", [iff(self.eqv("angleMode", "DEG"),
                        res(b.mathop("sin", A())),
                        res(b.mathop("sin", b.mul(A(), self.N(rad2deg)))))])
        one("cos", [iff(self.eqv("angleMode", "DEG"),
                        res(b.mathop("cos", A())),
                        res(b.mathop("cos", b.mul(A(), self.N(rad2deg)))))])
        one("tan", [
            iff(self.eqv("angleMode", "DEG"),
                [self.setv("sv", b.mathop("sin", A())),
                 self.setv("cv", b.mathop("cos", A()))],
                [self.setv("sv", b.mathop("sin", b.mul(A(), self.N(rad2deg)))),
                 self.setv("cv", b.mathop("cos", b.mul(A(), self.N(rad2deg))))]),
            iff(b.lt(b.mathop("abs", self.V("cv")), self.N(1e-12)),
                dom("Undefined"),
                res(b.div(self.V("sv"), self.V("cv")))),
        ])
        one("asin", [iff(b.gt(b.mathop("abs", A()), self.N(1)), dom("Domain"),
                        [iff(self.eqv("angleMode", "DEG"),
                             res(b.mathop("asin", A())),
                             res(b.mul(b.mathop("asin", A()), self.N(deg2rad))))])])
        one("acos", [iff(b.gt(b.mathop("abs", A()), self.N(1)), dom("Domain"),
                        [iff(self.eqv("angleMode", "DEG"),
                             res(b.mathop("acos", A())),
                             res(b.mul(b.mathop("acos", A()), self.N(deg2rad))))])])
        one("atan", [iff(self.eqv("angleMode", "DEG"),
                         res(b.mathop("atan", A())),
                         res(b.mul(b.mathop("atan", A()), self.N(deg2rad))))])
        atan2_body = [
            iff(b.gt(Bv(), self.N(0)),
                [self.setv("r", b.mathop("atan", b.div(A(), Bv())))],
                [iff(b.lt(Bv(), self.N(0)),
                     [self.setv("r", b.mathop("atan", b.div(A(), Bv()))),
                      iff(b.not_(b.lt(A(), self.N(0))),
                          [self.changev("r", self.N(180))],
                          [self.changev("r", self.N(-180))])],
                     [iff(b.gt(A(), self.N(0)), [self.setv("r", self.N(90))],
                          [iff(b.lt(A(), self.N(0)), [self.setv("r", self.N(-90))],
                               [self.setv("r", self.N(0))])])])]),
            iff(self.eqv("angleMode", "DEG"), res(self.V("r")),
                res(b.mul(self.V("r"), self.N(deg2rad)))),
        ]
        branches.append(("atan2", [iff(fixed(2), atan2_body, bad())]))
        # ---- hyperbolics
        one("sinh", res(b.div(b.sub(b.mathop("e ^", A()), b.mathop("e ^", b.sub(self.N(0), A()))), self.N(2))))
        one("cosh", res(b.div(b.add(b.mathop("e ^", A()), b.mathop("e ^", b.sub(self.N(0), A()))), self.N(2))))
        one("tanh", [
            iff(b.gt(A(), self.N(20)), res(self.N(1)),
                [iff(b.lt(A(), self.N(-20)), res(self.N(-1)),
                     res(b.div(b.sub(b.mathop("e ^", A()), b.mathop("e ^", b.sub(self.N(0), A()))),
                             b.add(b.mathop("e ^", A()), b.mathop("e ^", b.sub(self.N(0), A()))))))])])
        one("asinh", [
            self.setv("r", b.mathop("ln", b.add(b.mathop("abs", A()),
                                                b.mathop("sqrt", b.add(b.mul(A(), A()), self.N(1)))))),
            iff(b.lt(A(), self.N(0)), [self.setv("r", b.sub(self.N(0), self.V("r")))], []),
            res(self.V("r")),
        ])
        one("acosh", [iff(b.lt(A(), self.N(1)), dom("Domain"),
                          res(b.mathop("ln", b.add(A(), b.mathop("sqrt", b.sub(b.mul(A(), A()), self.N(1)))))))])
        one("atanh", [iff(b.not_(b.lt(b.mathop("abs", A()), self.N(1))), dom("Domain"),
                          res(b.mul(self.N(0.5), b.mathop("ln", b.div(b.add(self.N(1), A()),
                                                                       b.sub(self.N(1), A()))))))])
        # ---- logs / powers
        one("ln", [iff(b.not_(b.gt(A(), self.N(0))), dom("Domain"), res(b.mathop("ln", A())))])
        log_body = [
            iff(fixed(1),
                [iff(b.not_(b.gt(A(), self.N(0))), dom("Domain"), res(b.mathop("log", A())))],
                [iff(b.or_(b.not_(b.gt(A(), self.N(0))), self.eqn("a", 1)), dom("Domain"),
                     [iff(b.not_(b.gt(Bv(), self.N(0))), dom("Domain"),
                          res(b.div(b.mathop("ln", Bv()), b.mathop("ln", A()))))])]),
        ]
        branches.append(("log", [iff(b.or_(fixed(1), fixed(2)), log_body, bad())]))
        one("log2", [iff(b.not_(b.gt(A(), self.N(0))), dom("Domain"),
                         res(b.div(b.mathop("ln", A()), b.mathop("ln", self.N(2)))))])
        one("exp", res(b.mathop("e ^", A())))
        one("sqrt", [iff(b.lt(A(), self.N(0)), dom("Domain"), res(b.mathop("sqrt", A())))])
        one("cbrt", [
            iff(b.lt(A(), self.N(0)),
                [self.call(p["powr"], [b.vblock(b.mathop("abs", A())), self.N(1 / 3)]),
                 iff(self.eqn("err", 0), [self.setv("r", b.sub(self.N(0), self.V("pOut2")))], [])],
                [self.call(p["powr"], [self.B("a"), self.N(1 / 3)]),
                 iff(self.eqn("err", 0), [self.setv("r", self.V("pOut2"))], [])]),
            iff(self.eqn("err", 0), res(self.V("r")), []),
        ])
        one("abs", res(b.mathop("abs", A())))
        one("floor", res(b.mathop("floor", A())))
        one("ceil", res(b.mathop("ceiling", A())))
        one("round", res(b.vblock(b.round_(A()))))
        one("sign", [iff(b.gt(A(), self.N(0)), res(self.N(1)),
                         [iff(b.lt(A(), self.N(0)), res(self.N(-1)), res(self.N(0)))])])
        one("trunc", [iff(b.gt(A(), self.N(0)), res(b.mathop("floor", A())),
                          res(b.mathop("ceiling", A())))])
        one("deg", res(b.mul(A(), self.N(rad2deg))))
        one("rad", res(b.mul(A(), self.N(deg2rad))))
        branches.append(("mod", [iff(fixed(2),
                                     [iff(self.eqn("b", 0), dom("Division by zero"),
                                          res(b.sub(A(), b.mul(Bv(), b.mathop("floor", b.div(A(), Bv()))))))],
                                     bad())]))
        branches.append(("pow", [iff(fixed(2), [
            self.call(p["powr"], [self.B("a"), self.B("b")]),
            iff(self.eqn("err", 0), res(self.V("pOut2")), []),
        ], bad())]))
        root_body = [
            iff(self.eqn("b", 0), dom("Domain"),
                [iff(b.and_(b.lt(A(), self.N(0)), even(Bv())), dom("Domain"),
                     [iff(b.lt(A(), self.N(0)),
                          [self.call(p["powr"], [b.vblock(b.mathop("abs", A())), b.div(self.N(1), Bv())]),
                           iff(self.eqn("err", 0), [self.setv("r", b.sub(self.N(0), self.V("pOut2")))], [])],
                          [self.call(p["powr"], [self.B("a"), b.div(self.N(1), Bv())]),
                           iff(self.eqn("err", 0), [self.setv("r", self.V("pOut2"))], [])]),
                      iff(self.eqn("err", 0), res(self.V("r")), [])])]),
        ]
        branches.append(("root", [iff(fixed(2), root_body, bad())]))
        # ---- combinatorics
        ncr_body = [
            iff(b.or_(b.not_(integer(A())), b.not_(integer(Bv()))), dom("Integer only"),
                [iff(b.or_(b.lt(A(), self.N(0)), b.or_(b.lt(Bv(), self.N(0)), b.gt(Bv(), A()))),
                     dom("Domain"),
                     [iff(b.gt(Bv(), b.sub(A(), Bv())), [self.setv("b", b.sub(A(), Bv()))], []),
                      iff(b.gt(Bv(), self.N(1000)), dom("Too large"), []),
                      iff(self.eqn("err", 0), [
                          self.setv("fr", self.N(1)),
                          self.setv("fi", self.N(1)),
                          b.repeat(Bv(), [
                              self.setv("fr", b.div(b.mul(self.V("fr"),
                                                          b.add(b.sub(A(), Bv()), self.V("fi"))),
                                                    self.V("fi"))),
                              self.changev("fi", self.N(1)),
                          ]),
                          self.setv("fr", b.vblock(b.round_(self.V("fr")))),
                          res(self.V("fr")),
                      ], [])])])]
        branches.append(("ncr", [iff(fixed(2), ncr_body, bad())]))
        npr_body = [
            iff(b.or_(b.not_(integer(A())), b.not_(integer(Bv()))), dom("Integer only"),
                [iff(b.or_(b.lt(A(), self.N(0)), b.or_(b.lt(Bv(), self.N(0)), b.gt(Bv(), A()))),
                     dom("Domain"),
                     [iff(b.gt(Bv(), self.N(1000)), dom("Too large"), []),
                      iff(self.eqn("err", 0), [
                          self.setv("fr", self.N(1)),
                          self.setv("fi", self.N(1)),
                          b.repeat(Bv(), [
                              self.setv("fr", b.mul(self.V("fr"), b.add(b.sub(A(), Bv()), self.V("fi")))),
                              self.changev("fi", self.N(1)),
                          ]),
                          self.setv("fr", b.vblock(b.round_(self.V("fr")))),
                          res(self.V("fr")),
                      ], [])])])]
        branches.append(("npr", [iff(fixed(2), npr_body, bad())]))
        # ---- variadic
        branches.append(("min", [iff(variadic(), [
            self.setv("fr", arg(1)),
            self.setv("fi", self.N(2)),
            b.repeat(b.sub(self.V("fargc"), self.N(1)), [
                iff(b.lt(b.vblock(b.list_item("args", self.V("fi"))), self.V("fr")),
                    [self.setv("fr", b.vblock(b.list_item("args", self.V("fi"))))], []),
                self.changev("fi", self.N(1)),
            ]),
            res(self.V("fr")),
        ], bad())]))
        branches.append(("max", [iff(variadic(), [
            self.setv("fr", arg(1)),
            self.setv("fi", self.N(2)),
            b.repeat(b.sub(self.V("fargc"), self.N(1)), [
                iff(b.gt(b.vblock(b.list_item("args", self.V("fi"))), self.V("fr")),
                    [self.setv("fr", b.vblock(b.list_item("args", self.V("fi"))))], []),
                self.changev("fi", self.N(1)),
            ]),
            res(self.V("fr")),
        ], bad())]))
        for name, fn in (("sum", None), ("avg", None), ("hypot", None)):
            pass
        branches.append(("sum", [iff(variadic(), [
            self.setv("fr", self.N(0)),
            self.setv("fi", self.N(1)),
            b.repeat(self.V("fargc"), [
                self.setv("fr", b.add(self.V("fr"), b.vblock(b.list_item("args", self.V("fi"))))),
                self.changev("fi", self.N(1)),
            ]),
            res(self.V("fr")),
        ], bad())]))
        branches.append(("avg", [iff(variadic(), [
            self.setv("fr", self.N(0)),
            self.setv("fi", self.N(1)),
            b.repeat(self.V("fargc"), [
                self.setv("fr", b.add(self.V("fr"), b.vblock(b.list_item("args", self.V("fi"))))),
                self.changev("fi", self.N(1)),
            ]),
            res(b.div(self.V("fr"), self.V("fargc"))),
        ], bad())]))
        branches.append(("hypot", [iff(variadic(), [
            self.setv("fr", self.N(0)),
            self.setv("fi", self.N(1)),
            b.repeat(self.V("fargc"), [
                self.setv("fr", b.add(self.V("fr"),
                                      b.mul(b.vblock(b.list_item("args", self.V("fi"))),
                                            b.vblock(b.list_item("args", self.V("fi")))))),
                self.changev("fi", self.N(1)),
            ]),
            res(b.mathop("sqrt", self.V("fr"))),
        ], bad())]))
        branches.append(("gcd", [iff(variadic(), [
            self.setv("u2", arg(1)),
            iff(b.not_(integer(self.V("u2"))), dom("Integer only"), []),
            self.setv("fr", b.mathop("abs", self.V("u2"))),
            self.setv("fi", self.N(2)),
            b.repeat(b.sub(self.V("fargc"), self.N(1)), [
                self.setv("u2", b.vblock(b.list_item("args", self.V("fi")))),
                iff(b.not_(integer(self.V("u2"))), dom("Integer only"), []),
                iff(self.eqn("err", 0), [
                    self.call(p["gcd2"], [self.B("fr"), self.B("u2")]),
                    self.setv("fr", self.V("g2Out")),
                ], []),
                self.changev("fi", self.N(1)),
            ]),
            iff(self.eqn("err", 0), res(self.V("fr")), []),
        ], bad())]))
        branches.append(("lcm", [iff(variadic(), [
            self.setv("u4", arg(1)),
            iff(b.not_(integer(self.V("u4"))), dom("Integer only"), []),
            self.setv("fr", b.mathop("abs", self.V("u4"))),
            self.setv("fi", self.N(2)),
            b.repeat(b.sub(self.V("fargc"), self.N(1)), [
                self.setv("u4", b.vblock(b.list_item("args", self.V("fi")))),
                iff(b.not_(integer(self.V("u4"))), dom("Integer only"), []),
                iff(self.eqn("err", 0), [
                    iff(b.or_(self.eqn("fr", 0), self.eqn("u4", 0)),
                        [self.setv("fr", self.N(0))],
                        [self.call(p["gcd2"], [self.B("fr"), self.B("u4")]),
                         self.setv("fr", b.div(b.mathop("abs", b.mul(self.V("fr"), self.V("u4"))),
                                               self.V("g2Out")))]),
                ], []),
                self.changev("fi", self.N(1)),
            ]),
            iff(self.eqn("err", 0), res(self.V("fr")), []),
        ], bad())]))
        branches.append(("rand", [iff(fixed(0),
                                      res(b.div(b.random(self.N(0), self.N(1000000)), self.N(1000000))),
                                      bad())]))
        branches.append(("randint", [iff(fixed(2), [
            self.setv("a", b.vblock(b.round_(A()))),
            self.setv("b", b.vblock(b.round_(Bv()))),
            iff(b.gt(self.V("a"), self.V("b")), [
                self.setv("r", self.V("a")),
                self.setv("a", self.V("b")),
                self.setv("b", self.V("r")),
            ], []),
            res(b.random(self.V("a"), self.V("b"))),
        ], bad())]))
        one("gamma", [self.call(p["gamma"], [self.B("a")]),
                      iff(self.eqn("err", 0), res(self.V("gOut")), [])])
        one("fact", [self.call(p["fact"], [self.B("a")]),
                     iff(self.eqn("err", 0), res(self.V("fOut")), [])])
        # ---- dispatch
        cur = self.err("Unknown fn")
        for name, body in reversed(branches):
            cur = [b.ifelse(self.eqv("fname", name), body, cur)]
        dispatch = cur[0]
        A = lambda: self.V("a")
        Bv = lambda: self.V("b")
        fn = lambda name: self.eqv("fname", name)
        fixedn = lambda n: b.eq(self.V("fargc"), self.N(n))
        abs_a = lambda: b.mathop("abs", A())
        domain_q = b.or_(
            b.or_(
                b.and_(fn("sqrt"), b.lt(A(), self.N(0))),
                b.and_(b.or_(fn("ln"), b.or_(fn("log"), fn("log2"))),
                       b.lt(A(), self.N(0)))),
            b.or_(
                b.and_(b.and_(fn("log"), fixedn(2)),
                       b.or_(b.lt(A(), self.N(0)), b.lt(Bv(), self.N(0)))),
                b.or_(
                    b.and_(b.or_(fn("asin"), fn("acos")),
                           b.gt(abs_a(), self.N(1))),
                    b.or_(
                        b.and_(fn("acosh"), b.lt(A(), self.N(1))),
                        b.and_(fn("atanh"),
                               b.not_(b.lt(abs_a(), self.N(1))))))))
        pow_q = b.and_(fn("pow"),
                       b.and_(b.lt(A(), self.N(0)),
                              b.not_(b.eq(Bv(), b.vblock(b.round_(Bv()))))))
        root_q = b.and_(fn("root"),
                        b.and_(b.lt(A(), self.N(0)),
                               b.eq(b.mul(b.mathop("floor", b.div(Bv(), self.N(2))), self.N(2)), Bv())))
        needs_q = b.or_(b.or_(self.eqn("qAny", 1), domain_q), b.or_(pow_q, root_q))
        push = iff(b.and_(self.eqn("err", 0), self.eqn("haveResult", 1)), [
            iff(b.or_(b.or_(b.not_(b.eq(self.V("fr"), self.V("fr"))),
                            b.not_(b.eq(self.V("frIm"), self.V("frIm")))),
                      b.or_(b.not_(b.eq(self.V("frJ"), self.V("frJ"))),
                            b.not_(b.eq(self.V("frK"), self.V("frK"))))),
                self.err("Domain"), []),
            iff(b.and_(self.eqn("err", 0),
                       b.or_(b.or_(b.gt(b.mathop("abs", self.V("fr")), self.N(1e308)),
                                   b.gt(b.mathop("abs", self.V("frIm")), self.N(1e308))),
                             b.or_(b.gt(b.mathop("abs", self.V("frJ")), self.N(1e308)),
                                   b.gt(b.mathop("abs", self.V("frK")), self.N(1e308))))),
                self.err("Overflow"), []),
            iff(self.eqn("err", 0), [
                b.list_add("vals", self.V("fr")),
                b.list_add("valsIm", self.V("frIm")),
                b.list_add("valsJ", self.V("frJ")),
                b.list_add("valsK", self.V("frK")),
            ], []),
        ], [])
        body = [
            iff(self.eqn("err", 0), [
                self.setv("fname", b.vblock(b.arg("fn"))),
                self.setv("fargc", b.vblock(b.arg("ac"))),
                iff(b.lt(b.vblock(b.list_len("vals")), self.V("fargc")), self.err("Stack"), [
                    b.list_delete_all("args"), b.list_delete_all("argsIm"),
                    b.list_delete_all("argsJ"), b.list_delete_all("argsK"),
                    self.setv("qAny", self.N(0)),
                    b.repeat(self.V("fargc"), [
                        self.call(self.p["popVal"], []),
                        iff(self.eqn("err", 0), [
                            b.list_insert("args", self.V("pv"), self.N(1)),
                            b.list_insert("argsIm", self.V("pvIm"), self.N(1)),
                            b.list_insert("argsJ", self.V("pvJ"), self.N(1)),
                            b.list_insert("argsK", self.V("pvK"), self.N(1)),
                            iff(b.or_(b.or_(b.not_(self.eqn("pvIm", 0)),
                                             b.not_(self.eqn("pvJ", 0))),
                                      b.not_(self.eqn("pvK", 0))),
                                [self.setv("qAny", self.N(1))], []),
                        ], []),
                    ]),
                    self.setv("a", b.vblock(b.list_item("args", self.N(1)))),
                    self.setv("aIm", b.vblock(b.list_item("argsIm", self.N(1)))),
                    self.setv("aJ", b.vblock(b.list_item("argsJ", self.N(1)))),
                    self.setv("aK", b.vblock(b.list_item("argsK", self.N(1)))),
                    self.setv("b", b.vblock(b.list_item("args", self.N(2)))),
                    self.setv("bIm", b.vblock(b.list_item("argsIm", self.N(2)))),
                    self.setv("bJ", b.vblock(b.list_item("argsJ", self.N(2)))),
                    self.setv("bK", b.vblock(b.list_item("argsK", self.N(2)))),
                    self.setv("haveResult", self.N(0)),
                    iff(needs_q,
                        [self.call(self.p["qFunc"], [self.B("fname"), self.B("fargc")])],
                        [dispatch]),
                    push,
                ]),
            ], []),
        ]
        self.p["applyFunc"].body(body)

    # -------------------------------------------------------------- evaluate
    def gen_evaluate(self):
        b = self.b
        p = self.p["evaluate"]
        # parse loop
        ch = self.V("ch")
        src = self.V("src")
        digit_here = self.digit(ch)
        dot_num = b.and_(self.eqv("ch", "."),
                         self.digit(b.vblock(b.letter(b.add(self.V("i"), self.N(1)), src))))
        num_cond = b.or_(digit_here, dot_num)
        alpha_cond = self.alpha(ch)
        op_cond = b.gt(b.vblock(b.list_itemnum("opChars", ch)), self.N(0))

        # --- number token
        def scan_digits():
            return b.repeat_until(
                b.or_(b.gt(self.V("i"), self.V("n")),
                      b.not_(self.digit(b.vblock(b.letter(self.V("i"), src))))),
                [self.setv("numStr", b.join(self.V("numStr"), b.vblock(b.letter(self.V("i"), src)))),
                 self.changev("i", self.N(1))])
        number_body = [
            b.if_(self.is_value(), [
                b.ifelse(self.eqv("prev", "num"),
                         self.err("Missing operator"),
                         [self.call(self.p["pushBinary"], [self.T("*")])]),
            ]),
            b.if_(self.eqn("err", 0), [
                self.setv("numStr", self.T("")),
                self.setv("hasDot", self.N(0)),
                b.if_(self.eqv("ch", "."), [
                    self.setv("numStr", self.T(".")),
                    self.setv("hasDot", self.N(1)),
                    self.changev("i", self.N(1)),
                ]),
                scan_digits(),
                b.if_(b.and_(b.and_(b.not_(b.gt(self.V("i"), self.V("n"))),
                                    self.eqv("ch", "")),
                             self.eqn("hasDot", 0)), []),  # placeholder
            ]),
        ]
        # build number body properly (dot + exponent)
        number_body = [
            b.if_(self.is_value(), [
                b.ifelse(self.eqv("prev", "num"),
                         self.err("Missing operator"),
                         [self.call(self.p["pushBinary"], [self.T("*")])]),
            ]),
            b.if_(self.eqn("err", 0), [
                self.setv("numStr", self.T("")),
                self.setv("hasDot", self.N(0)),
                b.if_(self.eqv("ch", "."), [
                    self.setv("numStr", self.T(".")),
                    self.setv("hasDot", self.N(1)),
                    self.changev("i", self.N(1)),
                ]),
                scan_digits(),
                b.if_(b.and_(b.not_(b.gt(self.V("i"), self.V("n"))),
                             b.eq(b.vblock(b.letter(self.V("i"), src)), self.T("."))),
                      [b.if_(self.eqn("hasDot", 0), [
                          self.setv("numStr", b.join(self.V("numStr"), self.T("."))),
                          self.setv("hasDot", self.N(1)),
                          self.changev("i", self.N(1)),
                          scan_digits(),
                      ])]),
                b.if_(b.and_(b.not_(b.gt(self.V("i"), self.V("n"))),
                             b.eq(b.vblock(b.letter(self.V("i"), src)), self.T("e"))),
                      [self.setv("la", b.vblock(b.letter(b.add(self.V("i"), self.N(1)), src))),
                       b.if_(b.or_(self.digit(self.V("la")),
                                   b.and_(b.or_(self.eqv("la", "+"), self.eqv("la", "-")),
                                          self.digit(b.vblock(b.letter(b.add(self.V("i"), self.N(2)), src))))),
                             [
                                 self.setv("numStr", b.join(self.V("numStr"), self.T("e"))),
                                 self.changev("i", self.N(1)),
                                 b.if_(b.or_(self.eqv("la", "+"), self.eqv("la", "-")), [
                                     self.setv("numStr", b.join(self.V("numStr"), self.V("la"))),
                                     self.changev("i", self.N(1)),
                                 ]),
                                 scan_digits(),
                             ])]),
                b.ifelse(self.eqv("numStr", "."),
                         self.err("Bad number"),
                         [
                             self.setv("numVal", b.add(self.V("numStr"), self.N(0))),
                             b.ifelse(b.gt(b.mathop("abs", self.V("numVal")), self.N(1e308)),
                                      self.err("Overflow"),
                                      [
                                          self.call(self.p["pushNumber"], [self.B("numStr"), self.T("0"), self.T("0"), self.T("0")]),
                                          self.setv("prev", self.T("num")),
                                      ]),
                         ]),
            ]),
        ]
        # --- identifier token
        scan_id = b.repeat_until(
            b.or_(b.gt(self.V("i"), self.V("n")),
                  b.not_(self.alnum(lambda: b.vblock(b.letter(self.V("i"), src))))),
            [self.setv("id", b.join(self.V("id"), b.vblock(b.letter(self.V("i"), src)))),
             self.changev("i", self.N(1))])
        skip_spaces = b.repeat_until(
            b.or_(b.gt(self.V("i"), self.V("n")),
                  b.not_(self.eqv("ch", ""))),
            [self.changev("i", self.N(1))])
        # fix skip_spaces condition: stop at non-space
        skip_spaces = b.repeat_until(
            b.or_(b.gt(self.V("i"), self.V("n")),
                  b.not_(b.eq(b.vblock(b.letter(self.V("i"), src)), self.T(" ")))),
            [self.changev("i", self.N(1))])
        ident_body = [
            b.if_(self.is_value(), [
                self.call(self.p["pushBinary"], [self.T("*")]),
            ]),
            b.if_(self.eqn("err", 0), [
                self.setv("id", self.T("")),
                scan_id,
                b.ifelse(b.gt(b.vblock(b.list_itemnum("constNames", self.B("id"))), self.N(0)),
                         [
                             self.call(self.p["pushConst"], [self.B("id")]),
                             self.setv("prev", self.T("const")),
                         ],
                         [
                             skip_spaces,
                             b.ifelse(b.and_(b.not_(b.gt(self.V("i"), self.V("n"))),
                                             b.eq(b.vblock(b.letter(self.V("i"), src)), self.T("("))),
                                      [
                                          self.call(self.p["pushFunc"], [self.B("id")]),
                                          self.call(self.p["pushParen"], [self.N(1)]),
                                          self.changev("i", self.N(1)),
                                          self.setv("prev", self.T("lparen")),
                                      ],
                                      self.err("Expected (")),
                         ]),
            ]),
        ]
        # --- parentheses
        lparen_body = [
            b.if_(self.is_value(), [self.call(self.p["pushBinary"], [self.T("*")])]),
            b.if_(self.eqn("err", 0), [
                self.call(self.p["pushParen"], [self.N(0)]),
                self.setv("prev", self.T("lparen")),
                self.changev("i", self.N(1)),
            ]),
        ]
        rparen_body = [
            self.call(self.p["closeParen"], []),
            b.if_(self.eqn("err", 0), [self.changev("i", self.N(1))]),
        ]
        # --- comma
        comma_body = [
            b.ifelse(self.b.not_(self.is_value()),
                     self.err("Missing arg"),
                     [b.ifelse(b.or_(self.b.eq(self.b.vblock(b.list_len("parenCount")), self.N(0)),
                                     self.b.eq(self.last("parenFunc"), self.N(0))),
                               self.err("Bad comma"),
                               [
                                   b.repeat_until(b.or_(self.b.eq(self.b.vblock(b.list_len("ops")), self.N(0)),
                                                        self.b.eq(self.last("ops"), self.T("("))),
                                                  [self.call(self.p["popOp"], [])]),
                                   b.ifelse(self.b.eq(self.b.vblock(b.list_len("ops")), self.N(0)),
                                            self.err("Bad comma"),
                                            [
                                                b.list_replace("parenCount", self.b.vblock(b.list_len("parenCount")),
                                                               b.add(self.last("parenCount"), self.N(1))),
                                                b.list_replace("parenEmpty", self.b.vblock(b.list_len("parenEmpty")), self.N(0)),
                                                self.setv("prev", self.T("comma")),
                                                self.changev("i", self.N(1)),
                                            ]),
                               ])])]
        # --- operators / postfix
        unary = b.and_(b.or_(self.eqv("ch", "-"), self.eqv("ch", "+")),
                       b.or_(b.or_(self.eqv("prev", "none"), self.eqv("prev", "op")),
                             b.or_(self.eqv("prev", "lparen"), self.eqv("prev", "comma"))))
        postfix = b.or_(self.eqv("ch", "!"), self.eqv("ch", "%"))
        op_body = [
            b.ifelse(postfix,
                     [b.ifelse(self.b.not_(self.is_value()),
                               self.err("Missing operand"),
                               [self.call(self.p["pushOut"], [self.V("ch"), self.T("o"), self.N(0)]),
                                self.setv("prev", self.T("postfix")),
                                self.changev("i", self.N(1))])],
                     [b.ifelse(unary,
                               [b.ifelse(self.eqv("ch", "-"),
                                         [self.call(self.p["pushOp"], [self.T("u-")])],
                                         []),
                                self.setv("prev", self.T("op")),
                                self.changev("i", self.N(1))],
                               [b.ifelse(self.b.not_(self.is_value()),
                                         self.err("Missing operand"),
                                         [self.call(self.p["pushBinary"], [self.V("ch")]),
                                          self.setv("prev", self.T("op")),
                                          self.changev("i", self.N(1))])])])
        ]
        # assemble branch chain (order: space, number, ident, (, ), comma, op, else)
        bad_char = self.err("Bad char")
        chain = bad_char
        chain = [b.ifelse(op_cond, op_body, chain)]
        chain = [b.ifelse(self.eqv("ch", ","), comma_body, chain)]
        chain = [b.ifelse(self.eqv("ch", ")"), rparen_body, chain)]
        chain = [b.ifelse(self.eqv("ch", "("), lparen_body, chain)]
        chain = [b.ifelse(alpha_cond, ident_body, chain)]
        chain = [b.ifelse(num_cond, number_body, chain)]
        chain = [b.ifelse(self.eqv("ch", " "), [self.changev("i", self.N(1))], chain)]
        parse_loop = b.repeat_until(
            b.or_(b.gt(self.V("i"), self.V("n")), self.eqn("err", 1)),
            [self.setv("ch", b.vblock(b.letter(self.V("i"), src)))] + chain)

        # --- RPN evaluation
        rpn_body = [
            b.list_delete_all("vals"),
            b.list_delete_all("valsIm"), b.list_delete_all("valsJ"), b.list_delete_all("valsK"),
            self.setv("i", self.N(1)),
            b.repeat(self.b.vblock(b.list_len("out")), [
                b.if_(self.eqn("err", 0), [
                    self.setv("tok", b.vblock(b.list_item("out", self.V("i")))),
                    self.setv("kind", b.vblock(b.list_item("outKind", self.V("i")))),
                    b.ifelse(self.eqv("kind", "n"),
                             [
                                 self.setv("numVal", b.add(self.V("tok"), self.N(0))),
                                 self.setv("numIm", b.vblock(b.list_item("outIm", self.V("i")))),
                                 self.setv("numJ", b.vblock(b.list_item("outJ", self.V("i")))),
                                 self.setv("numK", b.vblock(b.list_item("outK", self.V("i")))),
                                 b.ifelse(b.or_(b.or_(b.gt(b.mathop("abs", self.V("numVal")), self.N(1e308)),
                                                      b.gt(b.mathop("abs", self.V("numIm")), self.N(1e308))),
                                                b.or_(b.gt(b.mathop("abs", self.V("numJ")), self.N(1e308)),
                                                      b.gt(b.mathop("abs", self.V("numK")), self.N(1e308)))),
                                          self.err("Overflow"),
                                          [b.list_add("vals", self.V("numVal")),
                                           b.list_add("valsIm", self.V("numIm")),
                                           b.list_add("valsJ", self.V("numJ")),
                                           b.list_add("valsK", self.V("numK"))]),
                             ],
                             [b.ifelse(self.eqv("kind", "o"),
                                       [self.call(self.p["applyOp"], [self.B("tok")])],
                                       [self.call(self.p["applyFunc"], [self.B("tok"),
                                                                        self.b.vblock(b.list_item("outArgc", self.V("i")))])])]),
                ]),
                self.changev("i", self.N(1)),
            ]),
            b.if_(self.eqn("err", 0), [
                b.ifelse(b.not_(self.b.eq(self.b.vblock(b.list_len("vals")), self.N(1))),
                         self.err("Syntax"),
                         [
                             self.setv("result", b.vblock(b.list_item("vals", self.N(1)))),
                             self.setv("resultIm", b.vblock(b.list_item("valsIm", self.N(1)))),
                             self.setv("resultJ", b.vblock(b.list_item("valsJ", self.N(1)))),
                             self.setv("resultK", b.vblock(b.list_item("valsK", self.N(1)))),
                             b.if_(b.or_(b.or_(b.not_(b.eq(self.V("result"), self.V("result"))),
                                               b.not_(b.eq(self.V("resultIm"), self.V("resultIm")))),
                                         b.or_(b.not_(b.eq(self.V("resultJ"), self.V("resultJ"))),
                                               b.not_(b.eq(self.V("resultK"), self.V("resultK"))))),
                                   self.err("Domain")),
                             b.if_(b.and_(self.eqn("err", 0),
                                          b.or_(b.or_(b.gt(b.mathop("abs", self.V("result")), self.N(1e308)),
                                                      b.gt(b.mathop("abs", self.V("resultIm")), self.N(1e308))),
                                                b.or_(b.gt(b.mathop("abs", self.V("resultJ")), self.N(1e308)),
                                                      b.gt(b.mathop("abs", self.V("resultK")), self.N(1e308))))),
                                   self.err("Overflow")),
                         ]),
            ]),
        ]
        # main body
        body = [
            self.call(self.p["normalize"], [self.b.vblock(self.b.arg("source"))]),
            self.setv("src", self.V("normOut")),
            self.setv("err", self.N(0)),
            self.setv("errMsg", self.T("")),
            self.setv("result", self.N(0)),
            self.setv("prev", self.T("none")),
            self.setv("i", self.N(1)),
            self.setv("n", b.vblock(b.length(self.V("src")))),
            b.list_delete_all("out"), b.list_delete_all("outRe"), b.list_delete_all("outIm"),
            b.list_delete_all("outJ"), b.list_delete_all("outK"),
            b.list_delete_all("outKind"), b.list_delete_all("outArgc"),
            b.list_delete_all("ops"), b.list_delete_all("funcNames"),
            b.list_delete_all("parenCount"), b.list_delete_all("parenEmpty"),
            b.list_delete_all("parenFunc"), b.list_delete_all("vals"),
            b.list_delete_all("valsIm"), b.list_delete_all("valsJ"), b.list_delete_all("valsK"),
            b.list_delete_all("args"), b.list_delete_all("argsIm"),
            b.list_delete_all("argsJ"), b.list_delete_all("argsK"),
            b.ifelse(self.eqn("n", 0),
                     self.err("Empty"),
                     [
                         parse_loop,
                         b.if_(self.eqn("err", 0), [
                             b.repeat_until(b.or_(self.b.eq(self.b.vblock(b.list_len("parenCount")), self.N(0)),
                                                  self.eqn("err", 1)),
                                            [self.call(self.p["closeParen"], [])]),
                         ]),
                         b.if_(b.and_(self.eqn("err", 0), self.b.not_(self.is_value())),
                               self.err("Incomplete")),
                         b.if_(self.eqn("err", 0), [
                             b.repeat_until(b.or_(self.b.eq(self.b.vblock(b.list_len("ops")), self.N(0)),
                                                  self.eqn("err", 1)),
                                            [b.ifelse(b.or_(self.b.eq(self.last("ops"), self.T("(")),
                                                            self.b.eq(self.last("ops"), self.T("f"))),
                                                      self.err("Unmatched ("),
                                                      [self.call(self.p["popOp"], [])])]),
                         ]),
                         b.if_(self.eqn("err", 0), rpn_body),
                     ]),
        ]
        p.body(body)

    # --------------------------------------------------------------- preview
    def gen_preview(self):
        b = self.b
        p = self.p["preview"]
        body = [
            b.ifelse(self.eqv("expr", ""),
                     [self.setv("previewOk", self.N(0))],
                     [
                         self.call(self.p["evaluate"], [self.B("expr")]),
                         b.ifelse(self.eqn("err", 1),
                                  [self.setv("previewOk", self.N(0))],
                                  [self.setv("previewVal", self.V("result")),
                                   self.setv("previewIm", self.V("resultIm")),
                                   self.setv("previewJ", self.V("resultJ")),
                                   self.setv("previewK", self.V("resultK")),
                                   self.setv("previewOk", self.N(1))]),
                         self.setv("err", self.N(0)),
                         self.setv("errMsg", self.T("")),
                     ]),
        ]
        p.body(body)

    def gen_eval(self):
        b = self.b
        p = self.p["eval"]
        p.body([
            b.if_(self.eqv("expr", ""), [self.setv("expr", self.T("0"))]),
            self.call(self.p["evaluate"], [self.B("expr")]),
            b.ifelse(self.eqn("err", 1),
                     [self.setv("justEval", self.N(1))],
                     [self.setv("ans", self.V("result")),
                      self.setv("justEval", self.N(1))]),
            self.setv("previewOk", self.N(0)),
            self.call(self.p["drawDisplay"], []),
        ])

    def gen_insert_text(self):
        b = self.b
        p = self.p["insertText"]
        opch = b.gt(b.vblock(b.list_itemnum("opChars",
                                             b.vblock(b.letter(self.N(1), self.b.vblock(self.b.arg("t")))))),
                    self.N(0))
        body = [
            b.if_(self.eqn("justEval", 1), [
                b.ifelse(opch,
                         [
                             self.call(self.p["fmt"], [self.B("ans")]),
                             self.setv("expr", self.V("fmtOut")),
                         ],
                         [self.setv("expr", self.T(""))]),
                self.setv("justEval", self.N(0)),
            ]),
            b.if_(b.lt(b.vblock(b.length(self.V("expr"))), self.N(200)), [
                self.setv("expr", b.join(self.V("expr"), self.b.vblock(self.b.arg("t")))),
            ]),
            self.call(self.p["preview"], []),
            self.call(self.p["drawDisplay"], []),
        ]
        p.body(body)

    def gen_del_last(self):
        b = self.b
        p = self.p["delLast"]
        p.body([
            self.setv("justEval", self.N(0)),
            b.if_(b.gt(b.vblock(b.length(self.V("expr"))), self.N(0)), [
                self.call(self.p["dropLast"], [self.B("expr")]),
                self.setv("expr", self.V("dOut")),
            ]),
            self.call(self.p["preview"], []),
            self.call(self.p["drawDisplay"], []),
        ])

    def gen_clear_all(self):
        b = self.b
        p = self.p["clearAll"]
        p.body([
            self.setv("expr", self.T("")),
            self.setv("justEval", self.N(0)),
            self.setv("err", self.N(0)),
            self.setv("errMsg", self.T("")),
            self.setv("previewOk", self.N(0)),
            self.call(self.p["drawDisplay"], []),
        ])

    def gen_toggles(self):
        b = self.b
        p = self.p["toggleMode"]
        p.body([
            b.ifelse(self.eqv("angleMode", "DEG"),
                     [self.setv("angleMode", self.T("RAD"))],
                     [self.setv("angleMode", self.T("DEG"))]),
            self.call(self.p["drawDisplay"], []),
        ])
        p = self.p["toggleInv"]
        p.body([
            self.setv("inv", b.sub(self.N(1), self.V("inv"))),
            self.call(self.p["drawDisplay"], []),
        ])

    def gen_memory(self):
        b = self.b
        self.p["memClear"].body([
            self.setv("mem", self.N(0)),
            self.setv("memIm", self.N(0)),
            self.setv("memJ", self.N(0)),
            self.setv("memK", self.N(0)),
            self.call(self.p["drawDisplay"], []),
        ])
        self.p["memRecall"].body([
            self.call(self.p["fmtVal"], [self.B("mem"), self.B("memIm"),
                                         self.B("memJ"), self.B("memK")]),
            self.call(self.p["insertText"], [self.B("fmtValOut")]),
        ])
        p = self.p["memAdd"]
        p.body([
            b.ifelse(self.eqn("justEval", 1),
                     [self.setv("a", self.V("ans")),
                      self.setv("aIm", self.V("ansIm")),
                      self.setv("aJ", self.V("ansJ")),
                      self.setv("aK", self.V("ansK"))],
                     [b.ifelse(self.eqv("expr", ""),
                               [self.setv("a", self.N(0)),
                                self.setv("aIm", self.N(0)),
                                self.setv("aJ", self.N(0)),
                                self.setv("aK", self.N(0))],
                               [
                                   self.call(self.p["evaluate"], [self.B("expr")]),
                                   b.if_(self.eqn("err", 0), [
                                       self.setv("a", self.V("result")),
                                       self.setv("aIm", self.V("resultIm")),
                                       self.setv("aJ", self.V("resultJ")),
                                       self.setv("aK", self.V("resultK")),
                                   ]),
                               ])]),
            self.setv("err", self.N(0)),
            self.setv("errMsg", self.T("")),
            self.setv("mem", b.add(self.V("mem"),
                                   b.mul(self.b.vblock(self.b.arg("sign")), self.V("a")))),
            self.setv("memIm", b.add(self.V("memIm"),
                                     b.mul(self.b.vblock(self.b.arg("sign")), self.V("aIm")))),
            self.setv("memJ", b.add(self.V("memJ"),
                                    b.mul(self.b.vblock(self.b.arg("sign")), self.V("aJ")))),
            self.setv("memK", b.add(self.V("memK"),
                                    b.mul(self.b.vblock(self.b.arg("sign")), self.V("aK")))),
            self.call(self.p["drawDisplay"], []),
        ])

    def gen_switch(self):
        b = self.b
        self.p["graphFromExpr"].body([
            b.if_(b.or_(b.contains(self.V("expr"), self.T("x")),
                        b.contains(self.V("expr"), self.T("X"))),
                  [self.setv("graphExpr", self.V("expr"))]),
            self.call(self.p["switchToGraph"], []),
        ])
        self.p["typeAsk"].body([
            b.op("sensing_askandwait", {"QUESTION": self.T("Enter expression:")}),
            self.call(self.p["normalize"], [b.vblock(b.op("sensing_answer"))]),
            b.if_(b.not_(self.eqv("normOut", "")), [
                self.setv("expr", self.V("normOut")),
                self.setv("justEval", self.N(0)),
                self.call(self.p["preview"], []),
                self.call(self.p["drawDisplay"], []),
            ]),
        ])
        p = self.p["switchToGraph"]
        p.body([
            b.if_(b.not_(self.eqv("screen", "graph")), [
                self.setv("savedAngleMode", self.V("angleMode")),
            ]),
            self.setv("screen", self.T("graph")),
            self.setv("angleMode", self.T("RAD")),
            self.setv("traceOn", self.N(0)),
            self.setv("traceValid", self.N(0)),
            b.op("looks_switchbackdropto", {"BACKDROP": b.vmenu(b.menu_backdrop("graph"))}),
            self.setv("uiCostume", self.T("graphui")),
            b.op("looks_switchcostumeto", {"COSTUME": b.vblock(b.var("uiCostume"),
                                                               b.menu_costume("graphui"))}),
            b.op("motion_gotoxy", {"X": self.N(0), "Y": self.N(0)}),
            b.op("looks_setsizeto", {"SIZE": self.N(100)}),
            b.op("looks_show"),
            self.call(self.p["plotGraph"], []),
        ])
        p = self.p["switchToCalc"]
        p.body([
            self.setv("screen", self.T("calc")),
            self.setv("angleMode", self.V("savedAngleMode")),
            self.setv("traceOn", self.N(0)),
            self.setv("traceValid", self.N(0)),
            self.setv("err", self.N(0)),
            self.setv("errMsg", self.T("")),
            b.op("looks_switchbackdropto", {"BACKDROP": b.vmenu(b.menu_backdrop("calc"))}),
            self.setv("uiCostume", self.T("normal")),
            b.if_(self.eqn("inv", 1), [self.setv("uiCostume", self.T("second"))]),
            b.op("looks_switchcostumeto", {"COSTUME": b.vblock(b.var("uiCostume"),
                                                               b.menu_costume("normal"))}),
            b.op("motion_gotoxy", {"X": self.N(0), "Y": self.N(0)}),
            b.op("looks_setsizeto", {"SIZE": self.N(100)}),
            b.op("looks_show"),
            self.call(self.p["drawDisplay"], []),
        ])
        self.p["graphAsk"].body([
            b.op("sensing_askandwait", {"QUESTION": self.T("f(x) =")}),
            self.call(self.p["normalize"], [b.vblock(b.op("sensing_answer"))]),
            b.if_(b.not_(self.eqv("normOut", "")), [self.setv("graphExpr", self.V("normOut"))]),
            self.call(self.p["plotGraph"], []),
        ])

    # ------------------------------------------------------------ graph keys
    def gen_graph_keys(self):
        b = self.b
        p = self.p["handleGraphKey"]
        def idx():
            return b.vblock(b.arg("idx"))
        iff = self.iff
        body = [
            iff(b.eq(idx(), self.N(1)), [self.call(self.p["switchToCalc"], [])],
                [iff(b.eq(idx(), self.N(2)), [self.call(self.p["graphAsk"], [])],
                     [iff(b.eq(idx(), self.N(3)), [
                         self.setv("scale", b.div(self.V("scale"), self.N(1.25))),
                         b.if_(b.lt(self.V("scale"), self.N(5)), [self.setv("scale", self.N(5))]),
                         self.call(self.p["plotGraph"], []),
                     ], [iff(b.eq(idx(), self.N(4)), [
                         self.setv("scale", b.mul(self.V("scale"), self.N(1.25))),
                         b.if_(b.gt(self.V("scale"), self.N(80)), [self.setv("scale", self.N(80))]),
                         self.call(self.p["plotGraph"], []),
                     ], [iff(b.eq(idx(), self.N(5)), [
                         self.setv("scale", self.N(20)),
                         self.call(self.p["plotGraph"], []),
                     ], [iff(b.eq(idx(), self.N(6)), [
                         self.setv("traceOn", b.sub(self.N(1), self.V("traceOn"))),
                         b.if_(self.eqn("traceOn", 0), [self.setv("traceValid", self.N(0))]),
                     ], [])])])])])]),
        ]
        p.body(body)

    # -------------------------------------------------------------- plotting
    def gen_plot(self):
        b = self.b
        p = self.p["plotGraph"]
        def pen(color, size):
            return [
                b.op("pen_setPenColorToColor", {"COLOR": [1, [9, color]]}),
                b.op("pen_setPenSizeTo", {"SIZE": self.N(size)}),
            ]
        vgrid = [
            self.setv("pgs", self.V("scale")),
            self.setv("k", b.mathop("ceiling", b.div(self.N(-235), self.V("pgs")))),
            b.repeat(b.add(b.sub(b.mathop("floor", b.div(self.N(235), self.V("pgs"))), self.V("k")), self.N(1)), [
                self.setv("pgx", b.mul(self.V("k"), self.V("pgs"))),
                self.iff(self.eqn("k", 0), pen("#6f86ad", 2),
                         [self.iff(b.eq(b.mod(b.mathop("abs", self.V("k")), self.N(5)), self.N(0)),
                                   pen("#243553", 1), pen("#182338", 1))]),
                b.op("motion_gotoxy", {"X": self.V("pgx"), "Y": self.N(-135)}),
                b.op("pen_penDown"),
                b.op("motion_gotoxy", {"X": self.V("pgx"), "Y": self.N(150)}),
                b.op("pen_penUp"),
                self.changev("k", self.N(1)),
            ]),
        ]
        hgrid = [
            self.setv("k", b.mathop("ceiling", b.div(self.N(-135), self.V("pgs")))),
            b.repeat(b.add(b.sub(b.mathop("floor", b.div(self.N(150), self.V("pgs"))), self.V("k")), self.N(1)), [
                self.setv("pgy", b.mul(self.V("k"), self.V("pgs"))),
                self.iff(self.eqn("k", 0), pen("#6f86ad", 2),
                         [self.iff(b.eq(b.mod(b.mathop("abs", self.V("k")), self.N(5)), self.N(0)),
                                   pen("#243553", 1), pen("#182338", 1))]),
                b.op("motion_gotoxy", {"X": self.N(-235), "Y": self.V("pgy")}),
                b.op("pen_penDown"),
                b.op("motion_gotoxy", {"X": self.N(235), "Y": self.V("pgy")}),
                b.op("pen_penUp"),
                self.changev("k", self.N(1)),
            ]),
        ]
        curve = [
            *pen("#3fe0c0", 3),
            self.setv("px", self.N(-235)),
            self.setv("penWasDown", self.N(0)),
            b.repeat(self.N(236), [
                self.setv("xval", b.div(self.V("px"), self.V("pgs"))),
                self.call(self.p["evaluate"], [self.B("graphExpr")]),
                self.iff(self.eqn("err", 1),
                         [b.op("pen_penUp"), self.setv("penWasDown", self.N(0))],
                         [
                             self.setv("py", b.mul(self.V("result"), self.V("pgs"))),
                             self.iff(b.or_(b.or_(b.or_(b.lt(self.V("py"), self.N(-135)),
                                                       b.gt(self.V("py"), self.N(150))),
                                                 b.not_(self.eqn("resultIm", 0))),
                                           b.or_(b.not_(self.eqn("resultJ", 0)),
                                                 b.not_(self.eqn("resultK", 0)))),
                                      [b.op("pen_penUp"), self.setv("penWasDown", self.N(0))],
                                      [
                                          self.iff(self.eqn("penWasDown", 0), [
                                              b.op("motion_gotoxy", {"X": self.V("px"), "Y": self.V("py")}),
                                              b.op("pen_penDown"),
                                              self.setv("penWasDown", self.N(1)),
                                          ], []),
                                          b.op("motion_gotoxy", {"X": self.V("px"), "Y": self.V("py")}),
                                      ]),
                         ]),
                self.changev("px", self.N(2)),
            ]),
            b.op("pen_penUp"),
            self.setv("xval", self.N(0)),
            self.setv("err", self.N(0)),
            self.setv("errMsg", self.T("")),
        ]
        header = [
            self.call(self.p["fmt"], [self.B("scale")]),
            self.setv("exprText", b.join(self.T("f(x)="), self.V("graphExpr"))),
            self.setv("exprText", b.join(
                self.V("exprText"),
                b.join(self.T("   "), b.join(self.V("angleMode"),
                                             b.join(self.T("  ZOOM "), self.V("fmtOut")))))),
            self.setv("esize", self.N(80)),
            b.if_(b.gt(b.vblock(b.length(self.V("exprText"))), self.N(40)),
                  [self.setv("esize", self.N(65))]),
            self.call(self.p["drawLine"], [self.B("exprText"), self.N(162),
                                           self.V("esize"), self.N(0)]),
        ]
        body = [
            b.if_(self.eqv("graphExpr", ""), [self.setv("graphExpr", self.T("sin(x)"))]),
            b.op("pen_clear"),
            b.op("looks_hide"),
            *vgrid, *hgrid, *curve, *header,
            b.op("looks_switchcostumeto", {"COSTUME": b.vmenu(b.menu_costume("graphui"))}),
            b.op("motion_gotoxy", {"X": self.N(0), "Y": self.N(0)}),
            b.op("looks_setsizeto", {"SIZE": self.N(100)}),
            b.op("looks_show"),
        ]
        p.body(body)

    # -------------------------------------------------------------- drawLine
    def gen_draw_line(self):
        b = self.b
        p = self.p["drawLine"]
        adv = b.div(b.mul(self.N(9.6), self.V("dlSize")), self.N(100))
        body = [
            self.setv("dlText", b.vblock(b.arg("text"))),
            self.setv("dlY", b.vblock(b.arg("yy"))),
            self.setv("dlSize", b.vblock(b.arg("sz"))),
            self.setv("dlAlign", b.vblock(b.arg("al"))),
            self.setv("dlLen", b.vblock(b.length(self.V("dlText")))),
            b.if_(b.gt(self.V("dlLen"), self.N(0)), [
                self.setv("dlAdv", adv),
                self.setv("dlMax", b.mathop("floor", b.div(self.N(452), self.V("dlAdv")))),
                self.iff(b.gt(self.V("dlLen"), self.V("dlMax")),
                         [
                             self.setv("dlStart", b.add(b.sub(self.V("dlLen"), self.V("dlMax")), self.N(1))),
                             self.setv("dlLen", self.V("dlMax")),
                         ],
                         [self.setv("dlStart", self.N(1))]),
                self.iff(self.eqn("dlAlign", 0),
                         [self.setv("dlX", self.N(-224))],
                         [self.setv("dlX", b.sub(self.N(224), b.mul(self.V("dlLen"), self.V("dlAdv"))))]),
                self.setv("dlI", self.V("dlStart")),
                b.repeat(self.V("dlLen"), [
                    self.setv("dlCh", b.vblock(b.letter(self.V("dlI"), self.V("dlText")))),
                    b.if_(self.eqv("dlCh", "*"), [self.setv("dlCh", self.T("×"))]),
                    b.if_(self.eqv("dlCh", "/"), [self.setv("dlCh", self.T("÷"))]),
                    b.if_(self.eqv("dlCh", "-"), [self.setv("dlCh", self.T("−"))]),
                    b.if_(b.not_(self.eqv("dlCh", " ")), [
                        self.setv("dlIdx", b.vblock(b.list_itemnum("glyphChars", self.V("dlCh")))),
                        b.if_(self.eqn("dlIdx", 0), [self.setv("dlIdx", self.N(31))]),
                        b.op("looks_switchcostumeto", {
                            "COSTUME": b.vblock(b.list_item("glyphNames", self.V("dlIdx")),
                                                b.menu_costume("g65"))
                        }),
                        b.op("motion_gotoxy", {
                            "X": b.add(self.V("dlX"), b.div(b.mul(self.N(6), self.V("dlSize")), self.N(100))),
                            "Y": self.V("dlY"),
                        }),
                        b.op("looks_setsizeto", {"SIZE": self.V("dlSize")}),
                        b.op("pen_stamp"),
                    ]),
                    self.changev("dlX", self.V("dlAdv")),
                    self.changev("dlI", self.N(1)),
                ]),
            ]),
        ]
        p.body(body)

    # ----------------------------------------------------------- drawDisplay
    def gen_draw_display(self):
        b = self.b
        p = self.p["drawDisplay"]
        body = [
            self.setv("statusText", self.V("angleMode")),
            b.if_(self.eqn("inv", 1), [self.setv("statusText", b.join(self.V("statusText"), self.T("  2ND")))]),
            b.if_(b.or_(b.or_(b.not_(self.eqn("mem", 0)), b.not_(self.eqn("memIm", 0))),
                            b.or_(b.not_(self.eqn("memJ", 0)), b.not_(self.eqn("memK", 0)))),
                  [self.setv("statusText", b.join(self.V("statusText"), self.T("  M")))]),
            self.setv("exprText", self.V("expr")),
            b.if_(self.eqv("exprText", ""), [self.setv("exprText", self.T("0"))]),
            b.ifelse(self.eqn("err", 1),
                     [self.setv("resultText", b.join(self.T("Error: "), self.V("errMsg")))],
                     [b.ifelse(self.eqn("justEval", 1),
                               [self.call(self.p["fmtVal"], [self.B("result"), self.B("resultIm"),
                                                        self.B("resultJ"), self.B("resultK")]),
                                self.setv("resultText", self.V("fmtValOut"))],
                               [b.ifelse(self.eqn("previewOk", 1),
                                         [self.call(self.p["fmtVal"], [self.B("previewVal"), self.B("previewIm"),
                                                                  self.B("previewJ"), self.B("previewK")]),
                                          self.setv("resultText", self.V("fmtValOut"))],
                                         [self.setv("resultText", self.T(""))])])]),
            # choose sizes by length so quaternion results fit
            self.setv("esize", self.N(110)),
            b.if_(b.gt(b.vblock(b.length(self.V("exprText"))), self.N(38)), [self.setv("esize", self.N(90))]),
            b.if_(b.gt(b.vblock(b.length(self.V("exprText"))), self.N(48)), [self.setv("esize", self.N(75))]),
            self.setv("rsize", self.N(110)),
            b.if_(b.gt(b.vblock(b.length(self.V("resultText"))), self.N(24)), [self.setv("rsize", self.N(90))]),
            b.if_(b.gt(b.vblock(b.length(self.V("resultText"))), self.N(34)), [self.setv("rsize", self.N(75))]),
            b.if_(b.gt(b.vblock(b.length(self.V("resultText"))), self.N(46)), [self.setv("rsize", self.N(62))]),
            b.op("pen_clear"),
            b.op("looks_hide"),
            self.call(self.p["drawLine"], [self.B("statusText"), self.N(143), self.N(60), self.N(0)]),
            self.call(self.p["drawLine"], [self.B("exprText"), self.N(127), self.V("esize"), self.N(0)]),
            self.call(self.p["drawLine"], [self.B("resultText"), self.N(104), self.V("rsize"), self.N(1)]),
            self.setv("uiCostume", self.T("normal")),
            b.if_(self.eqn("inv", 1), [self.setv("uiCostume", self.T("second"))]),
            b.op("looks_switchcostumeto", {"COSTUME": b.vblock(b.var("uiCostume"),
                                                               b.menu_costume("normal"))}),
            b.op("motion_gotoxy", {"X": self.N(0), "Y": self.N(0)}),
            b.op("looks_setsizeto", {"SIZE": self.N(100)}),
            b.op("looks_show"),
        ]
        p.body(body)

    # ---------------------------------------------------------- key dispatch
    def gen_handle_key(self):
        b = self.b
        proc = self.p["handleKeyIndex"]
        p = self.p
        def idx():
            return b.vblock(b.arg("idx"))
        if "kind" not in self.m.vars:
            self.m.var("kind", "")
        if "ins" not in self.m.vars:
            self.m.var("ins", "")
        body = [
            b.ifelse(self.eqn("inv", 1),
                     [
                         self.setv("kind", b.vblock(b.list_item("keyKinds2", idx()))),
                         self.setv("ins", b.vblock(b.list_item("keyInserts2", idx()))),
                     ],
                     [
                         self.setv("kind", b.vblock(b.list_item("keyKinds", idx()))),
                         self.setv("ins", b.vblock(b.list_item("keyInserts", idx()))),
                     ]),
        ]
        cases = [
            ("ins", [self.call(p["insertText"], [self.B("ins")])]),
            ("del", [self.call(p["delLast"], [])]),
            ("ac", [self.call(p["clearAll"], [])]),
            ("eval", [self.call(p["eval"], [])]),
            ("mode", [self.call(p["toggleMode"], [])]),
            ("inv", [self.call(p["toggleInv"], [])]),
            ("mc", [self.call(p["memClear"], [])]),
            ("mr", [self.call(p["memRecall"], [])]),
            ("m+", [self.call(p["memAdd"], [self.N(1)])]),
            ("m-", [self.call(p["memAdd"], [self.N(-1)])]),
            ("ans", [
                self.call(p["fmtVal"], [self.B("ans"), self.B("ansIm"),
                                        self.B("ansJ"), self.B("ansK")]),
                self.call(p["insertText"], [self.B("fmtValOut")]),
            ]),
            ("graph", [self.call(p["graphFromExpr"], [])]),
            ("type", [self.call(p["typeAsk"], [])]),
        ]
        for kind, call in cases:
            body.append(b.if_(self.eqv("kind", kind), call))
        proc.body(body)
