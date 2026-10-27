#!/usr/bin/env python3
"""Helpers for building probe batteries.

A battery is one .sb3 whose single script evaluates N probes in order.  Each
probe's expression is stored into its own sprite variable (``p0``, ``p1``, ...)
so a battery of hundreds of expressions can be compared in one run without the
space-separated list dump getting in the way.

A probe is ``(label, fn)`` (value probe, ``fn(p)`` returns an input) or
``(label, fn, "stack")`` (``fn(p)`` appends ordinary stack blocks and returns
nothing; use a following value probe to snapshot the observable state).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIFF = os.path.dirname(HERE)
sys.path.insert(0, DIFF)

import gen_repros  # noqa: E402

PROBE_DIR = os.path.join(HERE, "probes")


class Chain:
    """Builds a block chain inside a substack without touching the outer next."""

    def __init__(self, proj, parent):
        self.proj = proj
        self.parent = parent
        self.first = None
        self.last = None

    def push(self, opcode, inputs=None, fields=None):
        bid = self.proj._new(opcode, inputs or {}, fields or {},
                             parent=self.last or self.parent)
        if self.last is not None:
            self.proj.blocks[self.last]["next"] = bid
        else:
            self.first = bid
        self.last = bid
        return bid

    def set(self, name, vid, expr):
        return self.push("data_setvariableto", {"VALUE": expr},
                         {"VARIABLE": [name, vid]})

    def change(self, name, vid, expr):
        return self.push("data_changevariableby", {"VALUE": expr},
                         {"VARIABLE": [name, vid]})

    def add(self, list_name, list_id, item):
        return self.push("data_addtolist", {"ITEM": item},
                         {"LIST": [list_name, list_id]})

    def stop(self, option):
        return self.push("control_stop", {}, {"STOP_OPTION": [option, None]})

    def if_(self, cond, fn):
        b = self.push("control_if", {"CONDITION": cond})
        c = Chain(self.proj, b)
        fn(c)
        if c.first is not None:
            self.proj.blocks[b]["inputs"]["SUBSTACK"] = [2, c.first]
        return b

    def repeat(self, times, fn, opcode="control_repeat", inputs=None):
        if opcode == "control_repeat":
            b = self.push(opcode, {"TIMES": times})
        else:
            b = self.push(opcode, {"CONDITION": times})
        c = Chain(self.proj, b)
        fn(c)
        if c.first is not None:
            self.proj.blocks[b]["inputs"]["SUBSTACK"] = [2, c.first]
        return b

    def repeat_until(self, cond, fn):
        return self.repeat(cond, fn, opcode="control_repeat_until")

    def while_(self, cond, fn):
        return self.repeat(cond, fn, opcode="control_while")

    def for_each(self, list_name, list_id, var_name, var_id, fn):
        b = self.push("control_for_each",
                      {"VARIABLE": [1, [12, var_name, var_id]],
                       "VALUE": [1, [13, list_name, list_id]]})
        c = Chain(self.proj, b)
        fn(c)
        if c.first is not None:
            self.proj.blocks[b]["inputs"]["SUBSTACK"] = [2, c.first]
        return b


class Battery(gen_repros.Project):
    """Project with extra block constructors and a probe API."""

    # ---- literal inputs ------------------------------------------------
    @staticmethod
    def nlit(v):
        return [1, [4, str(v)]]

    @staticmethod
    def slit(s):
        return [1, [10, s]]

    # ---- expression constructors --------------------------------------
    def _rep(self, opcode, inputs=None, fields=None):
        return [1, self._new(opcode, inputs or {}, fields or {})]

    def b(self, opcode, a, b):
        return self._rep(opcode, {"NUM1": a, "NUM2": b})

    def add(self, a, b):
        return self.b("operator_add", a, b)

    def sub(self, a, b):
        return self.b("operator_subtract", a, b)

    def mul(self, a, b):
        return self.b("operator_multiply", a, b)

    def div(self, a, b):
        return self.b("operator_divide", a, b)

    def mod(self, a, b):
        return self.b("operator_mod", a, b)

    def eq(self, a, b):
        return self._rep("operator_equals", {"OPERAND1": a, "OPERAND2": b})

    def lt(self, a, b):
        return self._rep("operator_lt", {"OPERAND1": a, "OPERAND2": b})

    def gt(self, a, b):
        return self._rep("operator_gt", {"OPERAND1": a, "OPERAND2": b})

    def math(self, op, a):
        return self._rep("operator_mathop", {"NUM": a},
                         {"OPERATOR": [op, None]})

    def neg(self, a):
        return self._rep("operator_subtract", {"NUM1": self.nlit(0),
                                               "NUM2": a})

    def or_(self, a, b):
        return self._rep("operator_or", {"OPERAND1": a, "OPERAND2": b})

    def and_(self, a, b):
        return self._rep("operator_and", {"OPERAND1": a, "OPERAND2": b})

    def not_(self, a):
        return self._rep("operator_not", {"OPERAND": a})

    def rnd(self, a):
        return self._rep("operator_round", {"NUM": a})

    def join(self, a, b):
        return self._rep("operator_join", {"STRING1": a, "STRING2": b})

    def letter(self, idx, s):
        return self._rep("operator_letter_of", {"LETTER": idx, "STRING": s})

    def length(self, s):
        return self._rep("operator_length", {"STRING": s})

    def contains(self, hay, needle):
        return self._rep("operator_contains",
                         {"STRING1": hay, "STRING2": needle})

    # ---- lists ---------------------------------------------------------
    def item(self, list_name, list_id, index):
        return self._rep("data_itemoflist", {"INDEX": index},
                         {"LIST": [list_name, list_id]})

    def itemnum(self, list_name, list_id, value):
        return self._rep("data_itemnumoflist", {"ITEM": value},
                         {"LIST": [list_name, list_id]})

    def listlen(self, list_name, list_id):
        return self._rep("data_lengthoflist", {},
                         {"LIST": [list_name, list_id]})

    def listhas(self, list_name, list_id, value):
        return self._rep("data_listcontainsitem", {"ITEM": value},
                         {"LIST": [list_name, list_id]})

    def listcontents(self, list_name, list_id):
        return self._rep("data_listcontents", {}, {"LIST": [list_name, list_id]})

    def var(self, name, vid):
        return self._rep("data_variable", {}, {"VARIABLE": [name, vid]})

    def push(self, opcode, inputs=None, fields=None):
        return super().push(opcode, inputs or {}, fields or {})

    # ---- stack operations (for stateful probes) ------------------------
    def l_add(self, list_name, list_id, item):
        return self.push("data_addtolist", {"ITEM": item},
                         {"LIST": [list_name, list_id]})

    def l_delete(self, list_name, list_id, index):
        return self.push("data_deleteoflist", {"INDEX": index},
                         {"LIST": [list_name, list_id]})

    def l_insert(self, list_name, list_id, index, item):
        return self.push("data_insertatlist", {"INDEX": index, "ITEM": item},
                         {"LIST": [list_name, list_id]})

    def l_replace(self, list_name, list_id, index, item):
        return self.push("data_replaceitemoflist",
                         {"INDEX": index, "ITEM": item},
                         {"LIST": [list_name, list_id]})

    def set(self, name, vid, expr):
        return self.push("data_setvariableto", {"VALUE": expr},
                         {"VARIABLE": [name, vid]})

    def chain(self, fn):
        c = Chain(self, self._prev)
        fn(c)
        if c.first is not None:
            self.blocks[self._prev]["next"] = c.first
            self._prev = c.last
        return c

    def repeat(self, times, fn, opcode="control_repeat"):
        b = self.push(opcode, {"TIMES": times} if opcode == "control_repeat"
                      else {"CONDITION": times})
        c = Chain(self, b)
        fn(c)
        if c.first is not None:
            self.blocks[b]["inputs"]["SUBSTACK"] = [2, c.first]
        return b

    def for_each(self, list_name, list_id, var_name, var_id, fn):
        b = self.push("control_for_each",
                      {"VARIABLE": [1, [12, var_name, var_id]],
                       "VALUE": [1, [13, list_name, list_id]]})
        c = Chain(self, b)
        fn(c)
        if c.first is not None:
            self.blocks[b]["inputs"]["SUBSTACK"] = [2, c.first]
        return b

    def change(self, name, vid, expr):
        return self.push("data_changevariableby", {"VALUE": expr},
                         {"VARIABLE": [name, vid]})

    def write_as(self, filename):
        os.makedirs(PROBE_DIR, exist_ok=True)
        path = os.path.join(PROBE_DIR, filename)
        self.write(path)
        return path

    def write(self, path):
        """Like the base writer, but ASCII-escaped JSON so lone surrogates are
        representable (``"\\ud83d"`` is valid JSON, raw UTF-8 is not)."""
        import zipfile
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.json",
                       json.dumps(self.build(), ensure_ascii=True))
            z.writestr(gen_repros.SVG_MD5 + ".svg", gen_repros.SVG)


def build_battery(name, probes, lists=None, variables=None):
    """Build ``probes/<name>.sb3`` plus ``probes/<name>.json`` (labels)."""
    p = Battery()
    p.start()
    labels = []
    var_fields = {}
    for i, spec in enumerate(probes):
        label, fn = spec[0], spec[1]
        kind = spec[2] if len(spec) > 2 else "value"
        if kind == "stack":
            fn(p)
            labels.append({"i": len(labels), "label": label, "stack": True})
            continue
        vname = "p%d" % i
        vid = "vp%d" % i
        var_fields[vid] = [vname, 0]
        p.set(vname, vid, fn(p))
        labels.append({"i": i, "label": label, "var": vname})
    merged = dict(var_fields)
    merged.update(variables or {})
    p.variables = merged
    p.lists = lists or {}
    path = p.write_as(name + ".sb3")
    with open(os.path.join(PROBE_DIR, name + ".json"), "w") as f:
        json.dump(labels, f, ensure_ascii=False, indent=1)
    return path
