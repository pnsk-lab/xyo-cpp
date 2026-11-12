"""Generated structures for the common numeric CFG tests and benchmarks."""
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_if, S_ifelse, S_repeat, S_set, add, arg, days,
    div, eq, lt, make_project, mod, mul, num, op, sub, text, var,
)

KINDS = ("inline", "if_else", "phi_temp", "helper_args", "helper_branch",
         "nested_calls", "branch_call", "argument_snapshot", "all_combined")


def oracle(count):
    x, acc = 1, 0
    for _ in range(count):
        x = (x * 17 + 23) % 65521
        acc = (acc + x) % 65521
    return [x, acc, x]


def build_body(g, kind, floating=False):
    predicate = lt(mod(var("x"), num(2)), num(1))
    def step(k=num(17), d=num(23)):
        return [S_set("x", mod(add(mul(var("x"), k), d), num(65521))),
                S_set("acc", mod(add(var("acc"), var("x")), num(65521)))]
    if floating:
        body = [S_set("x", div(add(var("x"), num(1)), num(2))),
                S_set("acc", add(var("acc"), var("x")))]
        if kind in ("if_else", "phi_temp"):
            return [S_ifelse(lt(var("x"), num(3)), body, body)]
        return body
    if kind == "inline":
        return step()
    if kind == "if_else":
        return [S_ifelse(predicate, step(), step())]
    if kind == "phi_temp":
        return [S_ifelse(predicate,
                    [S_set("temp", mul(var("x"), num(17)))],
                    [S_set("temp", add(mul(var("x"), num(16)), var("x")))]),
                S_set("x", mod(add(var("temp"), num(23)), num(65521))),
                S_set("acc", mod(add(var("acc"), var("x")), num(65521)))]
    if kind == "argument_snapshot":
        g.define("snapshot %s", ["old"],
                 [S_set("x", add(num(0), num(0))),
                  S_set("x", mod(add(mul(arg("old"), num(17)), num(23)), num(65521))),
                  S_set("acc", mod(add(var("acc"), var("x")), num(65521)))], warp=True)
        return [S_call("snapshot %s", [var("x")])]
    step_body = step(arg("k"), arg("d"))
    if kind == "helper_branch":
        step_body = [S_ifelse(predicate, step_body, step_body)]
    g.define("step %s %s", ["k", "d"], step_body, warp=True)
    call = S_call("step %s %s", [num(17), num(23)])
    if kind == "helper_args" or kind == "helper_branch":
        return [call]
    if kind == "nested_calls":
        g.define("wrap %s", ["k"], [S_call("step %s %s", [arg("k"), num(23)])], warp=True)
        return [S_call("wrap %s", [num(17)])]
    if kind == "branch_call":
        return [S_ifelse(predicate, [call], step())]
    if kind == "all_combined":
        g.define("wrap %s", ["k"], [S_call("step %s %s", [arg("k"), num(23)])], warp=True)
        return [S_ifelse(predicate,
                    [S_call("wrap %s", [var("coeff")])],
                    [S_set("temp", mul(var("x"), var("coeff"))),
                     S_set("x", mod(add(var("temp"), num(23)), num(65521))),
                     S_set("acc", mod(add(var("acc"), var("x")), num(65521)))])]
    raise ValueError(kind)


def project(count, kind, samples=3, warp=True, x=None, acc=None, floating=False):
    g = Gen(["x", "acc", "aux", "coeff", "temp", "t0", "t1"],
            ["times", "checks", "tagprobe"])
    body = build_body(g, kind, floating)
    g.define("kernel", [], [S_set("x", x if x is not None else add(num(0), num(1))),
                           S_set("acc", acc if acc is not None else add(num(0), num(0))),
                           S_set("aux", add(num(0), num(0))),
                           S_set("temp", add(num(0), num(0))),
                           S_set("coeff", text("0017"))] +
             [S_repeat(num(count), body), S_set("aux", var("x"))], warp=warp)
    g.script([S_call("kernel"), S_call("kernel"), S_repeat(num(samples), [
        S_set("t0", days()), S_call("kernel"), S_set("t1", days()),
        S_add("times", mul(sub(var("t1"), var("t0")), num(86400000))),
        S_add("checks", var("x")), S_add("checks", var("acc")), S_add("checks", var("aux")),
    ]), S_add("tagprobe", var("coeff")), S_add("tagprobe", text("Z"))])
    return make_project(g.vars, g.lists, g)


def checkpoint_project(helper=False):
    """One native thread; host test changes coeff only at real Fiber yields."""
    g = Gen(["x", "i", "coeff"], [])
    expression = lambda coefficient: mod(add(mul(var("x"), coefficient), num(23)), num(65521))
    update = [S_set("x", expression(var("coeff")))]
    if helper:
        g.define("step %s", ["coefficient"], [S_set("x", expression(arg("coefficient")))], warp=True)
        update = [S_ifelse(eq(mod(var("i"), num(2)), num(0)),
                           [S_call("step %s", [var("coeff")])], update)]
    g.define("kernel", [], [S_repeat(num(1025), [S_change("i", num(1))] + update)], warp=True)
    g.script([S_set("x", add(num(0), num(1))), S_set("i", add(num(0), num(0))),
              S_set("coeff", text("0017")), S_call("kernel")])
    return make_project(g.vars, g.lists, g)
