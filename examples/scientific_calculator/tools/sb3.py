"""Small Scratch 3 project builder used by the calculator generator.

Blocks are created bottom-up; every reporter input is attached to its parent
automatically, which keeps the JSON valid without hand-written ids.
"""
import hashlib
import json
import zipfile


class Model:
    def __init__(self):
        self.vars = {}
        self.lists = {}
        self.var_values = {}
        self.list_values = {}

    def var(self, name, value=0, vid=None):
        vid = vid or "v_" + name
        self.vars[name] = vid
        self.var_values[name] = value
        return vid

    def list(self, name, items=None, lid=None):
        lid = lid or "l_" + name
        self.lists[name] = lid
        self.list_values[name] = list(items or [])
        return lid

    def v(self, name):
        return self.vars[name]

    def l(self, name):
        return self.lists[name]


class Proc:
    def __init__(self, builder, name, args, warp=True):
        self.b = builder
        self.name = name
        self.args = list(args)
        self.arg_ids = ["a%d" % i for i in range(len(args))]
        self.warp = warp
        self.proccode = name + (" " + " ".join(["%s"] * len(args)) if args else "")
        self.defn = None
        self.proto = None
        self._define()

    def _define(self):
        b = self.b
        b.n += 1
        self.proto = "proto_%s_%d" % (self.name.replace(" ", "_"), b.n)
        b.blocks[self.proto] = {
            "opcode": "procedures_prototype",
            "next": None,
            "parent": None,
            "inputs": {},
            "fields": {},
            "shadow": True,
            "topLevel": False,
            "mutation": {
                "tagName": "mutation",
                "children": [],
                "proccode": self.proccode,
                "argumentids": json.dumps(self.arg_ids),
                "argumentnames": json.dumps(self.args),
                "argumentdefaults": json.dumps([""] * len(self.args)),
                "warp": "true" if self.warp else "false",
            },
        }
        for aid, aname in zip(self.arg_ids, self.args):
            sh = b.nid("argsh")
            b.blocks[sh] = {
                "opcode": "argument_reporter_string_number",
                "next": None,
                "parent": self.proto,
                "inputs": {},
                "fields": {"VALUE": [aname, None]},
                "shadow": True,
                "topLevel": False,
            }
            b.blocks[self.proto]["inputs"][aid] = [1, sh]
        self.defn = b.nid("def")
        b.blocks[self.defn] = {
            "opcode": "procedures_definition",
            "next": None,
            "parent": None,
            "inputs": {"custom_block": [1, self.proto]},
            "fields": {},
            "shadow": False,
            "topLevel": True,
            "x": 0,
            "y": 400 + len(b.procs) * 420,
        }
        b.blocks[self.proto]["parent"] = self.defn
        b.procs[self.name] = self

    def body(self, ids):
        if ids:
            self.b.stack(self.defn, ids)
            self.b.blocks[self.defn]["next"] = ids[0]
        return self.defn


class Builder:
    def __init__(self, model):
        self.m = model
        self.blocks = {}
        self.procs = {}
        self.n = 0

    # ---------------------------------------------------------------- ids
    def nid(self, p="b"):
        self.n += 1
        return "%s%d" % (p, self.n)

    def _add(self, opcode, inputs=None, fields=None, shadow=False,
             mutation=None, top=False, x=0, y=0):
        bid = self.nid()
        fixed = {}
        for k, v in (inputs or {}).items():
            if isinstance(v, bool):
                v = [1, [10, "true" if v else "false"]]
            elif isinstance(v, (int, float)):
                v = [1, [4, str(v)]]
            elif isinstance(v, str):
                if v in self.blocks:
                    v = [3, v, [10, ""]]
                else:
                    v = [1, [10, v]]
            fixed[k] = v
        inputs = fixed
        blk = {
            "opcode": opcode,
            "next": None,
            "parent": None,
            "inputs": inputs or {},
            "fields": fields or {},
            "shadow": shadow,
            "topLevel": top,
        }
        if top:
            blk["x"] = x
            blk["y"] = y
        if mutation:
            blk["mutation"] = mutation
        self.blocks[bid] = blk
        self._attach(bid, blk["inputs"])
        return bid

    def _attach(self, bid, inputs):
        for val in inputs.values():
            if not isinstance(val, list) or len(val) < 2:
                continue
            kind, target = val[0], val[1]
            if not isinstance(target, str):
                continue
            if kind in (1, 2, 3):
                self.blocks[target]["parent"] = bid

    # ------------------------------------------------------------ primitives
    @staticmethod
    def vnum(v):
        return [1, [4, str(v)]]

    @staticmethod
    def vtext(v):
        return [1, [10, str(v)]]

    def vvar(self, name):
        return [1, [12, name, self.m.v(name)]]

    @staticmethod
    def vblock(bid, default=None):
        return [3, bid, default if default is not None else [10, ""]]

    @staticmethod
    def vbool(bid):
        return [2, bid]

    @staticmethod
    def vsub(bid):
        return [2, bid]

    @staticmethod
    def vmenu(bid):
        return [1, bid]

    # ------------------------------------------------------------- reporters
    def var(self, name):
        return self._add("data_variable", fields={"VARIABLE": [name, self.m.v(name)]})

    def arg(self, name):
        return self._add("argument_reporter_string_number", fields={"VALUE": [name, None]})

    def op(self, opcode, inputs=None, fields=None):
        return self._add(opcode, inputs=inputs, fields=fields)

    def num(self, v):
        return self._add("math_number", fields={"NUM": [v, None]}, shadow=True)

    def txt(self, v):
        return self._add("text", fields={"TEXT": [v, None]}, shadow=True)

    def menu_costume(self, name):
        return self._add("looks_costume", fields={"COSTUME": [name, None]}, shadow=True)

    def menu_backdrop(self, name):
        return self._add("looks_backdrops", fields={"BACKDROP": [name, None]}, shadow=True)

    def menu_key(self, key):
        return self._add("sensing_keyoptions", fields={"KEY_OPTION": [key, None]}, shadow=True)

    # --------------------------------------------------------------- stacks
    def stack(self, parent, ids):
        flat = []
        def _flat(items):
            for it in items:
                if isinstance(it, list):
                    _flat(it)
                else:
                    flat.append(it)
        _flat(ids)
        ids = flat
        prev = None
        for i, bid in enumerate(ids):
            if i == 0:
                self.blocks[bid]["parent"] = parent
            else:
                self.blocks[bid]["parent"] = prev
                self.blocks[prev]["next"] = bid
            prev = bid
        return ids[0] if ids else None

    def top(self, ids, x=0, y=0):
        first = self.stack(None, ids)
        if first:
            self.blocks[first]["topLevel"] = True
            self.blocks[first]["x"] = x
            self.blocks[first]["y"] = y
        return first

    # ------------------------------------------------------------- variables
    def setv(self, name, value):
        return self._add("data_setvariableto",
                         inputs={"VALUE": value},
                         fields={"VARIABLE": [name, self.m.v(name)]})

    def changev(self, name, value):
        return self._add("data_changevariableby",
                         inputs={"VALUE": value},
                         fields={"VARIABLE": [name, self.m.v(name)]})

    def list_add(self, name, item):
        return self._add("data_addtolist", inputs={"ITEM": item},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_delete_all(self, name):
        return self._add("data_deletealloflist",
                         fields={"LIST": [name, self.m.l(name)]})

    def list_delete(self, name, index):
        return self._add("data_deleteoflist", inputs={"INDEX": index},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_insert(self, name, item, index):
        return self._add("data_insertatlist",
                         inputs={"ITEM": item, "INDEX": index},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_replace(self, name, index, item):
        return self._add("data_replaceitemoflist",
                         inputs={"INDEX": index, "ITEM": item},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_item(self, name, index):
        return self._add("data_itemoflist", inputs={"INDEX": index},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_itemnum(self, name, item):
        return self._add("data_itemnumoflist", inputs={"ITEM": item},
                         fields={"LIST": [name, self.m.l(name)]})

    def list_len(self, name):
        return self._add("data_lengthoflist",
                         fields={"LIST": [name, self.m.l(name)]})

    def list_contains(self, name, item):
        return self._add("data_listcontainsitem", inputs={"ITEM": item},
                         fields={"LIST": [name, self.m.l(name)]})

    # -------------------------------------------------------------- control
    def if_(self, cond, body):
        bid = self._add("control_if", inputs={"CONDITION": [2, cond]})
        if body:
            self.stack(bid, body)
            self.blocks[bid]["inputs"]["SUBSTACK"] = [2, body[0]]
        return bid

    def ifelse(self, cond, body, other):
        bid = self._add("control_if_else", inputs={"CONDITION": [2, cond]})
        if body:
            self.stack(bid, body)
            self.blocks[bid]["inputs"]["SUBSTACK"] = [2, body[0]]
        if other:
            self.stack(bid, other)
            self.blocks[bid]["inputs"]["SUBSTACK2"] = [2, other[0]]
        return bid

    def repeat(self, times, body):
        bid = self._add("control_repeat", inputs={"TIMES": times})
        if body:
            self.stack(bid, body)
            self.blocks[bid]["inputs"]["SUBSTACK"] = [2, body[0]]
        return bid

    def repeat_until(self, cond, body):
        bid = self._add("control_repeat_until", inputs={"CONDITION": [2, cond]})
        if body:
            self.stack(bid, body)
            self.blocks[bid]["inputs"]["SUBSTACK"] = [2, body[0]]
        return bid

    def forever(self, body):
        bid = self._add("control_forever")
        if body:
            self.stack(bid, body)
            self.blocks[bid]["inputs"]["SUBSTACK"] = [2, body[0]]
        return bid

    def wait(self, secs):
        return self._add("control_wait", inputs={"DURATION": secs})

    # ---------------------------------------------------------------- calls
    def call(self, proc, args=None, warp=None):
        args = args or []
        inputs = {}
        for aid, val in zip(proc.arg_ids, args):
            inputs[aid] = val
        mut = {
            "tagName": "mutation",
            "children": [],
            "proccode": proc.proccode,
            "argumentids": json.dumps(proc.arg_ids),
            "warp": "true" if (proc.warp if warp is None else warp) else "false",
        }
        return self._add("procedures_call", inputs=inputs, mutation=mut)

    def defproc(self, name, args=(), warp=True):
        return Proc(self, name, args, warp)

    # -------------------------------------------------------------- helpers
    def add(self, a, b):
        return self.op("operator_add", {"NUM1": a, "NUM2": b})

    def sub(self, a, b):
        return self.op("operator_subtract", {"NUM1": a, "NUM2": b})

    def mul(self, a, b):
        return self.op("operator_multiply", {"NUM1": a, "NUM2": b})

    def div(self, a, b):
        return self.op("operator_divide", {"NUM1": a, "NUM2": b})

    def mod(self, a, b):
        return self.op("operator_mod", {"NUM1": a, "NUM2": b})

    def join(self, a, b):
        return self.op("operator_join", {"STRING1": a, "STRING2": b})

    def letter(self, i, s):
        return self.op("operator_letter_of", {"LETTER": i, "STRING": s})

    def length(self, s):
        return self.op("operator_length", {"STRING": s})

    def eq(self, a, b):
        return self.op("operator_equals", {"OPERAND1": a, "OPERAND2": b})

    def gt(self, a, b):
        return self.op("operator_gt", {"OPERAND1": a, "OPERAND2": b})

    def lt(self, a, b):
        return self.op("operator_lt", {"OPERAND1": a, "OPERAND2": b})

    def and_(self, a, b):
        return self.op("operator_and", {"OPERAND1": [2, a], "OPERAND2": [2, b]})

    def or_(self, a, b):
        return self.op("operator_or", {"OPERAND1": [2, a], "OPERAND2": [2, b]})

    def not_(self, a):
        return self.op("operator_not", {"OPERAND": [2, a]})

    def contains(self, a, b):
        return self.op("operator_contains", {"STRING1": a, "STRING2": b})

    def mathop(self, op, a):
        return self.op("operator_mathop", {"NUM": a}, {"OPERATOR": [op, None]})

    def round_(self, a):
        return self.op("operator_round", {"NUM": a})

    def random(self, a, b):
        return self.op("operator_random", {"FROM": a, "TO": b})

    def eqv(self, name, value):
        return self.eq(self.vvar(name), self.vtext(value))

    def eqn(self, name, value):
        return self.eq(self.vvar(name), self.vnum(value))

    # --------------------------------------------------------------- output
    def stage_json(self, costumes, blocks=None):
        return {
            "isStage": True,
            "name": "Stage",
            "variables": {self.m.v(n): [n, self.m.var_values[n]] for n in self.m.vars},
            "lists": {self.m.l(n): [n, self.m.list_values[n]] for n in self.m.lists},
            "broadcasts": {},
            "blocks": blocks or {},
            "comments": {},
            "costumes": costumes,
            "sounds": [],
            "currentCostume": 0,
            "volume": 100,
            "layerOrder": 0,
            "tempo": 60,
            "videoTransparency": 50,
            "videoState": "on",
            "textToSpeechLanguage": "",
        }

    def sprite_json(self, name, costumes, blocks, layer, x=0, y=0,
                    visible=True, size=100, direction=90,
                    rotation_style="all around", variables=None, lists=None):
        return {
            "isStage": False,
            "name": name,
            "variables": variables or {},
            "lists": lists or {},
            "broadcasts": {},
            "blocks": blocks,
            "comments": {},
            "costumes": costumes,
            "sounds": [],
            "currentCostume": 0,
            "volume": 100,
            "layerOrder": layer,
            "visible": visible,
            "x": x,
            "y": y,
            "size": size,
            "direction": direction,
            "draggable": False,
            "rotationStyle": rotation_style,
        }


def md5(data):
    return hashlib.md5(data.encode("utf-8")).hexdigest()


def write_sb3(path, project, assets):
    """assets: dict filename-without-ext -> text content; ids become md5."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project, ensure_ascii=False,
                                             separators=(",", ":")))
        for _name, text in assets.items():
            h = md5(text)
            z.writestr(h + ".svg", text)
    return path


def costume(name, text, cx, cy):
    h = md5(text)
    return {
        "name": name,
        "dataFormat": "svg",
        "bitmapResolution": 1,
        "assetId": h,
        "md5ext": h + ".svg",
        "rotationCenterX": cx,
        "rotationCenterY": cy,
    }
