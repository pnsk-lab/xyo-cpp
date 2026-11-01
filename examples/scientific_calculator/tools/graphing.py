"""Standard-Scratch graph engine, mixed into Evaluator by the generator.

Integration: call _graph_def() after defining plotGraph/evaluate/runRpn/drawLine,
then _graph_gen() instead of gen_plot(). evaluate must publish parseOk independently
of its first numerical evaluation. runRpn must reset err and publish result and its
three imaginary components. Nothing here calls evaluate during sampling.

Coordinates in graphSegX1/Y1/X2/Y2 are clipped STAGE PIXELS, not world coordinates.
The finite 20,000-evaluation budget is per plot. This is a numerical visualization,
not a complete zero-set solver: even-multiplicity roots, isolated points, tiny loops
inside an 8px cell, and sufficiently rapid oscillations can be missed. Residual
validation deliberately prefers gaps over drawing a suspected pole. Refined cells
reuse the coarse row cache, but do not maintain a global fine-grid sample cache.
"""


class GraphingMixin:
    # Expressions below are immutable descriptions, NEVER built reporter IDs.
    # Every _ge traversal emits fresh reporters/input lists (Scratch has one parent).
    def _ge(self, expr):
        b = self.b
        if isinstance(expr, str):
            return b.vvar(expr)
        if isinstance(expr, (int, float)):
            return b.vnum(expr)
        op, *xs = expr
        if op == 'text':
            return b.vtext(xs[0])
        if op == 'arg':
            return b.vblock(b.arg(xs[0]))
        if op == 'item':
            return b.vblock(b.list_item(xs[0], self._ge(xs[1])))
        if op == 'lenlist':
            return b.vblock(b.list_len(xs[0]))
        if op == 'math':
            return b.vblock(b.mathop(xs[0], self._ge(xs[1])))
        if op in ('and_', 'or_', 'not_'):
            return b.vblock(getattr(b, op)(*[self._gb(x) for x in xs]))
        return b.vblock(getattr(b, op)(*[self._ge(x) for x in xs]))

    def _gb(self, expr):
        return self._ge(expr)[1]

    def _gs(self, name, expr):
        return self.b.setv(name, self._ge(expr))

    def _gc(self, name, *args):
        return self.b.call(self.p[name], [self._ge(x) for x in args])

    def _gi(self, cond, yes, no=None):
        if no is None:
            return self.b.if_(self._gb(cond), yes)
        return self.b.ifelse(self._gb(cond), yes, no)

    def _gr(self, count, body):
        return self.b.repeat(self._ge(count), body)

    def _gu(self, cond, body):
        return self.b.repeat_until(self._gb(cond), body)

    def _ga(self, name, expr):
        return self.b.list_add(name, self._ge(expr))

    def _graph_def(self):
        defaults = {'graphMode': 'explicit', 'graphSource': '', 'graphMessage': '',
                    'graphCompiled': 0, 'centerX': 0, 'centerY': 0, 'yval': 0}
        names = '''graphValue graphValid graphBudget graphBudgetMax graphStore
            graphScan graphChar graphClean graphLeft graphRight graphEqual graphSide
            graphIdent graphHasY graphRightHasY graphTrimOut graphTrimStart graphTrimEnd graphHeader graphSize graphTick graphTickPx graphTickIndex
            graphTickScreen graphAxis graphGridLimit graphSavedAngle
            graphClipT0 graphClipT1 graphClipOK graphClipR graphDX graphDY
            graphSX1 graphSY1 graphSX2 graphSY2 graphCX1 graphCY1 graphCX2 graphCY2
            graphEX graphEY graphEF graphEV graphNX graphNY graphNF graphNV
            graphAX graphAY graphAF graphAV graphBX graphBY graphBF graphBV
            graphMX graphMY graphMF graphMV graphError graphWidth
            graphRow graphCol graphRowSlot graphCellX graphCellY graphCellW
            graphCenter graphCenterValid graphMixed graphEdgeCount graphSaddle
            graphEdgeAX graphEdgeAY graphEdgeAF graphEdgeBX graphEdgeBY graphEdgeBF
            graphEdgeLX graphEdgeLY graphEdgeLF graphEdgeHX graphEdgeHY graphEdgeHF
            graphEdgeX graphEdgeY graphEdgeF graphEdgeValid graphEdgeBest
            graphEdgeScale graphEdgeT graphEdgeTestX graphEdgeTestY
            graphPendingX graphPendingY graphPendingW graphLabelX graphLabelY graphView
            graphDragActive graphDragMoved graphDragWasDown graphDragDown
            graphDragStartX graphDragStartY graphDragStartCX graphDragStartCY graphDragScale
            graphDragDX graphDragDY graphDragLastDX graphDragLastDY graphDragIndex
            graphDragMessage graphDragPreviewCount graphDragReplotCount'''.split()
        for name in names:
            defaults.setdefault(name, 0)
        for name, value in defaults.items():
            if name not in self.m.vars:
                self.m.var(name, value)
        for name in ('graphSegX1', 'graphSegY1', 'graphSegX2', 'graphSegY2',
                     'graphRowA', 'graphRowAV', 'graphRowB', 'graphRowBV',
                     'graphPendingXs', 'graphPendingYs', 'graphPendingWs',
                     'graphEdgeXs', 'graphEdgeYs',
                     'graphStackAX', 'graphStackAY', 'graphStackAF', 'graphStackAV',
                     'graphStackBX', 'graphStackBY', 'graphStackBF', 'graphStackBV'):
            if name not in self.m.lists:
                self.m.list(name)
        specs = {
            'graphPrepare': [], 'graphTrim': ['gs'], 'graphSample': ['sx', 'sy'],
            'graphSegment': ['x1', 'y1', 'x2', 'y2'],
            'graphClip': ['gp', 'gq'], 'graphExplicit': [], 'graphImplicit': [],
            'graphEdge': ['ex1', 'ey1', 'ef1', 'ex2', 'ey2', 'ef2'],
            'graphCell': ['cx', 'cy', 'cw', 'f0', 'f1', 'f2', 'f3',
                          'v0', 'v1', 'v2', 'v3'],
            'graphRefine': ['rx', 'ry', 'rw'], 'graphGrid': [], 'graphLabels': [],
            'graphFinish': [], 'graphDragBegin': ['sx', 'sy'],
            'graphDragMove': ['sx', 'sy'], 'graphDragEnd': [], 'graphDragPreview': [],
        }
        for name, args in specs.items():
            self.p[name] = self.b.defproc(name, args, True)

    def _graph_gen(self):
        self._graph_prepare()
        self._graph_sample()
        self._graph_segment()
        self._graph_explicit()
        self._graph_edge()
        self._graph_cell()
        self._graph_implicit()
        self._graph_grid()
        self._graph_labels()
        self._graph_plot()
        self._graph_drag()

    def _graph_prepare(self):
        s, c, i = self._gs, self._gc, self._gi
        T = lambda x: ('text', x)
        # Preserve interior whitespace: deleting it changes x y into identifier xy.
        # Track whole identifier "y", not occurrences in names such as hypot.
        self.p['graphTrim'].body([
            s('graphTrimOut', T('')), s('graphTrimStart', 1),
            s('graphTrimEnd', ('length', ('arg', 'gs'))),
            self._gu(('or_', ('gt', 'graphTrimStart', 'graphTrimEnd'),
                      ('not_', ('contains', T(' \t\n\r'), ('letter', 'graphTrimStart', ('arg', 'gs'))))),
                     [s('graphTrimStart', ('add', 'graphTrimStart', 1))]),
            self._gu(('or_', ('gt', 'graphTrimStart', 'graphTrimEnd'),
                      ('not_', ('contains', T(' \t\n\r'), ('letter', 'graphTrimEnd', ('arg', 'gs'))))),
                     [s('graphTrimEnd', ('sub', 'graphTrimEnd', 1))]),
            self._gr(('add', 1, ('sub', 'graphTrimEnd', 'graphTrimStart')), [
                s('graphTrimOut', ('join', 'graphTrimOut', ('letter', 'graphTrimStart', ('arg', 'gs')))),
                s('graphTrimStart', ('add', 'graphTrimStart', 1)),
            ]),
        ])
        flush = lambda: [i(('eq', 'graphIdent', T('y')), [s('graphHasY', 1)]),
                         s('graphIdent', T(''))]
        self.p['graphPrepare'].body([
            s('graphCompiled', 0), s('graphMessage', T('')),
            s('graphMode', T('explicit')), s('graphSource', T('')),
            s('graphBudget', 0), s('graphBudgetMax', 20000),
            c('normalize', 'graphExpr'), s('graphClean', 'normOut'),
            s('graphLeft', T('')), s('graphRight', T('')), s('graphEqual', 0),
            s('graphSide', 0), s('graphScan', 1), s('graphHasY', 0),
            s('graphIdent', T('')),
            self._gr(('length', 'graphClean'), [
                s('graphChar', ('letter', 'graphScan', 'graphClean')),
                i(('eq', 'graphChar', T('=')), [
                    s('graphEqual', ('add', 'graphEqual', 1)), s('graphSide', 1),
                ], [i(('eq', 'graphSide', 0),
                      [s('graphLeft', ('join', 'graphLeft', 'graphChar'))],
                      [s('graphRight', ('join', 'graphRight', 'graphChar'))])]),
                i(('or_', ('contains', T('abcdefghijklmnopqrstuvwxyz_'), 'graphChar'),
                   ('and_', ('not_', ('eq', 'graphIdent', T(''))),
                    ('contains', T('0123456789'), 'graphChar'))),
                  [s('graphIdent', ('join', 'graphIdent', 'graphChar'))], flush()),
                s('graphScan', ('add', 'graphScan', 1)),
            ]), *flush(),
            c('graphTrim', 'graphLeft'), s('graphLeft', 'graphTrimOut'),
            c('graphTrim', 'graphRight'), s('graphRight', 'graphTrimOut'),
            s('graphRightHasY', 0), s('graphIdent', T('')), s('graphScan', 1),
            self._gr(('length', 'graphRight'), [
                s('graphChar', ('letter', 'graphScan', 'graphRight')),
                i(('or_', ('contains', T('abcdefghijklmnopqrstuvwxyz_'), 'graphChar'),
                   ('and_', ('not_', ('eq', 'graphIdent', T(''))),
                    ('contains', T('0123456789'), 'graphChar'))),
                  [s('graphIdent', ('join', 'graphIdent', 'graphChar'))], [
                      i(('eq', 'graphIdent', T('y')), [s('graphRightHasY', 1)]),
                      s('graphIdent', T('')),
                  ]), s('graphScan', ('add', 'graphScan', 1)),
            ]), i(('eq', 'graphIdent', T('y')), [s('graphRightHasY', 1)]),
            i(('or_', ('gt', 'graphEqual', 1),
               ('or_', ('eq', 'graphLeft', T('')),
                ('and_', ('eq', 'graphEqual', 1), ('eq', 'graphRight', T(''))))),
              [s('graphMessage', T('Invalid equation: use one nonempty lhs=rhs'))], [
                i(('eq', 'graphEqual', 1), [
                    i(('and_', ('eq', 'graphLeft', T('y')),
                       # Self-referential y=... is an implicit equation.
                       ('eq', 'graphRightHasY', 0)),
                      [s('graphSource', 'graphRight')], [
                        s('graphMode', T('implicit')),
                        s('graphSource', ('join', ('join', ('join', T('('), 'graphLeft'),
                                                       T(')-(')), ('join', 'graphRight', T(')')))),
                    ]),
                ], [s('graphSource', 'graphLeft'),
                    i(('eq', 'graphHasY', 1), [s('graphMode', T('implicit'))])]),
                s('xval', 'centerX'), s('yval', 'centerY'),
                c('evaluate', 'graphSource'),
                i(('eq', 'parseOk', 1), [s('graphCompiled', 1)],
                  [s('graphMessage', ('join', T('Parse: '), 'errMsg'))]),
            ]),
        ])

    def _graph_sample(self):
        s, i, c = self._gs, self._gi, self._gc
        self.p['graphSample'].body([
            s('graphValid', 0), s('graphValue', 0),
            i(('and_', ('eq', 'graphCompiled', 1), ('lt', 'graphBudget', 'graphBudgetMax')), [
                s('graphBudget', ('add', 'graphBudget', 1)),
                s('xval', ('add', 'centerX', ('div', ('arg', 'sx'), 'scale'))),
                s('yval', ('add', 'centerY', ('div', ('arg', 'sy'), 'scale'))),
                c('runRpn'),
                i(('and_', ('eq', 'err', 0),
                   ('and_', ('eq', 'resultIm', 0),
                    ('and_', ('eq', 'resultJ', 0), ('eq', 'resultK', 0)))), [
                    # NaN and infinities fail this numeric bound (also avoids
                    # overflow in residual/decider arithmetic).
                    i(('lt', ('math', 'abs', 'result'), 1e100),
                      [s('graphValue', 'result'), s('graphValid', 1)]),
                ]),
            ], [i(('eq', 'graphCompiled', 1),
                  [s('graphMessage', ('text', 'Sample budget reached; partial graph'))])]),
        ])

    def _graph_segment(self):
        s, i, c = self._gs, self._gi, self._gc
        self.p['graphClip'].body([
            i(('eq', ('arg', 'gp'), 0),
              [i(('lt', ('arg', 'gq'), 0), [s('graphClipOK', 0)])], [
                s('graphClipR', ('div', ('arg', 'gq'), ('arg', 'gp'))),
                i(('lt', ('arg', 'gp'), 0), [
                    i(('gt', 'graphClipR', 'graphClipT1'), [s('graphClipOK', 0)]),
                    i(('gt', 'graphClipR', 'graphClipT0'), [s('graphClipT0', 'graphClipR')]),
                ], [
                    i(('lt', 'graphClipR', 'graphClipT0'), [s('graphClipOK', 0)]),
                    i(('lt', 'graphClipR', 'graphClipT1'), [s('graphClipT1', 'graphClipR')]),
                ]),
            ]),
        ])
        body = [s('graphSX1', ('arg', 'x1')), s('graphSY1', ('arg', 'y1')),
                s('graphSX2', ('arg', 'x2')), s('graphSY2', ('arg', 'y2')),
                s('graphDX', ('sub', 'graphSX2', 'graphSX1')),
                s('graphDY', ('sub', 'graphSY2', 'graphSY1')),
                s('graphClipT0', 0), s('graphClipT1', 1), s('graphClipOK', 1)]
        for coord in ('graphSX1', 'graphSY1', 'graphSX2', 'graphSY2'):
            body.append(i(('not_', ('lt', ('math', 'abs', coord), 1e100)), [s('graphClipOK', 0)]))
        body += [c('graphClip', ('sub', 0, 'graphDX'), ('add', 'graphSX1', 232)),
                 c('graphClip', 'graphDX', ('sub', 232, 'graphSX1')),
                 c('graphClip', ('sub', 0, 'graphDY'), ('add', 'graphSY1', 132)),
                 c('graphClip', 'graphDY', ('sub', 148, 'graphSY1'))]
        draw = []
        for out, base, delta, t in [('graphCX1', 'graphSX1', 'graphDX', 'graphClipT0'),
                                    ('graphCY1', 'graphSY1', 'graphDY', 'graphClipT0'),
                                    ('graphCX2', 'graphSX1', 'graphDX', 'graphClipT1'),
                                    ('graphCY2', 'graphSY1', 'graphDY', 'graphClipT1')]:
            draw.append(s(out, ('add', base, ('mul', delta, t))))
        draw += [self.b.op('pen_penUp'),
                 self.b.op('motion_gotoxy', {'X': self._ge('graphCX1'), 'Y': self._ge('graphCY1')}),
                 self.b.op('pen_penDown'),
                 self.b.op('motion_gotoxy', {'X': self._ge('graphCX2'), 'Y': self._ge('graphCY2')}),
                 self.b.op('pen_penUp'),
                 i(('eq', 'graphStore', 1), [self._ga(dst, src) for dst, src in
                    [('graphSegX1', 'graphCX1'), ('graphSegY1', 'graphCY1'),
                     ('graphSegX2', 'graphCX2'), ('graphSegY2', 'graphCY2')]])]
        body.append(i(('eq', 'graphClipOK', 1), draw))
        self.p['graphSegment'].body(body)

    def _graph_explicit(self):
        s, i, c = self._gs, self._gi, self._gc
        stack_names = ['graphStack' + x for x in ('AX', 'AY', 'AF', 'AV', 'BX', 'BY', 'BF', 'BV')]
        push = lambda vals: [self._ga(name, val) for name, val in zip(stack_names, vals)]
        pop = []
        for name in stack_names:
            pop += [s(name.replace('Stack', ''), ('item', name, ('lenlist', name))),
                    self.b.list_delete(name, self._ge(('lenlist', name)))]
        interval = [*pop,
            s('graphWidth', ('sub', 'graphBX', 'graphAX')),
            s('graphMX', ('div', ('add', 'graphAX', 'graphBX'), 2)),
            c('graphSample', 'graphMX', 0), s('graphMF', 'graphValue'), s('graphMV', 'graphValid'),
            s('graphMY', ('mul', ('sub', 'graphMF', 'centerY'), 'scale')),
            s('graphError', ('math', 'abs', ('sub', 'graphMY', ('div', ('add', 'graphAY', 'graphBY'), 2)))),
            i(('and_', ('gt', 'graphWidth', .25),
               ('or_', ('gt', 'graphError', .65),
                ('or_', ('eq', 'graphAV', 0), ('or_', ('eq', 'graphBV', 0), ('eq', 'graphMV', 0))))),
              [*push(['graphMX', 'graphMY', 'graphMF', 'graphMV', 'graphBX', 'graphBY', 'graphBF', 'graphBV']),
               *push(['graphAX', 'graphAY', 'graphAF', 'graphAV', 'graphMX', 'graphMY', 'graphMF', 'graphMV'])],
              [i(('and_', ('lt', 'graphError', 2),
                  ('and_', ('eq', 'graphAV', 1), ('and_', ('eq', 'graphBV', 1), ('eq', 'graphMV', 1)))),
                 [c('graphSegment', 'graphAX', 'graphAY', 'graphMX', 'graphMY'),
                  c('graphSegment', 'graphMX', 'graphMY', 'graphBX', 'graphBY')])]),
        ]
        self.p['graphExplicit'].body([
            *[self.b.list_delete_all(name) for name in stack_names],
            s('graphEX', -232), c('graphSample', 'graphEX', 0),
            s('graphEF', 'graphValue'), s('graphEV', 'graphValid'),
            s('graphEY', ('mul', ('sub', 'graphEF', 'centerY'), 'scale')),
            self._gr(58, [
                s('graphNX', ('add', 'graphEX', 8)), c('graphSample', 'graphNX', 0),
                s('graphNF', 'graphValue'), s('graphNV', 'graphValid'),
                s('graphNY', ('mul', ('sub', 'graphNF', 'centerY'), 'scale')),
                *push(['graphEX', 'graphEY', 'graphEF', 'graphEV', 'graphNX', 'graphNY', 'graphNF', 'graphNV']),
                self._gu(('or_', ('eq', ('lenlist', stack_names[0]), 0),
                          ('not_', ('lt', 'graphBudget', 'graphBudgetMax'))), interval),
                s('graphEX', 'graphNX'), s('graphEY', 'graphNY'),
                s('graphEF', 'graphNF'), s('graphEV', 'graphNV'),
            ]),
        ])

    def _graph_edge(self):
        s, i, c = self._gs, self._gi, self._gc
        # Endpoint sign must differ. Refine the bracket, but only accept when an
        # actual finite residual DECREASES. A sign flip across 1/x is not a root.
        body = []
        for side, arg in [('L', '1'), ('H', '2')]:
            for coord, ac in [('X', 'ex'), ('Y', 'ey'), ('F', 'ef')]:
                body.append(s('graphEdge' + side + coord, ('arg', ac + arg)))
        body += [s('graphEdgeValid', 0), s('graphEdgeBest', 1e100),
                 s('graphEdgeScale', ('math', 'abs', ('arg', 'ef1'))),
                 i(('lt', ('math', 'abs', ('arg', 'ef2')), 'graphEdgeScale'),
                   [s('graphEdgeScale', ('math', 'abs', ('arg', 'ef2')))]),
                 s('graphEdgeT', ('div', ('arg', 'ef1'), ('sub', ('arg', 'ef1'), ('arg', 'ef2'))))]
        test = [
            s('graphEdgeTestX', ('add', 'graphEdgeLX', ('mul', 'graphEdgeT', ('sub', 'graphEdgeHX', 'graphEdgeLX')))),
            s('graphEdgeTestY', ('add', 'graphEdgeLY', ('mul', 'graphEdgeT', ('sub', 'graphEdgeHY', 'graphEdgeLY')))),
            c('graphSample', 'graphEdgeTestX', 'graphEdgeTestY'),
            i(('eq', 'graphValid', 1), [
                i(('lt', ('math', 'abs', 'graphValue'), 'graphEdgeBest'), [
                    s('graphEdgeBest', ('math', 'abs', 'graphValue')),
                    s('graphEdgeX', 'graphEdgeTestX'), s('graphEdgeY', 'graphEdgeTestY'),
                ]),
                i(('eq', ('lt', 'graphValue', 0), ('lt', 'graphEdgeLF', 0)),
                  [s('graphEdgeLX', 'graphEdgeTestX'), s('graphEdgeLY', 'graphEdgeTestY'), s('graphEdgeLF', 'graphValue')],
                  [s('graphEdgeHX', 'graphEdgeTestX'), s('graphEdgeHY', 'graphEdgeTestY'), s('graphEdgeHF', 'graphValue')]),
            ]), s('graphEdgeT', .5),
        ]
        # Early residual success saves most samples on smooth contours.
        body += [self._gr(7, [i(('and_', ('lt', 'graphBudget', 'graphBudgetMax'),
                                ('not_', ('or_', ('eq', 'graphEdgeBest', 0),
                                 ('lt', 'graphEdgeBest', ('mul', .15, 'graphEdgeScale'))))), test)]),
                 i(('or_', ('eq', 'graphEdgeBest', 0),
                    ('lt', 'graphEdgeBest', ('mul', .15, 'graphEdgeScale'))), [
                     s('graphEdgeValid', 1),
                     self._ga('graphEdgeXs', 'graphEdgeX'), self._ga('graphEdgeYs', 'graphEdgeY'),
                     s('graphEdgeCount', ('add', 'graphEdgeCount', 1)),
                 ], [# Keep edge slots aligned, but mark the entire cell unusable.
                     self._ga('graphEdgeXs', 0), self._ga('graphEdgeYs', 0),
                     s('graphMixed', 0), s('graphEdgeCount', ('add', 'graphEdgeCount', 1)),
                 ])]
        self.p['graphEdge'].body(body)

    def _graph_cell(self):
        s, i, c = self._gs, self._gi, self._gc
        A = lambda x: ('arg', x)
        x, y, w = A('cx'), A('cy'), A('cw')
        corners = [(x, y), (('add', x, w), y),
                   (('add', x, w), ('add', y, w)), (x, ('add', y, w))]
        neg = lambda n: ('lt', A('f' + str(n)), 0)
        cross = lambda n, m: ('not_', ('eq', neg(n), neg(m)))
        push = []
        for dx, dy in [(0, 0), (.5, 0), (0, .5), (.5, .5)]:
            push += [self._ga('graphPendingXs', ('add', x, ('mul', dx, w))),
                     self._ga('graphPendingYs', ('add', y, ('mul', dy, w))),
                     self._ga('graphPendingWs', ('div', w, 2))]
        edges = []
        for n, m in [(0, 1), (1, 2), (2, 3), (3, 0)]:
            edges.append(i(cross(n, m), [c('graphEdge', *corners[n], A('f' + str(n)),
                                          *corners[m], A('f' + str(m)))]))
        def segment(n, m):
            return c('graphSegment', ('item', 'graphEdgeXs', n), ('item', 'graphEdgeYs', n),
                     ('item', 'graphEdgeXs', m), ('item', 'graphEdgeYs', m))
        leaf = [s('graphMixed', 1), s('graphEdgeCount', 0),
                self.b.list_delete_all('graphEdgeXs'), self.b.list_delete_all('graphEdgeYs'),
                *edges,
                i(('and_', ('eq', 'graphMixed', 1), ('eq', 'graphCenterValid', 1)), [
                    i(('eq', 'graphEdgeCount', 2), [segment(1, 2)]),
                    i(('eq', 'graphEdgeCount', 4), [
                        # Bilinear asymptotic decider, not arbitrary diagonals.
                        s('graphSaddle', ('sub', ('mul', A('f0'), A('f2')), ('mul', A('f1'), A('f3')))),
                        i(('eq', 'graphSaddle', 0),
                          [i(('eq', ('lt', 'graphCenter', 0), neg(0)),
                             [segment(1, 2), segment(3, 4)], [segment(1, 4), segment(2, 3)])],
                          [i(('gt', 'graphSaddle', 0),
                             [segment(1, 2), segment(3, 4)], [segment(1, 4), segment(2, 3)])]),
                    ]),
                ])]
        valid = ('and_', ('and_', ('eq', A('v0'), 1), ('eq', A('v1'), 1)),
                 ('and_', ('eq', A('v2'), 1), ('eq', A('v3'), 1)))
        mixed = ('or_', cross(0, 1), ('or_', cross(0, 2), cross(0, 3)))
        self.p['graphCell'].body([
            c('graphSample', ('add', x, ('div', w, 2)), ('add', y, ('div', w, 2))),
            s('graphCenter', 'graphValue'), s('graphCenterValid', 'graphValid'),
            i(('and_', valid, ('or_', mixed,
               ('and_', ('eq', 'graphCenterValid', 1), ('not_', ('eq', neg(0), ('lt', 'graphCenter', 0)))))), [
                i(('and_', ('gt', w, 2), ('lt', 'graphBudget', 16000)), push, leaf),
            ]),
        ])
        refine = []
        for n, (dx, dy) in enumerate([(0, 0), (1, 0), (1, 1), (0, 1)]):
            refine += [c('graphSample', ('add', A('rx'), ('mul', dx, A('rw'))),
                         ('add', A('ry'), ('mul', dy, A('rw')))),
                       s('graph' + ['AF', 'BF', 'MF', 'EF'][n], 'graphValue'),
                       s('graph' + ['AV', 'BV', 'MV', 'EV'][n], 'graphValid')]
        refine += [c('graphCell', A('rx'), A('ry'), A('rw'),
                     'graphAF', 'graphBF', 'graphMF', 'graphEF',
                     'graphAV', 'graphBV', 'graphMV', 'graphEV')]
        self.p['graphRefine'].body(refine)

    def _graph_implicit(self):
        s, i, c = self._gs, self._gi, self._gc
        # Two 59-sample rows; swap by copying once per row (no evaluator globals).
        body = [self.b.list_delete_all(name) for name in
                ('graphRowA', 'graphRowAV', 'graphRowB', 'graphRowBV',
                 'graphPendingXs', 'graphPendingYs', 'graphPendingWs')]
        body += [s('graphCol', 0), self._gr(59, [
            c('graphSample', ('add', -232, ('mul', 'graphCol', 8)), -132),
            self._ga('graphRowA', 'graphValue'), self._ga('graphRowAV', 'graphValid'),
            s('graphCol', ('add', 'graphCol', 1)),
        ]), s('graphRow', 0)]
        drain = []
        for axis, dest in [('Xs', 'X'), ('Ys', 'Y'), ('Ws', 'W')]:
            name = 'graphPending' + axis
            drain += [s('graphPending' + dest, ('item', name, ('lenlist', name))),
                      self.b.list_delete(name, self._ge(('lenlist', name)))]
        drain += [c('graphRefine', 'graphPendingX', 'graphPendingY', 'graphPendingW')]
        row = [self.b.list_delete_all('graphRowB'), self.b.list_delete_all('graphRowBV'),
               s('graphCol', 0), self._gr(59, [
                   c('graphSample', ('add', -232, ('mul', 'graphCol', 8)),
                     ('add', -124, ('mul', 'graphRow', 8))),
                   self._ga('graphRowB', 'graphValue'), self._ga('graphRowBV', 'graphValid'),
                   s('graphCol', ('add', 'graphCol', 1)),
               ]), s('graphCol', 1)]
        vals = [('item', name, idx) for name, idx in
                [('graphRowA', 'graphCol'), ('graphRowA', ('add', 'graphCol', 1)),
                 ('graphRowB', ('add', 'graphCol', 1)), ('graphRowB', 'graphCol'),
                 ('graphRowAV', 'graphCol'), ('graphRowAV', ('add', 'graphCol', 1)),
                 ('graphRowBV', ('add', 'graphCol', 1)), ('graphRowBV', 'graphCol')]]
        row += [self._gr(58, [
            i(('lt', 'graphBudget', 'graphBudgetMax'), [
                c('graphCell', ('add', -240, ('mul', 'graphCol', 8)),
                  ('add', -132, ('mul', 'graphRow', 8)), 8, *vals),
                self._gu(('or_', ('eq', ('lenlist', 'graphPendingXs'), 0),
                          ('not_', ('lt', 'graphBudget', 'graphBudgetMax'))), drain),
            ]), s('graphCol', ('add', 'graphCol', 1)),
        ]), self.b.list_delete_all('graphRowA'), self.b.list_delete_all('graphRowAV'),
            s('graphRowSlot', 1), self._gr(59, [
                self._ga('graphRowA', ('item', 'graphRowB', 'graphRowSlot')),
                self._ga('graphRowAV', ('item', 'graphRowBV', 'graphRowSlot')),
                s('graphRowSlot', ('add', 'graphRowSlot', 1)),
            ]), s('graphRow', ('add', 'graphRow', 1))]
        body += [self._gr(35, row)]
        self.p['graphImplicit'].body(body)

    def _graph_pen(self, color, size):
        return [self.b.op('pen_setPenColorToColor', {'COLOR': [1, [9, color]]}),
                self.b.op('pen_setPenSizeTo', {'SIZE': self._ge(size)})]

    def _graph_grid(self):
        s, i, c = self._gs, self._gi, self._gc
        # Nice world tick spacing giving 32..80px grid (1,2,5 times a power of10).
        body = [s('graphStore', 0), s('graphTick', 1),
                self._gu(('not_', ('lt', ('mul', 'graphTick', 'scale'), 32)),
                         [s('graphTick', ('mul', 'graphTick', 10))]),
                self._gu(('not_', ('gt', ('mul', 'graphTick', 'scale'), 320)),
                         [s('graphTick', ('div', 'graphTick', 10))]),
                i(('gt', ('mul', 'graphTick', 'scale'), 160), [s('graphTick', ('div', 'graphTick', 5))],
                  [i(('gt', ('mul', 'graphTick', 'scale'), 80), [s('graphTick', ('div', 'graphTick', 2))])]),
                s('graphTickPx', ('mul', 'graphTick', 'scale'))]
        for axis, center, lo, hi in [('x', 'centerX', -232, 232), ('y', 'centerY', -132, 148)]:
            screen = ('sub', ('mul', 'graphTickIndex', 'graphTickPx'), ('mul', center, 'scale'))
            endpoints = ('graphTickScreen', -132, 'graphTickScreen', 148) if axis == 'x' else (-232, 'graphTickScreen', 232, 'graphTickScreen')
            body += [s('graphTickIndex', ('math', 'ceiling', ('div', ('add', lo, ('mul', center, 'scale')), 'graphTickPx'))),
                     self._gr(('add', 1, ('math', 'floor', ('div', ('sub', hi, screen), 'graphTickPx'))), [
                         s('graphTickScreen', screen), *self._graph_pen('#243553', 1),
                         c('graphSegment', *endpoints),
                         s('graphTickIndex', ('add', 'graphTickIndex', 1)),
                     ])]
        body += [*self._graph_pen('#6f86ad', 1.5),
                 c('graphSegment', ('mul', ('sub', 0, 'centerX'), 'scale'), -132,
                   ('mul', ('sub', 0, 'centerX'), 'scale'), 148),
                 c('graphSegment', -232, ('mul', ('sub', 0, 'centerY'), 'scale'),
                   232, ('mul', ('sub', 0, 'centerY'), 'scale'))]
        self.p['graphGrid'].body(body)

    def _graph_labels(self):
        s, i, c = self._gs, self._gi, self._gc
        body = [s('graphLabelX', ('add', ('mul', ('sub', 0, 'centerX'), 'scale'), 4)),
                s('graphLabelY', ('sub', ('mul', ('sub', 0, 'centerY'), 'scale'), 12))]
        for name, lo, hi in [('graphLabelX', -224, 192), ('graphLabelY', -124, 138)]:
            body += [i(('lt', name, lo), [s(name, lo)]), i(('gt', name, hi), [s(name, hi)])]
        for axis, center, lo, hi in [('x', 'centerX', -212, 210), ('y', 'centerY', -120, 140)]:
            screen = ('sub', ('mul', 'graphTickIndex', 'graphTickPx'), ('mul', center, 'scale'))
            body += [s('graphTickIndex', ('math', 'ceiling', ('div', ('add', lo, ('mul', center, 'scale')), 'graphTickPx'))),
                     self._gr(('add', 1, ('math', 'floor', ('div', ('sub', hi, screen), 'graphTickPx'))), [
                         s('graphTickScreen', screen),
                         i(('or_', ('eq', ('text', axis), ('text', 'x')), ('not_', ('eq', 'graphTickIndex', 0))), [
                             c('fmt', ('mul', 'graphTickIndex', 'graphTick')),
                             c('drawAt', 'fmtOut',
                               ('sub', 'graphTickScreen', 6) if axis == 'x' else 'graphLabelX',
                               'graphLabelY' if axis == 'x' else ('sub', 'graphTickScreen', 1), 48),
                         ]),
                         s('graphTickIndex', ('add', 'graphTickIndex', 1)),
                     ])]
        self.p['graphLabels'].body(body)

    def _graph_plot(self):
        s, i, c = self._gs, self._gi, self._gc
        T = lambda x: ('text', x)
        self.p['plotGraph'].body([
            self.b.op('pen_penUp'), self.b.op('pen_clear'), self.b.op('looks_hide'),
            s('traceValid', 0),
            self.b.op('looks_seteffectto', {'VALUE': self._ge(0)}, {'EFFECT': ['GHOST', None]}),
            *[self.b.list_delete_all(name) for name in ('graphSegX1', 'graphSegY1', 'graphSegX2', 'graphSegY2')],
            # A malformed scale must not produce infinite nice-tick loops.
            i(('not_', ('and_', ('gt', 'scale', .000001), ('lt', 'scale', 1000000))), [s('scale', 20)]),
            c('graphPrepare'), c('graphGrid'), s('graphStore', 1),
            *self._graph_pen('#3fe0c0', 2),
            i(('eq', 'graphCompiled', 1), [
                i(('eq', 'graphMode', T('explicit')), [c('graphExplicit')], [c('graphImplicit')]),
                i(('not_', ('lt', 'graphBudget', 'graphBudgetMax')),
                  [s('graphMessage', T('Sample budget reached; partial graph'))]),
            ]),
            self.b.op('pen_penUp'),
            i(('and_', ('eq', 'graphCompiled', 1),
               ('and_', ('eq', ('lenlist', 'graphSegX1'), 0), ('eq', 'graphMessage', T('')))),
              [s('graphMessage', T('NO REAL CURVE in viewport (sampling may miss roots)'))]),
            c('graphFinish'),
        ])
        self.p['graphFinish'].body([
            c('graphLabels'),
            s('graphHeader', ('join', ('join', 'graphMode', T(': ')), 'graphExpr')),
            s('graphSize', 65), i(('gt', ('length', 'graphHeader'), 52), [s('graphSize', 48)]),
            c('drawLine', 'graphHeader', 168, 'graphSize', 0),
            c('fmt', 'graphTick'), s('graphView', ('join', T('RAD | grid '), 'fmtOut')),
            c('fmt', 'centerX'), s('graphView', ('join', 'graphView', ('join', T(' | center '), 'fmtOut'))),
            c('fmt', 'centerY'), s('graphView', ('join', 'graphView', ('join', T(','), 'fmtOut'))),
            i(('not_', ('eq', 'graphMessage', T(''))), [s('graphView', 'graphMessage')]),
            c('drawLine', 'graphView', 154, 45, 0),
            self.b.op('pen_penUp'),
            self.b.op('looks_switchcostumeto', {'COSTUME': self.b.vmenu(self.b.menu_costume('graphui'))}),
            self.b.op('motion_gotoxy', {'X': self._ge(0), 'Y': self._ge(0)}),
            self.b.op('looks_setsizeto', {'SIZE': self._ge(100)}),
            self.b.op('looks_seteffectto', {'VALUE': self._ge(0)}, {'EFFECT': ['GHOST', None]}),
            self.b.op('looks_show'), s('err', 0), s('errMsg', T('')),
        ])

    def _graph_drag(self):
        """Translate the cached polyline while held; resample only on release.

        The segment cache stays in its ORIGINAL screen coordinates throughout a
        gesture. The preview offset and camera both derive from the press point,
        avoiding accumulated rounding/drift and per-motion equation evaluation.
        Newly exposed areas are resampled when the pointer is released.
        """
        s, i, c = self._gs, self._gi, self._gc
        T = lambda value: ('text', value)
        self.p['graphDragBegin'].body([
            i(('and_', ('eq', 'screen', T('graph')),
                ('and_', ('eq', 'busy', 0),
                 ('and_', ('eq', 'graphDragActive', 0),
                  ('and_', ('and_', ('gt', ('arg', 'sx'), -232), ('lt', ('arg', 'sx'), 232)),
                   ('and_', ('gt', ('arg', 'sy'), -132), ('lt', ('arg', 'sy'), 148)))))), [
                s('graphDragActive', 1), s('busy', 1), s('graphDragMoved', 0),
                s('traceValid', 0), s('traceText', T('')),
                s('graphDragStartX', ('arg', 'sx')), s('graphDragStartY', ('arg', 'sy')),
                s('graphDragStartCX', 'centerX'), s('graphDragStartCY', 'centerY'),
                s('graphDragScale', 'scale'), s('graphDragLastDX', 0), s('graphDragLastDY', 0),
                s('graphDragDX', 0), s('graphDragDY', 0), s('graphDragMessage', 'graphMessage'),
            ]),
        ])
        self.p['graphDragMove'].body([
            i(('eq', 'graphDragActive', 1), [
                s('graphDragDX', ('sub', ('arg', 'sx'), 'graphDragStartX')),
                s('graphDragDY', ('sub', ('arg', 'sy'), 'graphDragStartY')),
                i(('or_', ('gt', ('math', 'abs', 'graphDragDX'), 3),
                          ('gt', ('math', 'abs', 'graphDragDY'), 3)), [s('graphDragMoved', 1)]),
                i(('and_', ('eq', 'graphDragMoved', 1),
                           ('or_', ('not_', ('eq', 'graphDragDX', 'graphDragLastDX')),
                                    ('not_', ('eq', 'graphDragDY', 'graphDragLastDY')))), [
                    s('centerX', ('sub', 'graphDragStartCX', ('div', 'graphDragDX', 'graphDragScale'))),
                    s('centerY', ('sub', 'graphDragStartCY', ('div', 'graphDragDY', 'graphDragScale'))),
                    c('graphDragPreview'),
                    s('graphDragLastDX', 'graphDragDX'), s('graphDragLastDY', 'graphDragDY'),
                ]),
            ]),
        ])
        self.p['graphDragPreview'].body([
            self.b.op('pen_penUp'), self.b.op('pen_clear'), self.b.op('looks_hide'),
            self.b.op('looks_seteffectto', {'VALUE': self._ge(0)}, {'EFFECT': ['GHOST', None]}),
            c('graphGrid'), s('graphStore', 0),
            *self._graph_pen('#3fe0c0', 2), s('graphDragIndex', 1),
            self._gr(('lenlist', 'graphSegX1'), [
                c('graphSegment',
                  ('add', ('item', 'graphSegX1', 'graphDragIndex'), 'graphDragDX'),
                  ('add', ('item', 'graphSegY1', 'graphDragIndex'), 'graphDragDY'),
                  ('add', ('item', 'graphSegX2', 'graphDragIndex'), 'graphDragDX'),
                  ('add', ('item', 'graphSegY2', 'graphDragIndex'), 'graphDragDY')),
                s('graphDragIndex', ('add', 'graphDragIndex', 1)),
            ]),
            s('graphMessage', T('Dragging: preview; release to refine')),
            c('graphFinish'),
            s('graphDragPreviewCount', ('add', 'graphDragPreviewCount', 1)),
        ])
        self.p['graphDragEnd'].body([
            i(('eq', 'graphDragActive', 1), [
                s('graphMessage', 'graphDragMessage'),
                i(('and_', ('eq', 'screen', T('graph')), ('eq', 'graphDragMoved', 1)), [
                    c('plotGraph'),
                    s('graphDragReplotCount', ('add', 'graphDragReplotCount', 1)),
                ]),
                s('graphDragActive', 0), s('busy', 0),
            ]),
        ])
