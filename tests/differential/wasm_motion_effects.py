#!/usr/bin/env python3
"""Commit-only motion effects in terminal hybrid regions.

`set x to` / `set y to` / `change x by` / `change y by` are applied by a
host call as the final operation of a region. No guard can run afterwards,
so a rollback never replays an already-applied move. The follow-up
`x position` / `y position` reporters stay interpreted and observe the
target state.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, num, op, var, text, make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def M(opcode, fields=None, **inputs):
    """Stack statement for motion/effects blocks (generator statement kind)."""
    return ("motion", opcode, inputs, fields or {})


def fixture(path: Path, kind: str) -> None:
    g = Gen(["r", "s", "dx"], [])
    if kind == "setx":
        g.script([S_set("r", num(0)),
                  M("motion_setx", X=num(10)),
                  S_set("r", op("motion_xposition"))])
    elif kind == "changex":
        g.script([M("motion_setx", X=num(10)),
                  M("motion_changexby", DX=num(5)),
                  S_set("r", op("motion_xposition"))])
    elif kind == "sety":
        g.script([M("motion_sety", Y=num(-7)),
                  S_set("r", op("motion_yposition"))])
    elif kind == "changey":
        g.script([M("motion_sety", Y=num(2)),
                  M("motion_changeyby", DY=num(3)),
                  S_set("r", op("motion_yposition"))])
    elif kind == "var_operand":
        g.script([S_set("dx", num(7)),
                  M("motion_changexby", DX=var("dx")),
                  S_set("r", op("motion_xposition"))])
    elif kind == "setsize":
        g.script([M("looks_setsizeto", SIZE=num(150)),
                  S_set("r", op("looks_size"))])
    elif kind == "changesize":
        g.script([M("looks_changesizeby", CHANGE=num(25)),
                  S_set("r", op("looks_size"))])
    elif kind == "nan_size":
        # 0/0 -> ToNumber maps NaN to 0 at the effect boundary, so a
        # relative change leaves the size unchanged (interpreter parity).
        g.script([M("looks_setsizeto", SIZE=num(100)),
                  M("looks_changesizeby", CHANGE=op("operator_divide",
                                                    NUM1=num(0), NUM2=num(0))),
                  S_set("r", op("looks_size"))])
    elif kind == "setvolume":
        g.script([M("sound_setvolumeto", VOLUME=num(50)),
                  S_set("r", op("sound_volume"))])
    elif kind == "changevolume":
        g.script([M("sound_setvolumeto", VOLUME=num(80)),
                  M("sound_changevolumeby", VOLUME=num(30)),
                  S_set("r", op("sound_volume"))])
    elif kind == "volume_nan":
        g.script([M("sound_setvolumeto", VOLUME=num(60)),
                  M("sound_changevolumeby", VOLUME=op("operator_divide",
                                                      NUM1=num(0),
                                                      NUM2=num(0))),
                  S_set("r", op("sound_volume"))])
    elif kind == "pen_ops":
        g.script([
            M("pen_clear"),
            M("pen_penDown"),
            M("pen_setPenSizeTo", SIZE=num(5)),
            M("pen_changePenSizeBy", SIZE=num(2)),
            M("pen_setPenColorParamTo",
              fields={"COLOR_PARAM": "color"}, VALUE=num(50)),
            M("pen_changePenColorParamBy",
              fields={"COLOR_PARAM": "transparency"}, VALUE=num(10)),
            M("pen_penUp"),
        ])
    elif kind == "direction_point":
        g.script([M("motion_pointindirection", DIRECTION=num(90)),
                  S_set("r", op("motion_direction"))])
    elif kind == "direction_turn":
        g.script([M("motion_pointindirection", DIRECTION=num(90)),
                  M("motion_turnright", DEGREES=num(30)),
                  S_set("r", op("motion_direction")),
                  M("motion_turnleft", DEGREES=num(45)),
                  S_set("s", op("motion_direction"))])
    elif kind == "pen_color":
        g.script([
            M("pen_setPenColorToColor",
              COLOR=op("colour_picker", fields={"COLOUR": "#FF0000"})),
        ])
    elif kind == "looks_ops":
        g.script([
            M("looks_seteffectto", fields={"EFFECT": "GHOST"}, VALUE=num(50)),
            M("looks_changeeffectby", fields={"EFFECT": "GHOST"},
              CHANGE=num(10)),
            M("looks_cleargraphiceffects"),
            M("looks_show"),
            M("looks_hide"),
            M("pen_stamp"),
        ])
    elif kind == "string_prefix":
        # A non-numeric write before the effect must keep the region out of
        # WASM; otherwise the post-effect numeric commit check would abort
        # and replay the move, doubling a relative change.
        g.script([S_set("r", op("operator_join",
                                STRING1=text("a"), STRING2=text("b"))),
                  M("motion_changexby", DX=num(5)),
                  S_set("s", op("motion_xposition"))])
    elif kind == "string_operand":
        # Non-numeric operand is not lowered into a commit-only effect.
        g.script([M("motion_setx", X=text("abc")),
                  S_set("r", op("motion_xposition"))])
    else:
        raise ValueError(kind)
    write_sb3(str(path), make_project(["r", "s", "dx"], [], g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    addon = str(args.addon.resolve())
    expected = {"setx": '{"dx": "0", "r": "10", "s": "0"}',
                "changex": '{"dx": "0", "r": "15", "s": "0"}',
                "sety": '{"dx": "0", "r": "-7", "s": "0"}',
                "changey": '{"dx": "0", "r": "5", "s": "0"}',
                "var_operand": '{"dx": "7", "r": "7", "s": "0"}',
                "setsize": '{"dx": "0", "r": "150", "s": "0"}',
                "changesize": '{"dx": "0", "r": "125", "s": "0"}',
                "nan_size": '{"dx": "0", "r": "100", "s": "0"}',
                "setvolume": '{"dx": "0", "r": "50", "s": "0"}',
                "changevolume": '{"dx": "0", "r": "100", "s": "0"}',
                "volume_nan": '{"dx": "0", "r": "60", "s": "0"}',
                "pen_ops": '{"dx": "0", "r": "0", "s": "0"}',
                "looks_ops": '{"dx": "0", "r": "0", "s": "0"}',
                "pen_color": '{"dx": "0", "r": "0", "s": "0"}',
                "direction_point": '{"dx": "0", "r": "90", "s": "0"}',
                "direction_turn": '{"dx": "0", "r": "120", "s": "75"}',
                "string_prefix": '{"dx": "0", "r": "ab", "s": "5"}',
                "string_operand": '{"dx": "0", "r": "0", "s": "0"}'}
    for kind, want in expected.items():
        sb3 = args.out / f"{kind}.sb3"
        fixture(sb3, kind)
        native = run_native(args.llvm, sb3, "hotspot")
        interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                        sb3, "off")
        builtin, blog = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                            sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "builtin",
                                     "XYO_WASM_EFFECT_TRACE": "1"})
        binaryen, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                     "XYO_BINARYEN_NODE_ADDON": addon,
                     "XYO_WASM_EFFECT_TRACE": "1"})
        if len({native, interp, builtin, binaryen}) != 1:
            raise AssertionError(
                f"{kind}: LLVM={native} interp={interp} "
                f"builtin={builtin} binaryen={binaryen}")
        if builtin != want:
            raise AssertionError(f"{kind}: expected {want}, got {builtin}")
        if kind == "pen_color" and "effect mode=74" not in blog:
            raise AssertionError(
                f"pen_color: tagged color effect not executed from WASM: "
                f"{blog[-1400:]}")
        if kind == "looks_ops" and blog.count("[wasm-effect] effect mode=") < 6:
            raise AssertionError(
                "looks_ops: expected 6 compiled looks/pen effects, got "
                f"{blog.count('[wasm-effect] effect mode=')}")
        if kind == "pen_ops" and blog.count("[wasm-effect] effect mode=") < 7:
            raise AssertionError(
                "pen_ops: expected 7 compiled pen effects, got "
                f"{blog.count('[wasm-effect] effect mode=')}")
        if kind not in ("string_operand", "string_prefix"):
            if "hybrid script=" not in blog:
                raise AssertionError(
                    f"{kind}: expected a hybrid terminal-effect plan: "
                    f"{blog[-1400:]}")
            if "[wasm-effect] effect mode=" not in blog:
                raise AssertionError(
                    f"{kind}: effect was not executed from WASM: "
                    f"{blog[-1400:]}")
        print(f"PASS {kind}: {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
