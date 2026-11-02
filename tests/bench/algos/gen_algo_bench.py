#!/usr/bin/env python3
"""Famous-algorithm benchmark projects (sb3) for xyo vs TurboWarp.

One project per algorithm/size.  Every project:

  1. builds a deterministic LCG dataset in a *warp* procedure (untimed),
  2. runs the algorithm once as a warm-up (pays JIT/compiler cost),
  3. runs it 3 times, timing each run with `days since 2000` (ms precision
     in both scratch-vm/TurboWarp and xyo),
  4. verifies the result independently (adjacent-order check + sum/hash),
  5. stores the 3 timings in the `times` list.

The harness reads `times` and `check` from the "Bench" sprite, so the same
sb3 files can be run by xyo (`XYO_DUMPVARS=json`) and by scratch-vm /
TurboWarp (`tests/bench/algos/run_tw.js`).

Algorithms (all pure Scratch blocks, no extensions):

  quicksort   in-place Hoare partition, recursive (middle pivot)
  mergesort   top-down recursive merge into an auxiliary list
  bubblesort  O(n^2) baseline with early exit
  sieve       Sieve of Eratosthenes (list of 0/1 flags)
  fib         naive recursive Fibonacci (call-heavy, no lists)

Scratch has no local variables, so recursive procedures save the state they
need after a recursive call on an explicit list stack (quicksort, fib) or
recompute it from the arguments (mergesort) — exactly what a Scratch
programmer has to do.

Usage:
  python3 tests/bench/algos/gen_algo_bench.py [outdir] [--only NAME]
                                             [--sizes qs=4000,ms=4000,...]
"""
import argparse
import json
import os
import zipfile

# --------------------------------------------------------------- expressions

def num(x):
    return ("num", x)


def text(s):
    return ("text", s)


def var(name):
    return ("var", name)


def arg(name):
    """Procedure argument (argument_reporter_string_number)."""
    return ("arg", name)


def op(name, fields=None, **inputs):
    return ("op", name, fields or {}, inputs)


def add(a, b):
    return op("operator_add", NUM1=a, NUM2=b)


def sub(a, b):
    return op("operator_subtract", NUM1=a, NUM2=b)


def mul(a, b):
    return op("operator_multiply", NUM1=a, NUM2=b)


def div(a, b):
    return op("operator_divide", NUM1=a, NUM2=b)


def mod(a, b):
    return op("operator_mod", NUM1=a, NUM2=b)


def lt(a, b):
    return op("operator_lt", OPERAND1=a, OPERAND2=b)


def gt(a, b):
    return op("operator_gt", OPERAND1=a, OPERAND2=b)


def eq(a, b):
    return op("operator_equals", OPERAND1=a, OPERAND2=b)


def le(a, b):
    return op("operator_not", OPERAND=gt(a, b))


def ge(a, b):
    return op("operator_not", OPERAND=lt(a, b))


def join(a, b):
    return op("operator_join", STRING1=a, STRING2=b)


def floor_(a):
    return op("operator_mathop", {"OPERATOR": "floor"}, NUM=a)


def round_(a):
    return op("operator_round", NUM=a)


def days():
    return op("sensing_dayssince2000")


def item(lst, idx):
    return op("data_itemoflist", {"LIST": lst}, INDEX=idx)


def length(lst):
    return op("data_lengthoflist", {"LIST": lst})


# ---------------------------------------------------------------- statements

def S_set(name, e):
    return ("set", name, e)


def S_change(name, e):
    return ("change", name, e)


def S_add(lst, e):
    return ("add", lst, e)


def S_replace(lst, idx, e):
    return ("replace", lst, idx, e)


def S_del(lst, idx):
    return ("delete", lst, idx)


def S_delall(lst):
    return ("deleteall", lst)


def S_if(cond, body):
    return ("if", cond, body)


def S_ifelse(cond, then, other):
    return ("ifelse", cond, then, other)


def S_repeat(times, body):
    return ("repeat", times, body)


def S_until(cond, body):
    return ("until", cond, body)


def S_call(proccode, args=()):
    return ("call", proccode, list(args))


# Boolean result opcodes: emitted as [2, id] into a boolean input slot.
BOOL_OPS = {
    "operator_lt", "operator_gt", "operator_equals",
    "operator_and", "operator_or", "operator_not",
    "data_listcontainsitem", "operator_contains",
}
# Boolean input names per opcode (the child is a boolean reporter).
BOOL_INPUTS = {
    "control_if": {"CONDITION"},
    "control_if_else": {"CONDITION"},
    "control_repeat_until": {"CONDITION"},
    "control_wait_until": {"CONDITION"},
    "operator_and": {"OPERAND1", "OPERAND2"},
    "operator_or": {"OPERAND1", "OPERAND2"},
    "operator_not": {"OPERAND"},
}


# ---------------------------------------------------------------- sb3 build

class Gen:
    """Tiny sb3 block-graph builder (project.json dict format)."""

    def __init__(self, variables, lists):
        self.blocks = {}
        self.n = 0
        self.vars = {name: "v_" + name for name in variables}
        self.lists = {name: "l_" + name for name in lists}
        self.procs = {}          # proccode -> mutation template

    def new(self, opcode, fields=None, mutation=None, shadow=False,
            top=False, x=0, y=0, parent=None):
        self.n += 1
        bid = "b%04d" % self.n
        blk = {"opcode": opcode, "next": None, "parent": parent,
               "inputs": {}, "fields": fields or {},
               "topLevel": top, "shadow": shadow}
        if mutation is not None:
            blk["mutation"] = mutation
        if top:
            blk["x"] = x
            blk["y"] = y
        self.blocks[bid] = blk
        return bid

    def field(self, name, value):
        if name == "VARIABLE":
            return [value, self.vars[value]]
        if name == "LIST":
            return [value, self.lists[value]]
        return [value, None]

    # -- expressions -------------------------------------------------------
    def emit_value(self, e, parent, bool_slot=False):
        tag = e[0]
        if tag == "num":
            return [1, [4, fmt_num(e[1])]]
        if tag == "text":
            return [1, [10, str(e[1])]]
        if tag == "var":
            name = e[1]
            bid = self.new("data_variable", parent=parent,
                           fields={"VARIABLE": self.field("VARIABLE", name)})
            return [2, bid] if bool_slot else [3, bid, [10, ""]]
        if tag == "arg":
            bid = self.new("argument_reporter_string_number", parent=parent,
                           fields={"VALUE": [e[1], None]})
            return [2, bid] if bool_slot else [3, bid, [10, ""]]
        assert tag == "op", e
        _, opcode, fields, inputs = e
        bid = self.new(opcode, parent=parent,
                       fields={k: self.field(k, v) for k, v in fields.items()})
        for iname, sub in inputs.items():
            bslot = iname in BOOL_INPUTS.get(opcode, ())
            self.blocks[bid]["inputs"][iname] = self.emit_value(
                sub, bid, bslot)
        if opcode in BOOL_OPS:
            assert bool_slot, "boolean reporter %s in a value slot" % opcode
            return [2, bid]
        assert not bool_slot, "value reporter %s in a boolean slot" % opcode
        return [3, bid, [10, ""]]

    # -- statements --------------------------------------------------------
    def emit_stmt(self, s, parent=None):
        k = s[0]
        if k == "set":
            _, name, e = s
            bid = self.new("data_setvariableto", parent=parent,
                           fields={"VARIABLE": self.field("VARIABLE", name)})
            self.blocks[bid]["inputs"]["VALUE"] = self.emit_value(e, bid)
            return bid
        if k == "change":
            _, name, e = s
            bid = self.new("data_changevariableby", parent=parent,
                           fields={"VARIABLE": self.field("VARIABLE", name)})
            self.blocks[bid]["inputs"]["VALUE"] = self.emit_value(e, bid)
            return bid
        if k == "add":
            _, name, e = s
            bid = self.new("data_addtolist", parent=parent,
                           fields={"LIST": self.field("LIST", name)})
            self.blocks[bid]["inputs"]["ITEM"] = self.emit_value(e, bid)
            return bid
        if k == "replace":
            _, name, idx, e = s
            bid = self.new("data_replaceitemoflist", parent=parent,
                           fields={"LIST": self.field("LIST", name)})
            self.blocks[bid]["inputs"]["INDEX"] = self.emit_value(idx, bid)
            self.blocks[bid]["inputs"]["ITEM"] = self.emit_value(e, bid)
            return bid
        if k == "delete":
            _, name, idx = s
            bid = self.new("data_deleteoflist", parent=parent,
                           fields={"LIST": self.field("LIST", name)})
            self.blocks[bid]["inputs"]["INDEX"] = self.emit_value(idx, bid)
            return bid
        if k == "deleteall":
            return self.new("data_deletealloflist", parent=parent,
                            fields={"LIST": self.field("LIST", s[1])})
        if k in ("if", "until", "repeat", "ifelse"):
            opcode = {"if": "control_if", "until": "control_repeat_until",
                      "repeat": "control_repeat",
                      "ifelse": "control_if_else"}[k]
            bid = self.new(opcode, parent=parent)
            if k == "repeat":
                self.blocks[bid]["inputs"]["TIMES"] = self.emit_value(s[1], bid)
                body = s[2]
            else:
                self.blocks[bid]["inputs"]["CONDITION"] = self.emit_value(
                    s[1], bid, True)
                body = s[2]
            first = self.chain(body, bid)
            if first:
                self.blocks[bid]["inputs"]["SUBSTACK"] = [2, first]
            if k == "ifelse":
                first2 = self.chain(s[3], bid)
                if first2:
                    self.blocks[bid]["inputs"]["SUBSTACK2"] = [2, first2]
            return bid
        if k == "call":
            _, proccode, args = s
            tmpl = self.procs[proccode]
            bid = self.new("procedures_call", parent=parent,
                           mutation=json.loads(json.dumps(tmpl)))
            for aid, e in zip(json.loads(tmpl["argumentids"]), args):
                self.blocks[bid]["inputs"][aid] = self.emit_value(e, bid)
            return bid
        raise ValueError(k)

    def chain(self, stmts, parent=None):
        ids = [self.emit_stmt(s, parent) for s in stmts]
        for a, b in zip(ids, ids[1:]):
            self.blocks[a]["next"] = b
            self.blocks[b]["parent"] = a
        return ids[0] if ids else None

    def script(self, body, x=0, y=0):
        hat = self.new("event_whenflagclicked", top=True, x=x, y=y)
        first = self.chain(body, hat)
        self.blocks[hat]["next"] = first
        return hat

    def define(self, proccode, argnames, body, x=0, y=0, warp=True):
        argids = ["arg%d" % i for i in range(len(argnames))]
        mut = {"tagName": "mutation", "children": [],
               "proccode": proccode,
               "argumentids": json.dumps(argids),
               "argumentnames": json.dumps(argnames),
               "argumentdefaults": json.dumps(["0"] * len(argnames)),
               "warp": "true" if warp else "false"}
        self.procs[proccode] = mut
        d = self.new("procedures_definition", top=True, x=x, y=y)
        proto = self.new("procedures_prototype", shadow=True, parent=d,
                         mutation=json.loads(json.dumps(mut)))
        self.blocks[d]["inputs"]["custom_block"] = [1, proto]
        for aid, an in zip(argids, argnames):
            sh = self.new("argument_reporter_string_number", shadow=True,
                          parent=proto, fields={"VALUE": [an, None]})
            self.blocks[proto]["inputs"][aid] = [1, sh]
        first = self.chain(body, d)
        self.blocks[d]["next"] = first
        return d


def fmt_num(x):
    if isinstance(x, float) and x.is_integer():
        x = int(x)
    return str(x)


# ---------------------------------------------------------------- project

def make_project(varnames, listnames, gen, list_values=None):
    stage = {
        "isStage": True, "name": "Stage", "variables": {}, "lists": {},
        "broadcasts": {}, "blocks": {}, "comments": {},
        "costumes": [], "sounds": [], "currentCostume": 0, "volume": 100,
        "layerOrder": 0, "tempo": 60, "videoTransparency": 50,
        "videoState": "on", "textToSpeechLanguage": None,
    }
    sprite = {
        "isStage": False, "name": "Bench", "layerOrder": 1,
        "variables": {gen.vars[n]: [n, 0] for n in varnames},
        "lists": {gen.lists[n]: [n, (list_values or {}).get(n, [])]
                  for n in listnames},
        "broadcasts": {}, "blocks": gen.blocks, "comments": {},
        "costumes": [], "sounds": [], "currentCostume": 0, "volume": 100,
        "visible": False, "x": 0, "y": 0, "size": 100, "direction": 90,
        "draggable": False, "rotationStyle": "all around",
    }
    return {"targets": [stage, sprite], "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": "algo-bench"}}


def write_sb3(path, project):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project, separators=(",", ":")))


# ---------------------------------------------------------------- datasets

SEED0 = 12345
LCG_A, LCG_B, LCG_M = 25173, 13849, 65536


def lcg(n):
    out = []
    s = SEED0
    for _ in range(n):
        s = (s * LCG_A + LCG_B) % LCG_M
        out.append(s)
    return out


def lcg_expr():
    """seed = (seed * 25173 + 13849) mod 65536"""
    return mod(add(mul(var("seed"), num(LCG_A)), num(LCG_B)), num(LCG_M))


def build_data(n, with_aux):
    body = [
        S_delall("data"),
        S_set("seed", num(SEED0)),
        S_repeat(num(n), [
            S_set("seed", lcg_expr()),
            S_add("data", var("seed")),
        ]),
    ]
    if with_aux:
        body += [
            S_delall("aux"),
            S_repeat(num(n), [S_add("aux", num(0))]),
        ]
    return body


def check_sorted_body():
    """Independent verification of the final `data` list."""
    one = num(1)
    return [
        S_set("sum", num(0)),
        S_set("hash", num(0)),
        S_set("ok", one),
        S_set("k", one),
        S_until(gt(var("k"), length("data")), [
            S_change("sum", item("data", var("k"))),
            S_set("hash", mod(add(mul(var("hash"), num(31)),
                                 item("data", var("k"))), num(1000000007))),
            S_change("k", one),
        ]),
        # no adjacent inversion => sorted
        S_set("k", one),
        S_until(gt(var("k"), sub(length("data"), one)), [
            S_if(gt(item("data", var("k")),
                    item("data", add(var("k"), one))),
                 [S_set("ok", num(0))]),
            S_change("k", one),
        ]),
        S_set("check", join(join(var("sum"), text(":")),
                            join(var("hash"), join(text(":"), var("ok"))))),
    ]


def driver(build_stmts, warmup_stmts, algo, verify):
    """warm-up, then 3 timed runs (dataset rebuilt between runs).

    `build_stmts` is a list so algorithms whose input *is* the dataset
    (sieve) can pass [] and time their own build.  The warm-up is a list
    too: it may need its own (smaller) dataset.
    """
    return ([S_delall("times")] + list(build_stmts) +
            list(warmup_stmts) +             # JIT / TW compile cost
            [S_repeat(num(3), list(build_stmts) + [
                S_set("t0", days()),
                algo,
                S_set("t1", days()),
                S_add("times", mul(sub(var("t1"), var("t0")),
                                   num(86400000))),
            ])] +
            [verify])


# ---------------------------------------------------------------- algorithms

def gen_quicksort(n):
    g = Gen(["i", "j", "k", "pivot", "tmp", "seed", "t0", "t1",
             "sum", "hash", "ok", "check"],
            ["data", "stack", "times"])
    lo, hi = arg("lo"), arg("hi")
    g.define("build %s", ["n"], build_data(n, False) + [S_delall("stack")],
             x=0, y=500)
    g.define("qs %s %s", ["lo", "hi"], [
        S_if(lt(lo, hi), [
            S_set("pivot", item("data", floor_(div(add(lo, hi), num(2))))),
            S_set("i", lo),
            S_set("j", hi),
            S_until(gt(var("i"), var("j")), [
                S_until(ge(item("data", var("i")), var("pivot")),
                        [S_change("i", num(1))]),
                S_until(le(item("data", var("j")), var("pivot")),
                        [S_change("j", num(-1))]),
                S_if(le(var("i"), var("j")), [
                    S_set("tmp", item("data", var("i"))),
                    S_replace("data", var("i"), item("data", var("j"))),
                    S_replace("data", var("j"), var("tmp")),
                    S_change("i", num(1)),
                    S_change("j", num(-1)),
                ]),
            ]),
            # Scratch has no locals: save the range bounds that are still
            # needed after the first recursive call on a list stack.
            S_add("stack", var("i")),
            S_add("stack", var("j")),
            S_if(lt(lo, var("j")), [S_call("qs %s %s", [lo, var("j")])]),
            S_if(lt(item("stack", sub(length("stack"), num(1))), hi), [
                S_call("qs %s %s",
                       [item("stack", sub(length("stack"), num(1))), hi])]),
            S_del("stack", length("stack")),
            S_del("stack", length("stack")),
        ]),
    ], x=0, y=900)
    g.define("verify", [], check_sorted_body(), x=500, y=500)
    g.script(driver([S_call("build %s", [num(n)])],
                    [S_call("qs %s %s", [num(1), num(n)])],
                    S_call("qs %s %s", [num(1), num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_mergesort(n):
    g = Gen(["i", "j", "k", "mid", "tmp", "seed", "t0", "t1",
             "sum", "hash", "ok", "check"],
            ["data", "aux", "times"])
    lo, hi = arg("lo"), arg("hi")
    midx = floor_(div(add(lo, hi), num(2)))
    g.define("build %s", ["n"], build_data(n, True), x=0, y=500)
    g.define("ms %s %s", ["lo", "hi"], [
        S_if(lt(lo, hi), [
            S_call("ms %s %s", [lo, midx]),
            # lo/hi are arguments (stack-passed), so mid can be recomputed
            # after the first call instead of being saved on a list.
            S_call("ms %s %s", [add(floor_(div(add(lo, hi), num(2))), num(1)),
                                hi]),
            S_set("mid", floor_(div(add(lo, hi), num(2)))),
            S_set("i", lo),
            S_set("j", add(var("mid"), num(1))),
            S_set("k", lo),
            S_until(gt(var("k"), hi), [
                S_ifelse(gt(var("i"), var("mid")), [
                    S_replace("aux", var("k"), item("data", var("j"))),
                    S_change("j", num(1)),
                ], [
                    S_ifelse(gt(var("j"), hi), [
                        S_replace("aux", var("k"), item("data", var("i"))),
                        S_change("i", num(1)),
                    ], [
                        S_ifelse(le(item("data", var("i")),
                                    item("data", var("j"))), [
                            S_replace("aux", var("k"), item("data", var("i"))),
                            S_change("i", num(1)),
                        ], [
                            S_replace("aux", var("k"), item("data", var("j"))),
                            S_change("j", num(1)),
                        ]),
                    ]),
                ]),
                S_change("k", num(1)),
            ]),
            S_set("k", lo),
            S_until(gt(var("k"), hi), [
                S_replace("data", var("k"), item("aux", var("k"))),
                S_change("k", num(1)),
            ]),
        ]),
    ], x=0, y=900)
    g.define("verify", [], check_sorted_body(), x=500, y=500)
    g.script(driver([S_call("build %s", [num(n)])],
                    [S_call("ms %s %s", [num(1), num(n)])],
                    S_call("ms %s %s", [num(1), num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_bubblesort(n):
    g = Gen(["i", "j", "k", "tmp", "swapped", "seed", "t0", "t1",
             "sum", "hash", "ok", "check"],
            ["data", "times"])
    g.define("build %s", ["n"], build_data(n, False), x=0, y=500)
    g.define("bubble %s", ["n"], [
        S_set("i", num(1)),
        S_until(gt(var("i"), arg("n")), [
            S_set("swapped", num(0)),
            S_set("j", num(1)),
            S_until(gt(var("j"), sub(arg("n"), var("i"))), [
                S_if(gt(item("data", var("j")),
                        item("data", add(var("j"), num(1)))), [
                    S_set("tmp", item("data", var("j"))),
                    S_replace("data", var("j"),
                              item("data", add(var("j"), num(1)))),
                    S_replace("data", add(var("j"), num(1)), var("tmp")),
                    S_set("swapped", num(1)),
                ]),
                S_change("j", num(1)),
            ]),
            S_if(eq(var("swapped"), num(0)), [S_set("i", arg("n"))]),
            S_change("i", num(1)),
        ]),
    ], x=0, y=900)
    g.define("verify", [], check_sorted_body(), x=500, y=500)
    g.script(driver([S_call("build %s", [num(n)])],
                    [S_call("bubble %s", [num(n)])],
                    S_call("bubble %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_sieve(n):
    g = Gen(["i", "j", "k", "count", "sum", "hash", "ok", "check",
             "seed", "t0", "t1"],
            ["flags", "times"])
    nn = arg("n")
    # The 2M-item flag list is input data: build it untimed (outside the
    # measured region), exactly like the sort benchmarks build their arrays.
    # Otherwise the timed region is dominated by list growth and V8's GC,
    # which made this one number vary by 30% between runs.
    g.define("build_flags %s", ["n"], [
        S_delall("flags"),
        S_repeat(nn, [S_add("flags", num(1))]),
    ], x=0, y=300)
    g.define("sieve %s", ["n"], [
        S_replace("flags", num(1), num(0)),
        S_set("i", num(2)),
        S_until(gt(mul(var("i"), var("i")), nn), [
            S_if(eq(item("flags", var("i")), num(1)), [
                S_set("j", mul(var("i"), var("i"))),
                S_until(gt(var("j"), nn), [
                    S_replace("flags", var("j"), num(0)),
                    S_change("j", var("i")),
                ]),
            ]),
            S_change("i", num(1)),
        ]),
        S_set("count", num(0)),
        S_set("k", num(1)),
        S_repeat(nn, [
            S_change("count", item("flags", var("k"))),
            S_change("k", num(1)),
        ]),
    ], x=0, y=500)
    g.define("verify", [], [
        S_set("sum", num(0)),
        S_set("hash", num(0)),
        S_set("ok", num(1)),
        S_set("k", num(1)),
        S_until(gt(var("k"), length("flags")), [
            S_if(eq(item("flags", var("k")), num(1)), [
                S_set("hash", mod(add(mul(var("hash"), num(31)), var("k")),
                                  num(1000000007))),
            ]),
            S_change("k", num(1)),
        ]),
        S_set("check", join(var("count"),
                            join(text(":"), join(var("hash"),
                                                 join(text(":"), var("ok")))))),
    ], x=500, y=500)
    g.script(driver([S_call("build_flags %s", [num(n)])],
                    [S_call("sieve %s", [num(n)])],
                    S_call("sieve %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_fib(n):
    g = Gen(["res", "t0", "t1", "check", "ok"],
            ["stack", "times"])
    nn = arg("n")
    g.define("fib %s", ["n"], [
        S_ifelse(lt(nn, num(2)), [
            S_set("res", nn),
        ], [
            S_call("fib %s", [sub(nn, num(1))]),
            S_add("stack", var("res")),
            S_call("fib %s", [sub(nn, num(2))]),
            S_set("res", add(item("stack", length("stack")), var("res"))),
            S_del("stack", length("stack")),
        ]),
    ], x=0, y=500)
    g.define("verify", [], [
        S_set("ok", num(1)),
        S_set("check", join(var("res"), join(text(":"), var("ok")))),
    ], x=500, y=500)
    g.script(driver([],
                    [S_call("fib %s", [num(10)])],
                    S_call("fib %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


# ---------------------------------------------------------------- sha256

# FIPS 180-4 constants.
SHA256_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]
SHA256_H0 = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
POW2 = [1 << i for i in range(33)]                  # 2^0 .. 2^32
POW256 = [1, 256, 65536, 16777216]
XOR_TABLE = [a ^ b for a in range(256) for b in range(256)]


def _rot3(shifts, right_shift=None):
    """res = rotr(x, s0) xor rotr(x, s1) xor (rotr(x, s2) | x >> n)."""
    body = [
        S_call("rotr32 %s %s", [arg("x"), num(shifts[0])]),
        S_set("x1", var("res")),
        S_call("rotr32 %s %s", [arg("x"), num(shifts[1])]),
        S_set("x2", var("res")),
    ]
    if right_shift is None:
        body += [S_call("rotr32 %s %s", [arg("x"), num(shifts[2])]),
                 S_set("x3", var("res"))]
    else:
        body += [S_set("x3", floor_(div(arg("x"), num(right_shift))))]
    body += [
        S_call("xor32 %s %s", [var("x1"), var("x2")]),
        S_set("x4", var("res")),
        S_call("xor32 %s %s", [var("x4"), var("x3")]),
    ]
    return body


def gen_sha256(n=16384):
    """SHA-256 of an n-byte LCG message.

    Scratch has no bitwise operators, so 32-bit XOR is a 4×(byte XOR)
    lookup into a baked 64 KiB table (x ^ y per byte pair) and AND follows
    from x + y = (x xor y) + 2*(x and y).  ROTR and NOT are pure arithmetic:
    rotr(x,n) = floor(x/2^n) + (x mod 2^n)*2^(32-n), not(x) = 2^32-1-x.
    This is the usual way Scratch projects do hashing.
    """
    varnames = ["a", "b", "c", "d", "e", "f", "g", "h",
                "t", "pos", "bp", "nblocks", "res",
                "x1", "x2", "x3", "x4", "u1", "u2", "u3", "u4",
                "seed", "bits", "H1", "H2", "H3", "H4", "H5", "H6", "H7",
                "H8", "t0", "t1", "check"]
    listnames = ["msg", "W", "times", "xt", "K", "H0", "pow2", "pow256"]
    g = Gen(varnames, listnames)

    # -- 32-bit primitives ------------------------------------------------
    terms = []
    for kk in POW256:
        idx = add(mul(mod(floor_(div(arg("x"), num(kk))), num(256)), num(256)),
                  add(mod(floor_(div(arg("y"), num(kk))), num(256)), num(1)))
        term = mul(item("xt", idx), num(kk))
        terms.append(S_set("res", term) if kk == 1 else S_change("res", term))
    g.define("xor32 %s %s", ["x", "y"], terms, x=0, y=520)

    g.define("and32 %s %s", ["x", "y"], [
        S_call("xor32 %s %s", [arg("x"), arg("y")]),
        S_set("res", div(sub(add(arg("x"), arg("y")), var("res")), num(2))),
    ], x=0, y=760)

    pown = item("pow2", add(arg("n"), num(1)))
    g.define("rotr32 %s %s", ["x", "n"], [
        S_set("res", add(floor_(div(arg("x"), pown)),
                         mul(mod(arg("x"), pown),
                             item("pow2", sub(num(33), arg("n")))))),
    ], x=0, y=960)

    g.define("sigma0 %s", ["x"], _rot3((7, 18), 8), x=0, y=1160)
    g.define("sigma1 %s", ["x"], _rot3((17, 19), 1024), x=0, y=1360)
    g.define("Sigma0 %s", ["x"], _rot3((2, 13, 22)), x=0, y=1560)
    g.define("Sigma1 %s", ["x"], _rot3((6, 11, 25)), x=0, y=1760)

    g.define("ch %s %s %s", ["x", "y", "z"], [
        S_call("and32 %s %s", [arg("x"), arg("y")]),
        S_set("x1", var("res")),
        S_set("x2", sub(num(4294967295), arg("x"))),
        S_call("and32 %s %s", [var("x2"), arg("z")]),
        S_set("x3", var("res")),
        S_call("xor32 %s %s", [var("x1"), var("x3")]),
    ], x=0, y=1960)

    g.define("maj %s %s %s", ["x", "y", "z"], [
        S_call("and32 %s %s", [arg("x"), arg("y")]),
        S_set("x1", var("res")),
        S_call("and32 %s %s", [arg("x"), arg("z")]),
        S_set("x2", var("res")),
        S_call("and32 %s %s", [arg("y"), arg("z")]),
        S_set("x3", var("res")),
        S_call("xor32 %s %s", [var("x1"), var("x2")]),
        S_set("x4", var("res")),
        S_call("xor32 %s %s", [var("x4"), var("x3")]),
    ], x=0, y=2160)

    # -- message schedule + compression ----------------------------------
    bpexpr = add(add(var("pos"), mul(sub(var("t"), num(1)), num(4))), num(1))
    word = add(mul(add(mul(add(mul(item("msg", var("bp")), num(256)),
                            item("msg", add(var("bp"), num(1)))), num(256)),
                        item("msg", add(var("bp"), num(2)))), num(256)),
                  item("msg", add(var("bp"), num(3))))

    def w(i):
        return item("W", i)

    schedule = [
        S_set("t", num(17)),
        S_repeat(num(48), [
            S_call("sigma1 %s", [w(sub(var("t"), num(2)))]),
            S_set("u1", var("res")),
            S_call("sigma0 %s", [w(sub(var("t"), num(15)))]),
            S_set("u2", var("res")),
            S_set("u3", mod(add(add(add(var("u1"), w(sub(var("t"), num(7)))),
                                    var("u2")),
                                w(sub(var("t"), num(16)))),
                            num(4294967296))),
            S_replace("W", var("t"), var("u3")),
            S_change("t", num(1)),
        ]),
    ]

    rounds = [
        S_call("Sigma0 %s", [var("a")]),
        S_set("u1", var("res")),
        S_call("maj %s %s %s", [var("a"), var("b"), var("c")]),
        S_set("u2", var("res")),
        S_set("u3", mod(add(var("u1"), var("u2")), num(4294967296))),
        S_call("Sigma1 %s", [var("e")]),
        S_set("u1", var("res")),
        S_call("ch %s %s %s", [var("e"), var("f"), var("g")]),
        S_set("u2", var("res")),
        S_set("u4", mod(add(add(add(add(var("h"), var("u1")), var("u2")),
                              item("K", var("t"))), w(var("t"))),
                        num(4294967296))),
        S_set("h", var("g")),
        S_set("g", var("f")),
        S_set("f", var("e")),
        S_set("e", mod(add(var("d"), var("u4")), num(4294967296))),
        S_set("d", var("c")),
        S_set("c", var("b")),
        S_set("b", var("a")),
        S_set("a", mod(add(var("u4"), var("u3")), num(4294967296))),
        S_change("t", num(1)),
    ]

    g.define("sha256", [], [
        S_set("nblocks", floor_(div(length("msg"), num(64)))),
        S_set("pos", num(0)),
    ] + [S_set("H%d" % i, item("H0", num(i))) for i in range(1, 9)] + [
        S_repeat(var("nblocks"), [
            S_set("t", num(1)),
            S_repeat(num(16), [
                S_set("bp", bpexpr),
                S_replace("W", var("t"), word),
                S_change("t", num(1)),
            ]),
        ] + schedule + [
            S_set("a", var("H1")), S_set("b", var("H2")),
            S_set("c", var("H3")), S_set("d", var("H4")),
            S_set("e", var("H5")), S_set("f", var("H6")),
            S_set("g", var("H7")), S_set("h", var("H8")),
            S_set("t", num(1)),
            S_repeat(num(64), rounds),
        ] + [
            S_set("H%d" % i,
                  mod(add(var("H%d" % i), var(chr(ord("a") + i - 1))),
                      num(4294967296)))
            for i in range(1, 9)
        ] + [
            S_change("pos", num(64)),
        ]),
    ], x=0, y=2400)

    # -- untimed input: LCG bytes + SHA-256 padding ----------------------
    g.define("build_msg %s", ["n"], [
        S_delall("msg"),
        S_set("seed", num(42)),
        S_repeat(arg("n"), [
            S_set("seed", mod(add(mul(var("seed"), num(LCG_A)), num(LCG_B)),
                              num(LCG_M))),
            S_add("msg", mod(var("seed"), num(256))),
        ]),
        S_add("msg", num(128)),
        S_until(eq(mod(length("msg"), num(64)), num(56)),
                [S_add("msg", num(0))]),
        S_set("bits", mul(arg("n"), num(8))),
        S_add("msg", num(0)), S_add("msg", num(0)),
        S_add("msg", num(0)), S_add("msg", num(0)),
        S_add("msg", mod(floor_(div(var("bits"), num(16777216))), num(256))),
        S_add("msg", mod(floor_(div(var("bits"), num(65536))), num(256))),
        S_add("msg", mod(floor_(div(var("bits"), num(256))), num(256))),
        S_add("msg", mod(var("bits"), num(256))),
        S_delall("W"),
        S_repeat(num(64), [S_add("W", num(0))]),
    ], x=0, y=300)

    digest = var("H1")
    for i in range(2, 9):
        digest = join(digest, join(text(":"), var("H%d" % i)))
    g.define("verify", [], [S_set("check", digest)], x=500, y=500)

    g.script(driver([S_call("build_msg %s", [num(n)])],
                    [S_call("build_msg %s", [num(256)]),
                     S_call("sha256")],
                    S_call("sha256"),
                    S_call("verify")), x=0, y=0)

    list_values = {
        "K": SHA256_K,
        "H0": SHA256_H0,
        "pow2": POW2,
        "pow256": POW256,
        "xt": XOR_TABLE,
    }
    return make_project(varnames, listnames, g, list_values)



# ------------------------------------------------- control microbenchmarks

def _cycle_i_body(extra):
    """repeat n { change i by 1; if i > 1000 { set i to 1 }; extra }."""
    return [
        S_change("i", num(1)),
        S_if(gt(var("i"), num(1000)), [S_set("i", num(1))]),
    ] + list(extra)


def gen_loop(n=50000000):
    """Control: loop + counter overhead only (`change i by 1`)."""
    g = Gen(["i", "t0", "t1", "check"], ["times"])
    g.define("bench %s", ["n"], [
        S_set("i", num(0)),
        S_repeat(arg("n"), [S_change("i", num(1))]),
    ], x=0, y=500)
    g.define("verify", [], [S_set("check", var("i"))], x=500, y=500)
    g.script(driver([], [S_call("bench %s", [num(1000)])],
                    S_call("bench %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_listread(n=50000000):
    """Control: loop + list read, index cycling over 1000 elements."""
    g = Gen(["i", "sum", "t0", "t1", "check"], ["L", "times"])
    g.define("fill", [], [
        S_delall("L"),
        S_repeat(num(1000), [S_add("L", num(7))]),
        S_set("sum", num(0)),
    ], x=0, y=300)
    g.define("bench %s", ["n"], [
        S_set("i", num(0)),
        S_repeat(arg("n"), _cycle_i_body([
            S_change("sum", item("L", var("i"))),
        ])),
    ], x=0, y=500)
    g.define("verify", [], [S_set("check", var("sum"))], x=500, y=500)
    g.script(driver([S_call("fill")], [S_call("bench %s", [num(1000)])],
                    S_call("bench %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_listwrite(n=50000000):
    """Control: loop + list replace, index cycling over 1000 elements."""
    g = Gen(["i", "k", "sum", "t0", "t1", "check"], ["L", "times"])
    g.define("fill", [], [
        S_delall("L"),
        S_repeat(num(1000), [S_add("L", num(0))]),
        S_set("k", num(0)),
    ], x=0, y=300)
    g.define("bench %s", ["n"], [
        S_set("i", num(0)),
        S_repeat(arg("n"), _cycle_i_body([
            S_change("k", num(1)),
            S_replace("L", var("i"), var("k")),
        ])),
    ], x=0, y=500)
    g.define("verify", [], [
        S_set("sum", num(0)),
        S_set("i", num(1)),
        S_repeat(num(1000), [
            S_change("sum", item("L", var("i"))),
            S_change("i", num(1)),
        ]),
        S_set("check", var("sum")),
    ], x=500, y=500)
    g.script(driver([S_call("fill")], [S_call("bench %s", [num(1000)])],
                    S_call("bench %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def gen_listappend(n=20000000):
    """Control: list growth (`add k to L`)."""
    g = Gen(["k", "t0", "t1", "check"], ["L", "times"])
    g.define("fill", [], [S_delall("L")], x=0, y=300)
    g.define("bench %s", ["n"], [
        S_set("k", num(0)),
        S_repeat(arg("n"), [
            S_change("k", num(1)),
            S_add("L", var("k")),
        ]),
    ], x=0, y=500)
    g.define("verify", [], [S_set("check", length("L"))], x=500, y=500)
    g.script(driver([S_call("fill")], [S_call("bench %s", [num(1000)])],
                    S_call("bench %s", [num(n)]),
                    S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g)


# ---------------------------------------------------------------- expected

def sha256_message(n):
    out = bytearray()
    s = 42
    for _ in range(n):
        s = (s * LCG_A + LCG_B) % LCG_M
        out.append(s % 256)
    return bytes(out)


def sha256_words(n):
    import hashlib
    d = hashlib.sha256(sha256_message(n)).hexdigest()
    return [int(d[i * 8:(i + 1) * 8], 16) for i in range(8)]


def expected_check(name, n):
    if name in ("quicksort", "mergesort", "bubblesort"):
        a = sorted(lcg(n))
        total = sum(a)
        h = 0
        for v in a:
            h = (h * 31 + v) % 1000000007
        ok = 1
        for x, y in zip(a, a[1:]):
            if x > y:
                ok = 0
        return "%d:%d:%d" % (total, h, ok)
    if name == "sieve":
        fl = [1] * (n + 1)
        fl[0] = 0
        if n >= 1:
            fl[1] = 0
        i = 2
        while i * i <= n:
            if fl[i]:
                for j in range(i * i, n + 1, i):
                    fl[j] = 0
            i += 1
        count = sum(fl)
        h = 0
        for k in range(1, n + 1):
            if fl[k]:
                h = (h * 31 + k) % 1000000007
        return "%d:%d:1" % (count, h)
    if name == "micro_loop":
        return str(n)
    if name == "micro_read":
        return str(7 * n)
    if name == "micro_write":
        return str(1000 * n - 499500)
    if name == "micro_append":
        return str(n)
    if name == "sha256":
        return ":".join(str(h) for h in sha256_words(n))
    if name == "fib":
        a, b = 0, 1
        for _ in range(n):
            a, b = b, a + b
        return "%d:1" % a
    raise ValueError(name)


SPECS = {
    "quicksort": (gen_quicksort, 200000),
    "mergesort": (gen_mergesort, 200000),
    "bubblesort": (gen_bubblesort, 5000),
    "sieve": (gen_sieve, 2000000),
    "fib": (gen_fib, 31),
    "sha256": (gen_sha256, 65536),
    "micro_loop": (gen_loop, 50000000),
    "micro_read": (gen_listread, 50000000),
    "micro_write": (gen_listwrite, 50000000),
    "micro_append": (gen_listappend, 20000000),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", nargs="?", default="tests/bench/algos/out")
    ap.add_argument("--only", default="")
    ap.add_argument("--sizes", default="", help="e.g. quicksort=8000,fib=28")
    args = ap.parse_args()

    sizes = {k: v[1] for k, v in SPECS.items()}
    for kv in filter(None, args.sizes.split(",")):
        k, v = kv.split("=")
        if k not in sizes:
            raise SystemExit("unknown algorithm: " + k)
        sizes[k] = int(v)

    only = [s for s in filter(None, args.only.split(","))]
    names = only or list(SPECS)

    manifest = {}
    for name in names:
        gen, _ = SPECS[name]
        n = sizes[name]
        project = gen(n)
        path = os.path.join(args.outdir, "%s_%d.sb3" % (name, n))
        write_sb3(path, project)
        manifest[name] = {
            "file": os.path.basename(path),
            "n": n,
            "blocks": len(project["targets"][1]["blocks"]),
            "expected_check": expected_check(name, n),
        }
        print("wrote %-44s n=%-7d blocks=%d" %
              (path, n, manifest[name]["blocks"]))
    mpath = os.path.join(args.outdir, "manifest.json")
    os.makedirs(args.outdir, exist_ok=True)
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print("wrote", mpath)


if __name__ == "__main__":
    main()
