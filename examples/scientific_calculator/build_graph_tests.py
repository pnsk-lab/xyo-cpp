#!/usr/bin/env python3
"""Generate native Scratch graph-regression tests (no Python plotting fallback)."""
import os
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'tools'))
from model import build_model
from evaluator import Evaluator
from sb3 import Builder, costume, write_sb3

SVG = '<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"><path d="M0 0"/></svg>'


def build(path):
    m = build_model()
    for name in ('graphTestIndex', 'graphTestOK', 'graphTestX', 'graphTestY', 'graphTestMinX', 'graphTestMaxX', 'graphTestBudget', 'graphTestSegments'):
        m.var(name, 0)
    b = Builder(m)
    ev = Evaluator(b)
    p = ev.p
    body = []
    count = 0
    def check(condition, label):
        nonlocal count
        count += 1
        body.append(b.ifelse(condition,
            [b.op('looks_say', {'MESSAGE': b.vtext('pass')})],
            [b.list_add('testLog', b.vtext(label)), b.op('looks_say', {'MESSAGE': b.vtext('fail')})]))
    def prepare(expr):
        body.extend([b.setv('graphExpr', b.vtext(expr)), b.call(p['graphPrepare'], [])])
    def plot(expr):
        body.extend([b.setv('graphExpr', b.vtext(expr)), b.call(p['plotGraph'], [])])
    body += [b.setv('scale', b.vnum(20)), b.setv('centerX', b.vnum(0)), b.setv('centerY', b.vnum(0)),
             b.setv('angleMode', b.vtext('RAD'))]
    for expr, mode in [('sin(x)', 'explicit'), ('y=x^2', 'explicit'), ('x^2+y^2=25', 'implicit'),
                       ('x+2y', 'implicit'), ('y=2y+x', 'implicit'), ('hypot(x,2)', 'explicit')]:
        prepare(expr)
        check(b.and_(b.eqn('graphCompiled', 1), b.eqv('graphMode', mode)), 'prepare: ' + expr)
    for expr in ('x=y=1', '=1', 'x=', 'unknown(x)', 'x+*y'):
        prepare(expr)
        check(b.eqn('graphCompiled', 0), 'reject: ' + expr)
    prepare('x^2+y^2=25')
    body.append(b.call(p['graphSample'], [b.vnum(60), b.vnum(80)]))
    check(b.and_(b.eqn('graphValid', 1), b.lt(b.mathop('abs', b.vvar('graphValue')), b.vnum(1e-9))), 'circle sample at (3,4)')
    body += [b.setv('centerX', b.vnum(1)), b.setv('centerY', b.vnum(-2)),
             b.call(p['graphSample'], [b.vnum(40), b.vnum(120)])]
    check(b.and_(b.eqn('graphValid', 1), b.lt(b.mathop('abs', b.vvar('graphValue')), b.vnum(1e-9))), 'panned circle sample at (3,4)')
    body += [b.setv('centerX', b.vnum(0)), b.setv('centerY', b.vnum(0))]
    prepare('1/x')
    check(b.eqn('graphCompiled', 1), 'domain-invalid origin still compiles')
    body.append(b.call(p['graphSample'], [b.vnum(40), b.vnum(0)]))
    check(b.and_(b.eqn('graphValid', 1), b.eq(b.vvar('graphValue'), b.vnum(.5))), 'recover cached 1/x')
    body.append(b.call(p['graphSample'], [b.vnum(0), b.vnum(0)]))
    check(b.eqn('graphValid', 0), 'reject division by zero')
    prepare('i*x')
    body.append(b.call(p['graphSample'], [b.vnum(40), b.vnum(0)]))
    check(b.eqn('graphValid', 0), 'reject non-real graph sample')
    plot('x^2+y^2=25')
    check(b.gt(b.vblock(b.list_len('graphSegX1')), b.vnum(100)), 'circle has contour segments')
    body += [b.setv('graphTestOK', b.vnum(1)), b.setv('graphTestIndex', b.vnum(1)),
             b.setv('graphTestMinX', b.vnum(1000)), b.setv('graphTestMaxX', b.vnum(-1000))]
    def item(name):
        return b.vblock(b.list_item(name, b.vvar('graphTestIndex')))
    circle_loop = [
        b.setv('graphTestX', b.div(b.add(item('graphSegX1'), item('graphSegX2')), b.vnum(40))),
        b.setv('graphTestY', b.div(b.add(item('graphSegY1'), item('graphSegY2')), b.vnum(40))),
        b.if_(b.gt(b.mathop('abs', b.sub(b.add(b.mul(b.vvar('graphTestX'), b.vvar('graphTestX')),
                                                            b.mul(b.vvar('graphTestY'), b.vvar('graphTestY'))), b.vnum(25))), b.vnum(.03)),
              [b.setv('graphTestOK', b.vnum(0))]),
        b.if_(b.lt(b.vvar('graphTestX'), b.vvar('graphTestMinX')), [b.setv('graphTestMinX', b.vvar('graphTestX'))]),
        b.if_(b.gt(b.vvar('graphTestX'), b.vvar('graphTestMaxX')), [b.setv('graphTestMaxX', b.vvar('graphTestX'))]),
        b.changev('graphTestIndex', b.vnum(1)),
    ]
    body.append(b.repeat(b.vblock(b.list_len('graphSegX1')), circle_loop))
    check(b.eqn('graphTestOK', 1), 'circle segment residual < .03')
    check(b.and_(b.lt(b.vvar('graphTestMinX'), b.vnum(-4.9)), b.gt(b.vvar('graphTestMaxX'), b.vnum(4.9))), 'both sides of circle drawn')
    check(b.lt(b.vvar('graphBudget'), b.vnum(20000)), 'circle below sample budget')
    plot('1/(x-0.13)=0')
    check(b.eq(b.vblock(b.list_len('graphSegX1')), b.vnum(0)), 'implicit pole is not a contour')
    plot('1/(x-0.13)')
    check(b.gt(b.vblock(b.list_len('graphSegX1')), b.vnum(10)), 'explicit reciprocal branches drawn')
    body += [b.setv('graphTestOK', b.vnum(1)), b.setv('graphTestIndex', b.vnum(1))]
    body.append(b.repeat(b.vblock(b.list_len('graphSegX1')), [
        b.if_(b.lt(b.mul(b.sub(item('graphSegX1'), b.vnum(2.6)), b.sub(item('graphSegX2'), b.vnum(2.6))), b.vnum(0)),
              [b.setv('graphTestOK', b.vnum(0))]),
        b.changev('graphTestIndex', b.vnum(1)),
    ]))
    check(b.eqn('graphTestOK', 1), 'explicit pole not bridged')
    plot('x*y=1')
    check(b.gt(b.vblock(b.list_len('graphSegX1')), b.vnum(100)), 'implicit hyperbola branches')
    plot('1e-20/(x-0.13)=0')
    check(b.eq(b.vblock(b.list_len('graphSegX1')), b.vnum(0)), 'tiny-amplitude pole is not a contour')
    body += [b.setv('ans', b.vnum(3)), b.setv('ansIm', b.vnum(4)),
             b.setv('ansJ', b.vnum(0)), b.setv('ansK', b.vnum(0)),
             b.setv('justEval', b.vnum(1)), b.setv('savedAngleMode', b.vtext('DEG')),
             b.call(p['switchToCalc'], [])]
    check(b.and_(b.eqn('result', 3), b.eqn('resultIm', 4)), 'restore calculator answer after plotting')
    check(b.and_(b.eqv('angleMode', 'DEG'), b.and_(b.eqn('xval', 0), b.eqn('yval', 0))), 'restore calc angle and coordinates')
    # Drag lifecycle: pointer-sized jitter is ignored, cached previews never
    # evaluate the expression, and only one high-quality replot happens on release.
    body.append(b.call(p['graphDragBegin'], [b.vnum(0), b.vnum(0)]))
    check(b.eqn('graphDragActive', 0), 'drag disabled in calculator screen')
    body += [b.setv('screen', b.vtext('graph')), b.setv('angleMode', b.vtext('RAD'))]
    plot('x')
    body += [b.setv('graphTestBudget', b.vvar('graphBudget')),
             b.setv('graphTestSegments', b.vblock(b.list_len('graphSegX1'))),
             b.call(p['graphDragBegin'], [b.vnum(0), b.vnum(-157)])]
    check(b.and_(b.eqn('graphDragActive', 0), b.eqn('busy', 0)), 'button row cannot start drag')
    body += [b.setv('traceOn', b.vnum(1)), b.call(p['graphDragBegin'], [b.vnum(0), b.vnum(0)])]
    check(b.and_(b.eqn('graphDragActive', 1), b.eqn('busy', 1)), 'drag captures evaluator lock')
    body.append(b.call(p['graphDragMove'], [b.vnum(2), b.vnum(-2)]))
    check(b.and_(b.eqn('graphDragMoved', 0), b.eqn('graphDragPreviewCount', 0)), 'drag dead zone')
    body.append(b.call(p['graphDragMove'], [b.vnum(40), b.vnum(20)]))
    check(b.and_(b.eqn('centerX', -2), b.eqn('centerY', -1)), 'camera moves opposite to pointer')
    check(b.eq(b.vvar('graphBudget'), b.vvar('graphTestBudget')), 'preview does not sample equation')
    check(b.eq(b.vblock(b.list_len('graphSegX1')), b.vvar('graphTestSegments')), 'preview preserves source cache')
    body.append(b.call(p['graphDragMove'], [b.vnum(40), b.vnum(20)]))
    check(b.and_(b.eqn('graphDragPreviewCount', 1), b.eqn('graphDragReplotCount', 0)), 'unchanged pointer does not redraw')
    body.append(b.call(p['graphDragEnd'], []))
    check(b.and_(b.eqn('busy', 0), b.eqn('graphDragActive', 0)), 'release clears drag lock')
    check(b.and_(b.eqn('graphDragReplotCount', 1), b.eqn('traceOn', 1)), 'release refines once and preserves trace mode')
    body += [b.call(p['graphDragEnd'], []), b.call(p['graphDragBegin'], [b.vnum(0), b.vnum(0)]),
             b.call(p['graphDragEnd'], [])]
    check(b.eqn('graphDragReplotCount', 1), 'plain click and duplicate release do not pan')
    check(b.and_(b.eqn('centerX', -2), b.eqn('centerY', -1)), 'click does not recenter graph')
    body.append(b.op('looks_say', {'MESSAGE': b.vtext('end')}))
    run = b.defproc('graphRegression', [], True)
    run.body(body)
    hat = b._add('event_whenflagclicked', top=True)
    call = b.call(run, []); b.stack(hat, [call]); b.blocks[hat]['next'] = call
    project = {'targets': [b.stage_json([costume('calc', SVG, 1, 1), costume('graph', SVG, 1, 1)]),
        b.sprite_json('Graph tests', [costume('normal', SVG, 1, 1), costume('graphui', SVG, 1, 1)], b.blocks, layer=1, visible=False)],
        'monitors': [], 'extensions': ['pen'], 'meta': {'semver': '3.0.0', 'vm': '0.2.0', 'agent': 'calculator-graph-tests'}}
    write_sb3(path, project, {'transparent': SVG})
    print('wrote', path, 'checks:', count)


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else 'tests/fixtures/calculator_graph_tests.sb3')
