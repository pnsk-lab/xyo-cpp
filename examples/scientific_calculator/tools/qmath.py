"""Complex / quaternion procedures for the Scratch evaluator."""
import math

PI = math.pi


class QMathMixin:
    # ------------------------------------------------------------- helpers
    def _qdef(self):
        b = self.b
        p = self.p
        for name, args in (
            ("qMul", []), ("qInv", []), ("qNorm", []), ("qConj", []),
            ("qAbs", []), ("qArg", []), ("qLog", []), ("qExp", []),
            ("qSqrt", []), ("qSin", []), ("qCos", []), ("qTan", []),
            ("qSinh", []), ("qCosh", []), ("qTanh", []), ("qDiv", []),
            ("qPowInt", []), ("qPow", []), ("qAsin", []), ("qAcos", []),
            ("qAtan", []), ("qAsinh", []), ("qAcosh", []), ("qAtanh", []),
            ("qFunc", ["fn", "ac"]), ("applyOpQ", ["op"]),
        ):
            p[name] = b.defproc(name, args, True)

    def _qr(self, re, im, j, k):
        return [self.setv("r", re), self.setv("rIm", im),
                self.setv("rJ", j), self.setv("rK", k)]

    def _qset_a(self, re, im, j, k):
        return [self.setv("a", re), self.setv("aIm", im),
                self.setv("aJ", j), self.setv("aK", k)]

    def _qset_b(self, re, im, j, k):
        return [self.setv("b", re), self.setv("bIm", im),
                self.setv("bJ", j), self.setv("bK", k)]

    def _qsave_t1(self):
        return [self.setv("t1Re", self.V("r")), self.setv("t1Im", self.V("rIm")),
                self.setv("t1J", self.V("rJ")), self.setv("t1K", self.V("rK"))]

    def _qload_a_t1(self):
        return self._qset_a(self.V("t1Re"), self.V("t1Im"), self.V("t1J"), self.V("t1K"))

    def _qload_b_t1(self):
        return self._qset_b(self.V("t1Re"), self.V("t1Im"), self.V("t1J"), self.V("t1K"))

    # --------------------------------------------------------------- bodies
    def _qgen(self):
        self._qgen_mul()
        self._qgen_inv()
        self._qgen_norm()
        self._qgen_conj()
        self._qgen_abs()
        self._qgen_arg()
        self._qgen_log()
        self._qgen_exp()
        self._qgen_sqrt()
        self._qgen_trig()
        self._qgen_hyp()
        self._qgen_div()
        self._qgen_pow()
        self._qgen_inverse()
        self._qgen_func()
        self._qgen_opq()

    def _qm(self, a, b, op):
        return getattr(self.b, op)(a, b)

    def _qgen_mul(self):
        b = self.b
        A, AI, AJ, AK = self.V("a"), self.V("aIm"), self.V("aJ"), self.V("aK")
        B, BI, BJ, BK = self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")
        rr = b.sub(b.sub(b.sub(b.mul(A, B), b.mul(AI, BI)), b.mul(AJ, BJ)), b.mul(AK, BK))
        ri = b.add(b.add(b.mul(A, BI), b.mul(AI, B)), b.sub(b.mul(AJ, BK), b.mul(AK, BJ)))
        rj = b.add(b.add(b.mul(A, BJ), b.mul(AJ, B)), b.sub(b.mul(AK, BI), b.mul(AI, BK)))
        rk = b.add(b.add(b.mul(A, BK), b.mul(AK, B)), b.sub(b.mul(AI, BJ), b.mul(AJ, BI)))
        self.p["qMul"].body(self._qr(rr, ri, rj, rk))

    def _qgen_inv(self):
        b = self.b
        qn2 = b.add(b.add(b.mul(self.V("a"), self.V("a")), b.mul(self.V("aIm"), self.V("aIm"))),
                    b.add(b.mul(self.V("aJ"), self.V("aJ")), b.mul(self.V("aK"), self.V("aK"))))
        body = [
            self.setv("qn2", qn2),
            b.ifelse(self.eqn("qn2", 0), self.err("Division by zero"), [
                *self._qr(b.div(self.V("a"), self.V("qn2")),
                          b.div(b.sub(self.N(0), self.V("aIm")), self.V("qn2")),
                          b.div(b.sub(self.N(0), self.V("aJ")), self.V("qn2")),
                          b.div(b.sub(self.N(0), self.V("aK")), self.V("qn2"))),
            ]),
        ]
        self.p["qInv"].body(body)

    def _qgen_norm(self):
        b = self.b
        q = b.add(b.add(b.mul(self.V("a"), self.V("a")), b.mul(self.V("aIm"), self.V("aIm"))),
                  b.add(b.mul(self.V("aJ"), self.V("aJ")), b.mul(self.V("aK"), self.V("aK"))))
        self.p["qNorm"].body([self.setv("qn", b.mathop("sqrt", q))])

    def _qgen_conj(self):
        b = self.b
        self.p["qConj"].body(self._qr(self.V("a"), b.sub(self.N(0), self.V("aIm")),
                                      b.sub(self.N(0), self.V("aJ")),
                                      b.sub(self.N(0), self.V("aK"))))

    def _qgen_abs(self):
        b = self.b
        self.p["qAbs"].body([
            self.call(self.p["qNorm"], []),
            *self._qr(self.V("qn"), self.N(0), self.N(0), self.N(0)),
        ])

    def _qgen_arg(self):
        b = self.b
        # complex argument in radians; quaternion angle acos(a/|q|)
        complex_arg = [
            self.setv("qtheta", self.N(0)),
            b.ifelse(b.gt(self.V("a"), self.N(0)),
                     [self.setv("qtheta", b.mul(b.mathop("atan", b.div(self.V("aIm"), self.V("a"))),
                                                self.N(PI / 180)))],
                     [b.ifelse(b.lt(self.V("a"), self.N(0)),
                               [self.setv("qtheta", b.mul(b.mathop("atan", b.div(self.V("aIm"), self.V("a"))),
                                                          self.N(PI / 180))),
                                b.ifelse(b.not_(b.lt(self.V("aIm"), self.N(0))),
                                         [self.changev("qtheta", self.N(PI))],
                                         [self.changev("qtheta", self.N(-PI))])],
                               [b.ifelse(b.gt(self.V("aIm"), self.N(0)),
                                         [self.setv("qtheta", self.N(PI / 2))],
                                         [b.ifelse(b.lt(self.V("aIm"), self.N(0)),
                                                   [self.setv("qtheta", self.N(-PI / 2))],
                                                   [self.setv("qtheta", self.N(0))])])])]),
            *self._qr(self.V("qtheta"), self.N(0), self.N(0), self.N(0)),
        ]
        quat_arg = [
            self.call(self.p["qNorm"], []),
            b.ifelse(self.eqn("qn", 0), self.err("Domain"), [
                self.setv("qtheta", b.mul(b.mathop("acos", b.div(self.V("a"), self.V("qn"))),
                                          self.N(PI / 180))),
                *self._qr(self.V("qtheta"), self.N(0), self.N(0), self.N(0)),
            ]),
        ]
        self.p["qArg"].body([
            b.ifelse(b.and_(self.eqn("aJ", 0), self.eqn("aK", 0)), complex_arg, quat_arg),
        ])

    def _qgen_log(self):
        b = self.b
        qv = b.mathop("sqrt", b.add(b.add(b.mul(self.V("aIm"), self.V("aIm")),
                                            b.mul(self.V("aJ"), self.V("aJ"))),
                                    b.mul(self.V("aK"), self.V("aK"))))
        pure = [
            b.ifelse(b.gt(self.V("a"), self.N(0)),
                     [*self._qr(b.mathop("ln", self.V("a")), self.N(0), self.N(0), self.N(0))],
                     [b.ifelse(b.lt(self.V("a"), self.N(0)),
                               [*self._qr(b.mathop("ln", b.sub(self.N(0), self.V("a"))),
                                          self.N(PI), self.N(0), self.N(0))],
                               self.err("Domain"))]),
        ]
        general = [
            self.setv("qtheta", b.mul(b.mathop("acos", b.div(self.V("a"), self.V("qn"))),
                                      self.N(PI / 180))),
            b.ifelse(self.eqn("qv", 0), self.err("Domain"), [
                self.setv("qmag", b.div(self.V("qtheta"), self.V("qv"))),
                *self._qr(b.mathop("ln", self.V("qn")),
                          b.mul(self.V("qmag"), self.V("aIm")),
                          b.mul(self.V("qmag"), self.V("aJ")),
                          b.mul(self.V("qmag"), self.V("aK"))),
            ]),
        ]
        self.p["qLog"].body([
            self.call(self.p["qNorm"], []),
            b.ifelse(self.eqn("qn", 0), self.err("Domain"), [
                self.setv("qv", qv),
                b.ifelse(self.eqn("qv", 0), pure, general),
            ]),
        ])

    def _qgen_exp(self):
        b = self.b
        qv = b.mathop("sqrt", b.add(b.add(b.mul(self.V("aIm"), self.V("aIm")),
                                            b.mul(self.V("aJ"), self.V("aJ"))),
                                    b.mul(self.V("aK"), self.V("aK"))))
        body = [
            self.setv("qv", qv),
            self.setv("qea", b.mathop("e ^", self.V("a"))),
            b.ifelse(self.eqn("qv", 0),
                     self._qr(self.V("qea"), self.N(0), self.N(0), self.N(0)),
                     [
                         self.setv("qcr", b.mathop("cos", b.mul(self.V("qv"), self.N(180 / PI)))),
                         self.setv("qsr", b.mathop("sin", b.mul(self.V("qv"), self.N(180 / PI)))),
                         self.setv("qmag", b.div(b.mul(self.V("qea"), self.V("qsr")), self.V("qv"))),
                         *self._qr(b.mul(self.V("qea"), self.V("qcr")),
                                   b.mul(self.V("qmag"), self.V("aIm")),
                                   b.mul(self.V("qmag"), self.V("aJ")),
                                   b.mul(self.V("qmag"), self.V("aK"))),
                     ]),
        ]
        self.p["qExp"].body(body)

    def _qgen_sqrt(self):
        b = self.b
        real = [
            b.ifelse(b.not_(b.lt(self.V("a"), self.N(0))),
                     self._qr(b.mathop("sqrt", self.V("a")), self.N(0), self.N(0), self.N(0)),
                     self._qr(self.N(0), b.mathop("sqrt", b.sub(self.N(0), self.V("a"))),
                              self.N(0), self.N(0))),
        ]
        body = [
            b.ifelse(b.and_(b.and_(self.eqn("aIm", 0), self.eqn("aJ", 0)), self.eqn("aK", 0)),
                     real,
                     [
                         self.call(self.p["qLog"], []),
                         *self._qsave_t1(),
                         *self._qset_a(b.mul(self.V("t1Re"), self.N(0.5)),
                                       b.mul(self.V("t1Im"), self.N(0.5)),
                                       b.mul(self.V("t1J"), self.N(0.5)),
                                       b.mul(self.V("t1K"), self.N(0.5))),
                         self.call(self.p["qExp"], []),
                     ]),
        ]
        self.p["qSqrt"].body(body)

    def _qgen_trig(self):
        b = self.b
        qv = lambda: b.mathop("sqrt", b.add(b.add(b.mul(self.V("aIm"), self.V("aIm")),
                                                   b.mul(self.V("aJ"), self.V("aJ"))),
                                           b.mul(self.V("aK"), self.V("aK"))))
        # sin
        body = [
            self.setv("qv", qv()),
            self.setv("qsr", b.mathop("sin", b.mul(self.V("a"), self.N(180 / PI)))),
            self.setv("qcr", b.mathop("cos", b.mul(self.V("a"), self.N(180 / PI)))),
            b.ifelse(self.eqn("qv", 0),
                     self._qr(self.V("qsr"), self.N(0), self.N(0), self.N(0)),
                     [
                         self.setv("qsh", b.div(b.sub(b.mathop("e ^", self.V("qv")),
                                                      b.mathop("e ^", b.sub(self.N(0), self.V("qv")))),
                                                self.N(2))),
                         self.setv("qch", b.div(b.add(b.mathop("e ^", self.V("qv")),
                                                      b.mathop("e ^", b.sub(self.N(0), self.V("qv")))),
                                                self.N(2))),
                         self.setv("qmag", b.div(self.V("qch"), self.V("qv"))),
                         self.setv("qval", b.div(b.mul(self.V("qcr"), self.V("qsh")), self.V("qv"))),
                         *self._qr(b.mul(self.V("qsr"), self.V("qch")),
                                   b.mul(self.V("qval"), self.V("aIm")),
                                   b.mul(self.V("qval"), self.V("aJ")),
                                   b.mul(self.V("qval"), self.V("aK"))),
                     ]),
        ]
        self.p["qSin"].body(body)
        # cos
        body = [
            self.setv("qv", qv()),
            self.setv("qsr", b.mathop("sin", b.mul(self.V("a"), self.N(180 / PI)))),
            self.setv("qcr", b.mathop("cos", b.mul(self.V("a"), self.N(180 / PI)))),
            b.ifelse(self.eqn("qv", 0),
                     self._qr(self.V("qcr"), self.N(0), self.N(0), self.N(0)),
                     [
                         self.setv("qsh", b.div(b.sub(b.mathop("e ^", self.V("qv")),
                                                      b.mathop("e ^", b.sub(self.N(0), self.V("qv")))),
                                                self.N(2))),
                         self.setv("qch", b.div(b.add(b.mathop("e ^", self.V("qv")),
                                                      b.mathop("e ^", b.sub(self.N(0), self.V("qv")))),
                                                self.N(2))),
                         self.setv("qmag", b.div(self.V("qch"), self.V("qv"))),
                         self.setv("qval", b.div(b.mul(b.sub(self.N(0), self.V("qsr")), self.V("qsh")),
                                                 self.V("qv"))),
                         *self._qr(b.mul(self.V("qcr"), self.V("qch")),
                                   b.mul(self.V("qval"), self.V("aIm")),
                                   b.mul(self.V("qval"), self.V("aJ")),
                                   b.mul(self.V("qval"), self.V("aK"))),
                     ]),
        ]
        self.p["qCos"].body(body)
        # tan
        self.p["qTan"].body([
            self.call(self.p["qSin"], []),
            *self._qsave_t1(),
            self.call(self.p["qCos"], []),
            *self._qset_b(self.V("r"), self.V("rIm"), self.V("rJ"), self.V("rK")),
            *self._qload_a_t1(),
            self.call(self.p["qDiv"], []),
        ])

    def _qgen_hyp(self):
        b = self.b
        qv = lambda: b.mathop("sqrt", b.add(b.add(b.mul(self.V("aIm"), self.V("aIm")),
                                                   b.mul(self.V("aJ"), self.V("aJ"))),
                                           b.mul(self.V("aK"), self.V("aK"))))
        for name, sinh in (("qSinh", True), ("qCosh", False)):
            body = [
                self.setv("qv", qv()),
                self.setv("qshR", b.div(b.sub(b.mathop("e ^", self.V("a")),
                                               b.mathop("e ^", b.sub(self.N(0), self.V("a")))),
                                         self.N(2))),
                self.setv("qchR", b.div(b.add(b.mathop("e ^", self.V("a")),
                                               b.mathop("e ^", b.sub(self.N(0), self.V("a")))),
                                         self.N(2))),
                b.ifelse(self.eqn("qv", 0),
                         self._qr(self.V("qshR") if sinh else self.V("qchR"),
                                  self.N(0), self.N(0), self.N(0)),
                         [
                             self.setv("qsv", b.mathop("sin", b.mul(self.V("qv"), self.N(180 / PI)))),
                             self.setv("qcv", b.mathop("cos", b.mul(self.V("qv"), self.N(180 / PI)))),
                             self.setv("qmag", b.div(self.V("qchR") if sinh else self.V("qshR"),
                                                      self.V("qv"))),
                             *self._qr(b.mul(self.V("qshR") if sinh else self.V("qchR"), self.V("qcv")),
                                       b.mul(self.V("qmag"), b.mul(self.V("qsv"), self.V("aIm"))),
                                       b.mul(self.V("qmag"), b.mul(self.V("qsv"), self.V("aJ"))),
                                       b.mul(self.V("qmag"), b.mul(self.V("qsv"), self.V("aK")))),
                         ]),
            ]
            self.p[name].body(body)
        self.p["qTanh"].body([
            self.call(self.p["qSinh"], []),
            *self._qsave_t1(),
            self.call(self.p["qCosh"], []),
            *self._qset_b(self.V("r"), self.V("rIm"), self.V("rJ"), self.V("rK")),
            *self._qload_a_t1(),
            self.call(self.p["qDiv"], []),
        ])

    def _qgen_div(self):
        b = self.b
        body = [
            self.setv("qn2", b.add(b.add(b.mul(self.V("b"), self.V("b")),
                                         b.mul(self.V("bIm"), self.V("bIm"))),
                                   b.add(b.mul(self.V("bJ"), self.V("bJ")),
                                         b.mul(self.V("bK"), self.V("bK"))))),
            b.ifelse(self.eqn("qn2", 0), self.err("Division by zero"), [
                self.setv("t1Re", self.V("a")), self.setv("t1Im", self.V("aIm")),
                self.setv("t1J", self.V("aJ")), self.setv("t1K", self.V("aK")),
                *self._qset_b(self.V("b"), b.sub(self.N(0), self.V("bIm")),
                              b.sub(self.N(0), self.V("bJ")),
                              b.sub(self.N(0), self.V("bK"))),
                *self._qload_a_t1(),
                self.call(self.p["qMul"], []),
                *self._qr(b.div(self.V("r"), self.V("qn2")),
                          b.div(self.V("rIm"), self.V("qn2")),
                          b.div(self.V("rJ"), self.V("qn2")),
                          b.div(self.V("rK"), self.V("qn2"))),
            ]),
        ]
        self.p["qDiv"].body(body)

    def _qgen_pow(self):
        b = self.b
        # exponentiation by squaring (integer real exponent)
        odd = b.eq(b.sub(self.V("pn"), b.mul(self.N(2), b.mathop("floor", b.div(self.V("pn"), self.N(2))))),
                   self.N(1))
        body_int = [
            self.setv("pn", b.mathop("abs", b.vblock(b.round_(self.V("b"))))),
            *self._qset_a(self.N(1), self.N(0), self.N(0), self.N(0)),
            *self._qsave_t1(),
            *self._qset_a(self.V("a") if False else self.V("t1Re"), self.N(0), self.N(0), self.N(0)),
        ]
        # build carefully: t1 = 1, t2 = base
        body_int = [
            self.setv("pneg", b.vblock(b.lt(self.V("b"), self.N(0)))),
            self.setv("pn", b.mathop("abs", b.vblock(b.round_(self.V("b"))))),
            self.setv("t1Re", self.N(1)), self.setv("t1Im", self.N(0)),
            self.setv("t1J", self.N(0)), self.setv("t1K", self.N(0)),
            self.setv("t2Re", self.V("a")), self.setv("t2Im", self.V("aIm")),
            self.setv("t2J", self.V("aJ")), self.setv("t2K", self.V("aK")),
            b.repeat_until(self.eqn("pn", 0), [
                b.if_(odd, [
                    *self._qset_a(self.V("t1Re"), self.V("t1Im"), self.V("t1J"), self.V("t1K")),
                    *self._qset_b(self.V("t2Re"), self.V("t2Im"), self.V("t2J"), self.V("t2K")),
                    self.call(self.p["qMul"], []),
                    *self._qsave_t1(),
                ]),
                *self._qset_a(self.V("t2Re"), self.V("t2Im"), self.V("t2J"), self.V("t2K")),
                *self._qset_b(self.V("t2Re"), self.V("t2Im"), self.V("t2J"), self.V("t2K")),
                self.call(self.p["qMul"], []),
                self.setv("t2Re", self.V("r")), self.setv("t2Im", self.V("rIm")),
                self.setv("t2J", self.V("rJ")), self.setv("t2K", self.V("rK")),
                self.setv("pn", b.mathop("floor", b.div(self.V("pn"), self.N(2)))),
            ]),
            b.if_(self.eqn("pneg", 1), [
                *self._qset_a(self.V("t1Re"), self.V("t1Im"), self.V("t1J"), self.V("t1K")),
                self.call(self.p["qInv"], []),
                *self._qsave_t1(),
            ]),
            self.setv("pOut2", self.V("t1Re")), self.setv("pOut2Im", self.V("t1Im")),
            self.setv("pOut2J", self.V("t1J")), self.setv("pOut2K", self.V("t1K")),
        ]
        self.p["qPowInt"].body(body_int)

        def real_positive():
            return [
                self.setv("qval", b.mathop("e ^", b.mul(self.V("b"), b.mathop("ln", self.V("a"))))),
                self.setv("pOut2", self.V("qval")), self.setv("pOut2Im", self.N(0)),
                self.setv("pOut2J", self.N(0)), self.setv("pOut2K", self.N(0)),
            ]

        def real_negative():
            return [
                self.setv("qmag", b.mathop("e ^", b.mul(self.V("b"),
                                                        b.mathop("ln", b.sub(self.N(0), self.V("a")))))),
                self.setv("qtheta", b.mul(self.N(PI), self.V("b"))),
                self.setv("pOut2", b.mul(self.V("qmag"),
                                         b.mathop("cos", b.mul(self.V("qtheta"), self.N(180 / PI))))),
                self.setv("pOut2Im", b.mul(self.V("qmag"),
                                           b.mathop("sin", b.mul(self.V("qtheta"), self.N(180 / PI))))),
                self.setv("pOut2J", self.N(0)), self.setv("pOut2K", self.N(0)),
            ]

        def general():
            return [
            self.call(self.p["qLog"], []),
            *self._qsave_t1(),
            *self._qload_a_t1(),
            *self._qset_b(self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")),
            self.call(self.p["qMul"], []),
            *self._qsave_t1(),
            *self._qload_a_t1(),
            self.call(self.p["qExp"], []),
            self.setv("pOut2", self.V("r")), self.setv("pOut2Im", self.V("rIm")),
            self.setv("pOut2J", self.V("rJ")), self.setv("pOut2K", self.V("rK")),
            ]
        body = [
            b.ifelse(b.and_(b.and_(self.eqn("bIm", 0), self.eqn("bJ", 0)), self.eqn("bK", 0)), [
                b.ifelse(self.eqn("b", 0),
                         [self.setv("pOut2", self.N(1)), self.setv("pOut2Im", self.N(0)),
                          self.setv("pOut2J", self.N(0)), self.setv("pOut2K", self.N(0))],
                         [b.ifelse(b.and_(b.eq(self.V("b"), b.vblock(b.round_(self.V("b")))),
                                          b.not_(b.gt(b.mathop("abs", self.V("b")), self.N(1e15)))),
                                   [self.call(self.p["qPowInt"], [])],
                                   [b.ifelse(b.and_(b.and_(self.eqn("aIm", 0), self.eqn("aJ", 0)),
                                                    self.eqn("aK", 0)), [
                                       b.ifelse(b.gt(self.V("a"), self.N(0)), real_positive(),
                                                [b.ifelse(self.eqn("a", 0),
                                                          [b.ifelse(b.gt(self.V("b"), self.N(0)),
                                                                    [self.setv("pOut2", self.N(0)),
                                                                     self.setv("pOut2Im", self.N(0)),
                                                                     self.setv("pOut2J", self.N(0)),
                                                                     self.setv("pOut2K", self.N(0))],
                                                                    self.err("Domain"))],
                                                          real_negative())])],
                                       general())])]),
            ], general()),
        ]
        self.p["qPow"].body(body)

    def _qgen_inverse(self):
        b = self.b
        # save original z in t2 for formulas that overwrite a
        def save_z():
            return [self.setv("t2Re", self.V("a")), self.setv("t2Im", self.V("aIm")),
                    self.setv("t2J", self.V("aJ")), self.setv("t2K", self.V("aK"))]
        # asin(z) = -i log(iz + sqrt(1-z^2))
        z2r = b.sub(b.mul(self.V("a"), self.V("a")), b.mul(self.V("aIm"), self.V("aIm")))
        z2i = b.mul(self.N(2), b.mul(self.V("a"), self.V("aIm")))
        asin_body = [
            *save_z(),
            *self._qset_a(b.sub(self.N(1), z2r), b.sub(self.N(0), z2i), self.N(0), self.N(0)),
            self.call(self.p["qSqrt"], []),
            *self._qsave_t1(),
            *self._qset_a(b.sub(self.V("t1Re"), self.V("t2Im")),
                          b.add(self.V("t1Im"), self.V("t2Re")), self.N(0), self.N(0)),
            self.call(self.p["qLog"], []),
            *self._qsave_t1(),
            *self._qr(self.V("t1Im"), b.sub(self.N(0), self.V("t1Re")), self.N(0), self.N(0)),
        ]
        self.p["qAsin"].body(asin_body)
        self.p["qAcos"].body([
            self.call(self.p["qAsin"], []),
            *self._qsave_t1(),
            *self._qr(b.sub(self.N(PI / 2), self.V("t1Re")),
                      b.sub(self.N(0), self.V("t1Im")), self.N(0), self.N(0)),
        ])
        # atan(z) = -i/2 log((1+iz)/(1-iz)) ; save z in t2
        self.p["qAtan"].body([
            *save_z(),
            *self._qset_a(b.sub(self.N(1), self.V("t2Im")), self.V("t2Re"), self.N(0), self.N(0)),
            *self._qset_b(b.add(self.N(1), self.V("t2Im")),
                          b.sub(self.N(0), self.V("t2Re")), self.N(0), self.N(0)),
            self.call(self.p["qDiv"], []),
            *self._qsave_t1(),
            *self._qload_a_t1(),
            self.call(self.p["qLog"], []),
            *self._qsave_t1(),
            *self._qr(b.div(self.V("t1Im"), self.N(2)),
                      b.div(b.sub(self.N(0), self.V("t1Re")), self.N(2)), self.N(0), self.N(0)),
        ])
        # asinh = log(z + sqrt(z^2+1))
        self.p["qAsinh"].body([
            *save_z(),
            *self._qset_a(b.add(b.sub(b.mul(self.V("t2Re"), self.V("t2Re")),
                                      b.mul(self.V("t2Im"), self.V("t2Im"))), self.N(1)),
                          b.mul(self.N(2), b.mul(self.V("t2Re"), self.V("t2Im"))),
                          self.N(0), self.N(0)),
            self.call(self.p["qSqrt"], []),
            *self._qsave_t1(),
            *self._qset_a(b.add(self.V("t1Re"), self.V("t2Re")),
                          b.add(self.V("t1Im"), self.V("t2Im")), self.N(0), self.N(0)),
            self.call(self.p["qLog"], []),
        ])
        # acosh = log(z + sqrt(z^2-1))
        self.p["qAcosh"].body([
            *save_z(),
            *self._qset_a(b.sub(b.sub(b.mul(self.V("t2Re"), self.V("t2Re")),
                                      b.mul(self.V("t2Im"), self.V("t2Im"))), self.N(1)),
                          b.mul(self.N(2), b.mul(self.V("t2Re"), self.V("t2Im"))),
                          self.N(0), self.N(0)),
            self.call(self.p["qSqrt"], []),
            *self._qsave_t1(),
            *self._qset_a(b.add(self.V("t1Re"), self.V("t2Re")),
                          b.add(self.V("t1Im"), self.V("t2Im")), self.N(0), self.N(0)),
            self.call(self.p["qLog"], []),
        ])
        # atanh = 0.5 log((1+z)/(1-z))
        self.p["qAtanh"].body([
            *save_z(),
            *self._qset_a(b.add(self.N(1), self.V("t2Re")), self.V("t2Im"), self.N(0), self.N(0)),
            *self._qset_b(b.sub(self.N(1), self.V("t2Re")),
                          b.sub(self.N(0), self.V("t2Im")), self.N(0), self.N(0)),
            self.call(self.p["qDiv"], []),
            *self._qsave_t1(),
            *self._qload_a_t1(),
            self.call(self.p["qLog"], []),
            *self._qr(b.mul(self.V("r"), self.N(0.5)),
                      b.mul(self.V("rIm"), self.N(0.5)), self.N(0), self.N(0)),
        ])

    def _qgen_opq(self):
        b = self.b
        A, AI, AJ, AK = self.V("a"), self.V("aIm"), self.V("aJ"), self.V("aK")
        B, BI, BJ, BK = self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")
        op_is = lambda name: b.eq(b.vblock(b.arg("op")), self.T(name))
        body = [
            b.if_(self.eqn("err", 0), [
                b.if_(op_is("u-"), self._qr(b.sub(self.N(0), A), b.sub(self.N(0), AI),
                                            b.sub(self.N(0), AJ), b.sub(self.N(0), AK))),
                b.if_(op_is("!"), self.err("Real only")),
                b.if_(op_is("%"), self._qr(b.div(A, self.N(100)), b.div(AI, self.N(100)),
                                           b.div(AJ, self.N(100)), b.div(AK, self.N(100)))),
                b.if_(op_is("+"), self._qr(b.add(A, B), b.add(AI, BI),
                                           b.add(AJ, BJ), b.add(AK, BK))),
                b.if_(op_is("-"), self._qr(b.sub(A, B), b.sub(AI, BI),
                                           b.sub(AJ, BJ), b.sub(AK, BK))),
                b.if_(op_is("*"), [self.call(self.p["qMul"], [])]),
                b.if_(op_is("/"), [self.call(self.p["qDiv"], [])]),
                b.if_(op_is("^"), [
                    self.call(self.p["qPow"], []),
                    b.if_(self.eqn("err", 0), [
                        self.setv("r", self.V("pOut2")),
                        self.setv("rIm", self.V("pOut2Im")),
                        self.setv("rJ", self.V("pOut2J")),
                        self.setv("rK", self.V("pOut2K")),
                    ]),
                ]),
            ]),
        ]
        self.p["applyOpQ"].body(body)

    # -------------------------------------------------------------- dispatch
    def _qgen_func(self):
        b = self.b
        p = self.p
        A, AI, AJ, AK = self.V("a"), self.V("aIm"), self.V("aJ"), self.V("aK")
        nonreal = b.or_(b.or_(b.not_(self.eqn("aIm", 0)), b.not_(self.eqn("aJ", 0))),
                        b.not_(self.eqn("aK", 0)))
        def callq(name):
            return [self.call(p[name], [])]
        def result_from_r():
            return [
                self.setv("fr", self.V("r")), self.setv("frIm", self.V("rIm")),
                self.setv("frJ", self.V("rJ")), self.setv("frK", self.V("rK")),
                self.setv("haveResult", self.N(1)),
            ]
        def fn_is(name):
            return self.eqv("fname", name)
        def fixed(n):
            return b.eq(self.V("fargc"), self.N(n))
        def bad():
            return self.err("Bad args")

        branches = []
        # trig / hyperbolic / exp / log / sqrt
        for name in ("sin", "cos", "tan", "sinh", "cosh", "tanh", "exp", "sqrt", "asin", "acos", "atan", "asinh", "acosh", "atanh"):
            proc = {"sin": "qSin", "cos": "qCos", "tan": "qTan",
                    "sinh": "qSinh", "cosh": "qCosh", "tanh": "qTanh",
                    "exp": "qExp", "sqrt": "qSqrt",
                    "asin": "qAsin", "acos": "qAcos", "atan": "qAtan",
                    "asinh": "qAsinh", "acosh": "qAcosh", "atanh": "qAtanh"}[name]
            body = [b.ifelse(fixed(1), [*callq(proc), *result_from_r()], bad())]
            if name in ("asin", "acos", "atan", "asinh", "acosh", "atanh"):
                body = [b.ifelse(fixed(1), [b.ifelse(
                    b.and_(self.eqn("aJ", 0), self.eqn("aK", 0)),
                    [*callq(proc), *result_from_r()],
                    self.err("Complex only"))], bad())]
            branches.append((name, body))
        # ln / log / log2
        ln_body = [b.ifelse(fixed(1), [*callq("qLog"), *result_from_r()], bad())]
        branches.append(("ln", ln_body))
        log_body = [
            b.ifelse(b.or_(fixed(1), fixed(2)), [
                b.ifelse(fixed(1), [
                    *callq("qLog"),
                    *self._qr(b.div(self.V("r"), b.mathop("ln", self.N(10))),
                              b.div(self.V("rIm"), b.mathop("ln", self.N(10))),
                              b.div(self.V("rJ"), b.mathop("ln", self.N(10))),
                              b.div(self.V("rK"), b.mathop("ln", self.N(10)))),
                    *result_from_r(),
                ], [
                    # log(base, x) = log(x)/log(base)
                    *self._qset_a(self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")),
                    *callq("qLog"),
                    *self._qsave_t1(),
                    *self._qset_a(A, AI, AJ, AK),
                    *callq("qLog"),
                    *self._qset_b(self.V("t1Re"), self.V("t1Im"), self.V("t1J"), self.V("t1K")),
                    self.setv("t2Re", self.V("r")), self.setv("t2Im", self.V("rIm")),
                    self.setv("t2J", self.V("rJ")), self.setv("t2K", self.V("rK")),
                    *self._qset_a(self.V("t2Re"), self.V("t2Im"), self.V("t2J"), self.V("t2K")),
                    self.call(p["qDiv"], []),
                    *result_from_r(),
                ]),
            ], bad()),
        ]
        branches.append(("log", log_body))
        branches.append(("log2", [b.ifelse(fixed(1), [
            *callq("qLog"),
            *self._qr(b.div(self.V("r"), b.mathop("ln", self.N(2))),
                      b.div(self.V("rIm"), b.mathop("ln", self.N(2))),
                      b.div(self.V("rJ"), b.mathop("ln", self.N(2))),
                      b.div(self.V("rK"), b.mathop("ln", self.N(2)))),
            *result_from_r(),
        ], bad())]))
        # pow / root / cbrt
        branches.append(("pow", [b.ifelse(fixed(2), [
            *self._qset_a(A, AI, AJ, AK),
            *self._qset_b(self.V("b"), self.V("bIm"), self.V("bJ"), self.V("bK")),
            self.call(p["qPow"], []),
            *self._qr(self.V("pOut2"), self.V("pOut2Im"),
                      self.V("pOut2J"), self.V("pOut2K")),
            *result_from_r(),
        ], bad())]))
        branches.append(("root", [b.ifelse(fixed(2), [
            *self._qset_a(A, AI, AJ, AK),
            *self._qset_b(b.div(self.N(1), self.V("b")), self.N(0), self.N(0), self.N(0)),
            self.call(p["qPow"], []),
            *self._qr(self.V("pOut2"), self.V("pOut2Im"),
                      self.V("pOut2J"), self.V("pOut2K")),
            *result_from_r(),
        ], bad())]))
        branches.append(("cbrt", [b.ifelse(fixed(1), [
            *self._qset_a(A, AI, AJ, AK),
            *self._qset_b(self.N(1 / 3), self.N(0), self.N(0), self.N(0)),
            self.call(p["qPow"], []),
            *self._qr(self.V("pOut2"), self.V("pOut2Im"),
                      self.V("pOut2J"), self.V("pOut2K")),
            *result_from_r(),
        ], bad())]))
        # structural
        branches.append(("abs", [b.ifelse(fixed(1), [*callq("qAbs"), *result_from_r()], bad())]))
        branches.append(("norm", [b.ifelse(fixed(1), [*callq("qAbs"), *result_from_r()], bad())]))
        branches.append(("arg", [b.ifelse(fixed(1), [*callq("qArg"), *result_from_r()], bad())]))
        branches.append(("conj", [b.ifelse(fixed(1), [*callq("qConj"), *result_from_r()], bad())]))
        branches.append(("inv", [b.ifelse(fixed(1), [*callq("qInv"), *result_from_r()], bad())]))
        for name, comp in (("re", "a"), ("real", "a"), ("im", "aIm"),
                           ("imag", "aIm"), ("jpart", "aJ"), ("kpart", "aK")):
            branches.append((name, [b.ifelse(fixed(1), [
                *self._qr(self.V(comp), self.N(0), self.N(0), self.N(0)),
                *result_from_r(),
            ], bad())]))
        # sum / avg (component-wise)
        sum_body = [
            b.ifelse(b.gt(self.V("fargc"), self.N(0)), [
                *self._qset_a(self.N(0), self.N(0), self.N(0), self.N(0)),
                self.setv("fi", self.N(1)),
                b.repeat(self.V("fargc"), [
                    *self._qset_a(
                        b.add(self.V("a"), b.vblock(b.list_item("args", self.V("fi")))),
                        b.add(self.V("aIm"), b.vblock(b.list_item("argsIm", self.V("fi")))),
                        b.add(self.V("aJ"), b.vblock(b.list_item("argsJ", self.V("fi")))),
                        b.add(self.V("aK"), b.vblock(b.list_item("argsK", self.V("fi")))),
                    ),
                    self.changev("fi", self.N(1)),
                ]),
                *self._qr(self.V("a"), self.V("aIm"), self.V("aJ"), self.V("aK")),
                *result_from_r(),
            ], bad()),
        ]
        branches.append(("sum", sum_body))
        branches.append(("avg", [b.ifelse(b.gt(self.V("fargc"), self.N(0)), [
            *self._qset_a(self.N(0), self.N(0), self.N(0), self.N(0)),
            self.setv("fi", self.N(1)),
            b.repeat(self.V("fargc"), [
                *self._qset_a(
                    b.add(self.V("a"), b.vblock(b.list_item("args", self.V("fi")))),
                    b.add(self.V("aIm"), b.vblock(b.list_item("argsIm", self.V("fi")))),
                    b.add(self.V("aJ"), b.vblock(b.list_item("argsJ", self.V("fi")))),
                    b.add(self.V("aK"), b.vblock(b.list_item("argsK", self.V("fi")))),
                ),
                self.changev("fi", self.N(1)),
            ]),
            *self._qr(b.div(self.V("a"), self.V("fargc")),
                      b.div(self.V("aIm"), self.V("fargc")),
                      b.div(self.V("aJ"), self.V("fargc")),
                      b.div(self.V("aK"), self.V("fargc"))),
            *result_from_r(),
        ], bad())]))
        # dispatch
        cur = self.err("Real only")
        for name, body in reversed(branches):
            cur = [b.ifelse(fn_is(name), body, cur)]
        self.p["qFunc"].body([
            self.setv("fname", b.vblock(b.arg("fn"))),
            self.setv("fargc", b.vblock(b.arg("ac"))),
            cur[0],
        ])
