#!/usr/bin/env python3
# scratch-blocks の SVG アイコンを RGBA8 C 配列へ埋め込み用に変換する。
# 使い方: python3 tools/embed_icons.py > src/blockly/icons_rgba.inc
# 依存: rsvg-convert (librsvg) + ImageMagick (rgba 変換)。
import subprocess, tempfile, os

ICONS = [
    # (name, svg, grayscale, [w, h])
    ("flag",      "refs/scratch-vm/node_modules/scratch-blocks/media/green-flag.svg", False),
    ("repeat",    "refs/scratch-vm/node_modules/scratch-blocks/media/repeat.svg", False),
    ("turnright", "refs/scratch-vm/node_modules/scratch-blocks/media/rotate-right.svg", False),
    ("turnleft",  "refs/scratch-vm/node_modules/scratch-blocks/media/rotate-left.svg", False),
    ("pen",       "refs/scratch-vm/node_modules/scratch-blocks/media/extensions/pen-block-icon.svg", False),
    ("music",     "refs/scratch-vm/node_modules/scratch-blocks/media/extensions/music-block-icon.svg", False),
    # scratch-gui のタブアイコン（Code/Costumes/Sounds、グレースケール）
    ("tab_code",     "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--code.svg", True),
    ("tab_costumes", "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--costumes.svg", True),
    ("tab_sounds",   "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--sounds.svg", True),
    # 選択タブはカラーの同じ SVG（.is-selected img でグレースケール解除）
    ("tab_code_c",     "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--code.svg", False),
    ("tab_costumes_c", "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--costumes.svg", False),
    ("tab_sounds_c",   "refs/scratch-editor/packages/scratch-gui/src/components/gui/icon--sounds.svg", False),
    # メニューバーの Scratch ロゴ（白）と言語地球アイコン
    ("logo",  "refs/scratch-editor/packages/scratch-gui/src/components/menu-bar/scratch-logo.svg", False, 160, 44),
    ("globe", "refs/scratch-editor/packages/scratch-gui/src/components/language-selector/language-icon.svg", False, 48, 48),
    # ステージ操作系（緑旗・ストップ・ステージサイズ切替）
    ("gf_button", "refs/scratch-editor/packages/scratch-gui/src/components/green-flag/icon--green-flag.svg", False, 32, 32),
    ("stop_button","refs/scratch-editor/packages/scratch-gui/src/components/stop-all/icon--stop-all.svg", False, 32, 32),
    ("stage_small","refs/scratch-editor/packages/scratch-gui/src/components/stage-header/icon--small-stage.svg", False, 32, 32),
    ("stage_large","refs/scratch-editor/packages/scratch-gui/src/components/stage-header/icon--large-stage.svg", False, 32, 32),
    ("fullscreen","refs/scratch-editor/packages/scratch-gui/src/components/stage-header/icon--fullscreen.svg", False, 32, 32),
    ("unfullscreen","refs/scratch-editor/packages/scratch-gui/src/components/stage-header/icon--unfullscreen.svg", False, 32, 32),
    # スプライト追加・背景追加
    ("addsprite", "refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--sprite.svg", False),
    ("addbackdrop","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--backdrop.svg", False),
    # スプライト情報パネル（x, y, show, hide, 回転スタイル）
    ("spr_x",     "refs/scratch-editor/packages/scratch-gui/src/components/sprite-info/icon--x.svg", False),
    ("spr_y",     "refs/scratch-editor/packages/scratch-gui/src/components/sprite-info/icon--y.svg", False),
    ("spr_show",  "refs/scratch-editor/packages/scratch-gui/src/components/sprite-info/icon--show.svg", False),
    ("spr_hide",  "refs/scratch-editor/packages/scratch-gui/src/components/sprite-info/icon--hide.svg", False),
    ("rot_all",   "refs/scratch-editor/packages/scratch-gui/src/components/direction-picker/icon--all-around.svg", False),
    ("rot_lr",    "refs/scratch-editor/packages/scratch-gui/src/components/direction-picker/icon--left-right.svg", False),
    ("rot_no",    "refs/scratch-editor/packages/scratch-gui/src/components/direction-picker/icon--dont-rotate.svg", False),
    ("dial_face", "refs/scratch-editor/packages/scratch-gui/src/components/direction-picker/icon--dial.svg", False, 112, 112),
    ("dial_handle","refs/scratch-editor/packages/scratch-gui/src/components/direction-picker/icon--handle.svg", False, 40, 40),
    # メニューバー（gear=設定、file/edit 項目アイコン、ドロップダウン矢印）
    ("gear",    "refs/scratch-editor/packages/scratch-gui/src/components/menu-bar/icon--settings.svg", False),
    ("menu_file","refs/scratch-editor/packages/scratch-gui/src/components/menu-bar/icon--file.svg", False),
    ("menu_edit","refs/scratch-editor/packages/scratch-gui/src/components/menu-bar/icon--edit.svg", False),
    ("caret",   "refs/scratch-editor/packages/scratch-gui/src/components/menu-bar/dropdown-caret.svg", False),
    # メニューバー右側の Tutorials / Debug
    ("tutorials","refs/scratch-editor/packages/scratch-gui/src/lib/assets/icon--tutorials.svg", False),
    ("debug",   "refs/scratch-editor/packages/scratch-gui/src/components/debug-modal/icons/icon--debug.svg", False),
    # ワークスペース（ズーム/ゴミ箱/削除バッジ）
    ("zoom_in",  "refs/scratch-vm/node_modules/scratch-blocks/media/zoom-in.svg", False),
    ("zoom_out", "refs/scratch-vm/node_modules/scratch-blocks/media/zoom-out.svg", False),
    ("zoom_reset","refs/scratch-vm/node_modules/scratch-blocks/media/zoom-reset.svg", False),
    ("sprites_ws","refs/scratch-vm/node_modules/scratch-blocks/media/sprites.svg", False),
    ("delete_x", "refs/scratch-vm/node_modules/scratch-blocks/media/delete-x.svg", False),
    # サウンドエディタ（icon--sound / undo / redo / effects）
    ("snd_icon", "refs/scratch-editor/packages/scratch-gui/src/components/asset-panel/icon--sound.svg", False),
    ("snd_undo", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--undo.svg", False),
    ("snd_redo", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--redo.svg", False),
    ("snd_play", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--play.svg", False),
    ("snd_stop", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--stop.svg", False),
    ("snd_trim", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--trim.svg", False),
    ("fx_faster", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--faster.svg", False),
    ("fx_slower", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--slower.svg", False),
    ("fx_louder", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--louder.svg", False),
    ("fx_softer", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--softer.svg", False),
    ("fx_mute",   "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--mute.svg", False),
    ("fx_robot",  "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--robot.svg", False),
    ("fx_echo",   "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--echo.svg", False),
    ("fx_reverse","refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--reverse.svg", False),
    ("fx_fadein", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--fade-in.svg", False),
    ("fx_fadeout","refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--fade-out.svg", False),
    ("snd_copy",  "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--copy.svg", False),
    ("snd_copynew","refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--copy-to-new.svg", False),
    ("snd_paste", "refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--paste.svg", False),
    ("snd_delete","refs/scratch-editor/packages/scratch-gui/src/components/sound-editor/icon--delete.svg", False),
    ("turbo_icon","refs/scratch-editor/packages/scratch-gui/src/components/turbo-mode/icon--turbo.svg", False),
    # ペイントエディタ（scratch-paint）
    ("pe_bitmap","refs/scratch-editor/packages/scratch-paint/src/components/paint-editor/icons/bitmap.svg", False),
    ("pe_zoom_in","refs/scratch-editor/packages/scratch-paint/src/components/paint-editor/icons/zoom-in.svg", False),
    ("pe_zoom_out","refs/scratch-editor/packages/scratch-paint/src/components/paint-editor/icons/zoom-out.svg", False),
    ("pe_zoom_reset","refs/scratch-editor/packages/scratch-paint/src/components/paint-editor/icons/zoom-reset.svg", False),
    ("pe_undo","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/undo.svg", False),
    ("pe_redo","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/redo.svg", False),
    ("pe_group","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/group.svg", False),
    ("pe_ungroup","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/ungroup.svg", False),
    ("pe_send_forward","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/send-forward.svg", False),
    ("pe_send_backward","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/send-backward.svg", False),
    ("pe_send_front","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/send-front.svg", False),
    ("pe_send_back","refs/scratch-editor/packages/scratch-paint/src/components/fixed-tools/icons/send-back.svg", False),
    ("pe_flip_h","refs/scratch-editor/packages/scratch-paint/src/components/mode-tools/icons/flip-horizontal.svg", False),
    ("pe_flip_v","refs/scratch-editor/packages/scratch-paint/src/components/mode-tools/icons/flip-vertical.svg", False),
    ("pe_copy","refs/scratch-editor/packages/scratch-paint/src/components/mode-tools/icons/copy.svg", False),
    ("pe_paste","refs/scratch-editor/packages/scratch-paint/src/components/mode-tools/icons/paste.svg", False),
    ("pe_delete","refs/scratch-editor/packages/scratch-paint/src/components/mode-tools/icons/delete.svg", False),
    ("tool_select","refs/scratch-editor/packages/scratch-paint/src/components/select-mode/select.svg", False),
    ("tool_reshape","refs/scratch-editor/packages/scratch-paint/src/components/reshape-mode/reshape.svg", False),
    ("tool_brush","refs/scratch-editor/packages/scratch-paint/src/components/brush-mode/brush.svg", False),
    ("tool_eraser","refs/scratch-editor/packages/scratch-paint/src/components/eraser-mode/eraser.svg", False),
    ("tool_fill","refs/scratch-editor/packages/scratch-paint/src/components/fill-mode/fill.svg", False),
    ("tool_text","refs/scratch-editor/packages/scratch-paint/src/components/text-mode/text.svg", False),
    ("tool_line","refs/scratch-editor/packages/scratch-paint/src/components/line-mode/line.svg", False),
    ("tool_oval","refs/scratch-editor/packages/scratch-paint/src/components/oval-mode/oval.svg", False),
    ("tool_rect","refs/scratch-editor/packages/scratch-paint/src/components/rect-mode/rectangle.svg", False),
    ("tool_eyedrop","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/eye-dropper.svg", False),
    ("am_search","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--search.svg", False),
    ("am_paint","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--paint.svg", False),
    ("am_surprise","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--surprise.svg", False),
    ("am_upload","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--file-upload.svg", False),
    ("am_sound","refs/scratch-editor/packages/scratch-gui/src/components/asset-panel/icon--add-sound-lib.svg", False),
    ("am_costume","refs/scratch-editor/packages/scratch-gui/src/components/asset-panel/icon--add-costume-lib.svg", False),
    ("am_backdrop","refs/scratch-editor/packages/scratch-gui/src/components/asset-panel/icon--add-backdrop-lib.svg", False),
    ("am_record","refs/scratch-editor/packages/scratch-gui/src/components/asset-panel/icon--add-sound-record.svg", False),
    ("am_sprite","refs/scratch-editor/packages/scratch-gui/src/components/action-menu/icon--sprite.svg", False),
    ("cp_solid","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/fill-solid-enabled.svg", False),
    ("cp_horz","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/fill-horz-gradient-enabled.svg", False),
    ("cp_vert","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/fill-vert-gradient-enabled.svg", False),
    ("cp_radial","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/fill-radial-enabled.svg", False),
    ("cp_swap","refs/scratch-editor/packages/scratch-paint/src/components/color-picker/icons/swap.svg", False),
    ("cp_nofill","refs/scratch-editor/packages/scratch-paint/src/components/color-button/no-fill.svg", False),
]
W = H = 48

lines = [
    "// generated by tools/embed_icons.py — scratch-blocks SVG icons (RGBA8)",
    "// 名前空間は持たない — 呼出し側の無名名前空間内で #include する。",
    "#pragma once",
    "struct IconPx { const char* name; const unsigned char* px; int w, h; };",
]
entries = []
tmp = tempfile.mkdtemp()
for item in ICONS:
    name, path, gray = item[0], item[1], item[2]
    w, h = (item[3], item[4]) if len(item) > 3 else (W, H)
    png = os.path.join(tmp, f"ic_{name}.png")
    rgba = os.path.join(tmp, f"ic_{name}.rgba")
    subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h), path,
                    "-o", png], check=True)
    cmd = ["convert", png, "-alpha", "on"]
    if gray:
        cmd += ["-colorspace", "Gray"]
    cmd += ["-depth", "8", rgba]
    subprocess.run(cmd, check=True)
    data = open(rgba, "rb").read()
    assert len(data) == w * h * 4, (name, len(data))
    sym = f"kIcon_{name}"
    lines.append(f"static const unsigned char {sym}[{len(data)}] = {{")
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    entries.append(f'  {{ "{name}", {sym}, {w}, {h} }},')
lines.append("static const IconPx kIconPxs[] = {")
lines += entries
lines.append("};")
print("\n".join(lines))
