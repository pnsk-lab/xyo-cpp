#!/usr/bin/env node
// gen_block_catalog.mjs — scratch-blocks のブロック定義（形状・ラベル・入力・色）と
// scratch-gui のツールボックス（パレット構成・shadow 既定値）から
// src/blockly/catalog.hpp を生成する。
//
// 参照元:
//   refs/scratch-vm/node_modules/scratch-blocks/blocks_vertical/*.js
//   refs/scratch-vm/node_modules/scratch-blocks/blocks_common/*.js
//   refs/scratch-vm/node_modules/scratch-l10n/editor/blocks/en.json
//   refs/scratch-editor/packages/scratch-gui/src/lib/make-toolbox-xml.js
//   refs/scratch-editor/packages/scratch-gui/src/lib/settings/color-mode/default/index.js

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BLOCKS_ROOT = path.join(ROOT, 'refs/scratch-vm/node_modules/scratch-blocks');
const L10N = path.join(ROOT, 'refs/scratch-vm/node_modules/scratch-l10n/editor/blocks/en.json');
const TOOLBOX = path.join(ROOT, 'refs/scratch-editor/packages/scratch-gui/src/lib/make-toolbox-xml.js');
const COLORFILE = path.join(ROOT, 'refs/scratch-editor/packages/scratch-gui/src/lib/settings/color-mode/default/index.js');
const OUT = path.join(ROOT, 'src/blockly/catalog.hpp');

// 実行時に値を生成するブロック（パレット表現に不向き）は除外
const SKIP = new Set(['colour_picker']);

const labels = JSON.parse(fs.readFileSync(L10N, 'utf8'));

// ---------------------------------------------------------------------------
// 色（default/index.js の blockColors から category -> hex を取り出す）
// ---------------------------------------------------------------------------
function parseColors() {
    const src = fs.readFileSync(COLORFILE, 'utf8');
    const colors = {};
    const re = /(\w+):\s*\{\s*colourPrimary:\s*'#([0-9A-Fa-f]{6})',\s*colourSecondary:\s*'#([0-9A-Fa-f]{6})',\s*colourTertiary:\s*'#([0-9A-Fa-f]{6})'/g;
    let m;
    while ((m = re.exec(src))) {
        colors[m[1]] = {
            primary: m[2],
            secondary: m[3],
            tertiary: m[4]
        };
    }
    // scratch-blocks のカテゴリ名は data_lists を "data_lists" として持つ
    return colors;
}
const COLORS = parseColors();

function hexToU32(hex) {
    return parseInt(hex, 16);
}
function colorFor(category) {
    const c = COLORS[category];
    if (c) return c;
    // fallbacks
    if (category === 'more') return { primary: 'FF6680', secondary: 'FF4D6A', tertiary: 'FF3355' };
    if (category === 'textField') return { primary: 'FFFFFF', secondary: 'FFFFFF', tertiary: 'FFFFFF' };
    return { primary: '808080', secondary: '6E6E6E', tertiary: '5C5C5C' };
}

// ---------------------------------------------------------------------------
// ブロック定義のパース（jsonInit を抽出）
// ---------------------------------------------------------------------------
function stripComments(s) {
    s = s.replace(/\/\*[\s\S]*?\*\//g, '');
    s = s.replace(/\/\/[^\n]*/g, '');
    return s;
}

function extractBalanced(src, startIdx) {
    // startIdx は '{' の位置。対応する '}' まで返す（文字列を尊重）。
    let depth = 0;
    let inStr = null;
    let i = startIdx;
    for (; i < src.length; i++) {
        const ch = src[i];
        if (inStr) {
            if (ch === '\\') { i++; continue; }
            if (ch === inStr) inStr = null;
            continue;
        }
        if (ch === '"' || ch === "'") { inStr = ch; continue; }
        if (ch === '{') depth++;
        else if (ch === '}') { depth--; if (depth === 0) return src.slice(startIdx, i + 1); }
    }
    return null;
}

// Blockly.Categories の値 → 色キー（blockColors のキー）
const CATEGORY_TO_COLOUR = {
    motion: 'motion',
    looks: 'looks',
    sound: 'sounds',
    pen: 'pen',
    data: 'data',
    dataLists: 'data_lists',
    event: 'event',
    control: 'control',
    sensing: 'sensing',
    operators: 'operators',
    more: 'more'
};

function normalizeJson(body) {
    // 1) 単一引用符の文字列を二重引用符へ（エスケープ \' 等を正しく処理）
    body = body.replace(/'((?:[^'\\]|\\.)*)'/g, (m, inner) => {
        const unescaped = inner.replace(/\\(['"\\])/g, '$1');
        return JSON.stringify(unescaped);
    });
    // 2) Blockly.Msg.X -> ラベル、Blockly.Categories.X -> 色キー
    body = body.replace(/Blockly\.Msg\.([A-Za-z0-9_]+)/g, (m, k) => {
        const v = labels[k] !== undefined ? labels[k] : k;
        return JSON.stringify(v);
    });
    body = body.replace(/Blockly\.Categories\.([A-Za-z0-9_]+)/g,
        (m, k) => JSON.stringify(CATEGORY_TO_COLOUR[k] || k));
    // 3) Blockly.Colours.<cat>.<shade> / textField -> hex 文字列
    body = body.replace(/Blockly\.Colours\.(\w+)\.(primary|secondary|tertiary)/g, (m, cat, shade) => {
        const c = colorFor(cat);
        return JSON.stringify('#' + c[shade]);
    });
    body = body.replace(/Blockly\.Colours\.textField\b/g, '"#FFFFFF"');
    // 4) field_image の src（media パス連結）→ ファイル名だけ
    body = body.replace(/Blockly\.mainWorkspace\.options\.pathToMedia\s*\+\s*"([^"]*)"/g, '"$1"');
    // 5) 型定数
    body = body.replace(/Blockly\.LIST_VARIABLE_TYPE/g, '"list"');
    body = body.replace(/Blockly\.BROADCAST_MESSAGE_VARIABLE_TYPE/g, '"broadcast_msg"');
    body = body.replace(/Blockly\.VARIABLE_TYPE/g, '"variable"');
    // 6) 出力形状
    body = body.replace(/Blockly\.OUTPUT_SHAPE_ROUND/g, '"round"');
    body = body.replace(/Blockly\.OUTPUT_SHAPE_HEXAGONAL/g, '"hex"');
    // 7) 末尾カンマ除去
    body = body.replace(/,\s*([}\]])/g, '$1');
    return body;
}

function parseBlocks() {
    const dirs = [
        path.join(BLOCKS_ROOT, 'blocks_vertical'),
        path.join(BLOCKS_ROOT, 'blocks_common')
    ];
    const files = [];
    for (const d of dirs) {
        for (const f of fs.readdirSync(d)) {
            if (f.endsWith('.js')) files.push(path.join(d, f));
        }
    }
    const defs = new Map();
    const errors = [];
    for (const file of files) {
        const src = fs.readFileSync(file, 'utf8');
        const starts = [];
        const re = /Blockly\.Blocks\['([A-Za-z0-9_]+)'\]\s*=\s*\{/g;
        let m;
        while ((m = re.exec(src))) starts.push({ opcode: m[1], index: m.index });
        for (let i = 0; i < starts.length; i++) {
            const { opcode, index } = starts[i];
            const end = i + 1 < starts.length ? starts[i + 1].index : src.length;
            const seg = src.slice(index, end);
            const ji = seg.indexOf('this.jsonInit(');
            if (ji < 0) continue;  // 動的 init（control_stop 等）はスキップ
            const braceIdx = seg.indexOf('{', ji);
            if (braceIdx < 0) continue;
            const body = extractBalanced(seg, braceIdx);
            if (!body) { errors.push(opcode + ': unbalanced'); continue; }
            // 動的 options / colour など function を含むブロックはスキップ
            if (/function\s*\(/.test(body)) continue;
            if (SKIP.has(opcode)) continue;
            const clean = stripComments(body);
            // 色キーを raw body から検出（colours_* extension か Blockly.Colours.* 直接指定）
            let colourKey = '';
            const cm = clean.match(/Blockly\.Colours\.(\w+)\./);
            const em = clean.match(/colours_(\w+)/);
            if (cm) colourKey = cm[1];
            else if (em) colourKey = em[1];
            const normalized = normalizeJson(clean);
            let json;
            try {
                json = JSON.parse(normalized);
            } catch (e) {
                errors.push(opcode + ': JSON parse failed: ' + e.message);
                continue;
            }
            json._colourKey = colourKey;
            defs.set(opcode, json);
        }
    }
    // control_stop は動的 init のため手動で追加
    defs.set('control_stop', {
        message0: labels.CONTROL_STOP || 'stop',
        args0: [{
            type: 'field_dropdown',
            name: 'STOP_OPTION',
            options: [
                [labels.CONTROL_STOP_ALL || 'all', 'all'],
                [labels.CONTROL_STOP_THIS || 'this script', 'this script'],
                [labels.CONTROL_STOP_OTHER || 'other scripts in sprite', 'other scripts in sprite']
            ]
        }],
        extensions: ['colours_control', 'shape_statement'],
        _colourKey: 'control'
    });
    return { defs, errors };
}

// ---------------------------------------------------------------------------
// 形状の判定
// ---------------------------------------------------------------------------
function classify(def) {
    const ext = def.extensions || [];
    const hasSubstack2 = (def.args0 || []).some(a => a.name === 'SUBSTACK2' && a.type === 'input_statement');
    const hasSubstack = (def.args0 || []).some(a => a.type === 'input_statement');
    let shape = 'Stack';
    if (ext.includes('shape_hat')) shape = 'Hat';
    else if (ext.includes('shape_end')) shape = 'End';
    else if (ext.includes('output_boolean')) shape = 'Boolean';
    else if (ext.includes('output_number') || ext.includes('output_string')) shape = 'Reporter';
    // 出力形状の直接指定（outputShape）
    if (def.outputShape === 'hex') shape = 'Boolean';
    else if (def.outputShape === 'round') shape = 'Reporter';
    // C-block 判定（statement input を持つ = Loop / IfElse）
    if (hasSubstack2) return { shape, cblock: 2 };
    if (hasSubstack) return { shape, cblock: 1 };
    return { shape, cblock: 0 };
}

function categoryOf(def) {
    if (def._colourKey) return def._colourKey;
    const ext = def.extensions || [];
    for (const e of ext) {
        if (e.startsWith('colours_')) return e.slice('colours_'.length);
    }
    return def.category || 'more';
}

const KIND = {
    'input_value': 'Value',
    'input_statement': 'Statement',
    'field_number': 'Number',
    'field_angle': 'Angle',
    'field_dropdown': 'Dropdown',
    'field_variable': 'Variable',
    'field_variable_getter': 'Variable',
    'field_boolean': 'BooleanField',
    'field_image': 'Image',
    'field_label_serializable': 'Label'
};

function argDef(arg) {
    const kind = KIND[arg.type] || 'Value';
    const a = { kind, name: arg.name || '', dflt: '', options: [], shadow: '', shadowField: '', shadowDefault: '' };
    if (kind === 'Number' || kind === 'Angle') {
        if (arg.value !== undefined) a.dflt = String(arg.value);
        else a.dflt = '0';
    }
    if (kind === 'Dropdown') {
        if (Array.isArray(arg.options)) {
            for (const opt of arg.options) {
                if (Array.isArray(opt) && opt.length >= 2) {
                    a.options.push(String(opt[0]), String(opt[1]));
                }
            }
            if (a.options.length >= 2) a.dflt = a.options[1]; // 既定 = 先頭項目の value
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// ツールボックス（パレット構成 + value input の shadow 既定値）
// ---------------------------------------------------------------------------
function evalToolbox() {
    let src = fs.readFileSync(TOOLBOX, 'utf8');
    src = src.replace(/import \* as ScratchBlocks from 'scratch-blocks';\n?/, '');
    src = src.replace(/import \{defaultColors\} from '\.\/settings\/color-mode';\n?/, '');
    src = src.replace(/export default makeToolboxXML;\n?/, 'return makeToolboxXML;');
    const ScratchBlocks = {
        ScratchMsgs: { translate: (k, d) => (labels[k] !== undefined ? labels[k] : d) }
    };
    const blockColors = {};
    for (const [cat, c] of Object.entries(COLORS)) {
        blockColors[cat] = {
            colourPrimary: '#' + c.primary,
            colourSecondary: '#' + c.secondary,
            colourTertiary: '#' + c.tertiary,
            colourQuaternary: '#' + c.tertiary
        };
    }
    // defaultColors は blockColors（category -> {colourPrimary,...}）そのもの。
    const defaultColors = blockColors;
    const fn = new Function('ScratchBlocks', 'defaultColors', src)(ScratchBlocks, defaultColors);
    // isInitialSetup=false, isStage=false（スプライト用のパレットを得る）
    return fn(false, false, 'target', [], '', '', '', defaultColors);
}

function parseToolbox(xml) {
    // <category name="..." toolboxitemid="..." ...> ... </category>
    const categories = [];
    const catRe = /<category\s+name="([^"]*)"\s+toolboxitemid="([^"]*)"([^>]*)>/g;
    const shadowDefaults = new Map(); // opcode -> {inputName: {shadow, field, value}}
    let m;
    const blocksByCat = new Map(); // catId -> [opcodes in order]
    let currentCat = null;
    // トークン列を走査してカテゴリ・ブロック・sep を順に拾う
    const tokenRe = /<category\s+name="[^"]*"\s+toolboxitemid="([^"]*)"[^>]*>|<\/category>|<sep\s+gap="(\d+)"\/>|<block\s+type="([A-Za-z0-9_]+)"([^>]*?)(\/)?>|<value\s+name="([^"]+)">|<shadow[^>]*type="([A-Za-z0-9_]+)"[^>]*?(\/)?>|<field\s+name="([^"]+)"[^>]*>([^<]*)<\/field>|<\/value>|<\/shadow>/g;
    let tok;
    const catStack = [];
    // 現在のブロック・入力・shadow の追跡
    let curBlock = null;
    let curValue = null;
    let curShadow = null;
    while ((tok = tokenRe.exec(xml))) {
        const [full, catId, sepGap, blkType, , selfClose, valName, shadowType, shadowSelfClose, fieldName, fieldVal] = tok;
        if (tok.index === undefined) continue;
        if (full.startsWith('<category')) {
            catStack.push(catId);
            currentCat = catId;
            if (!blocksByCat.has(catId)) blocksByCat.set(catId, []);
            if (!categories.some(c => c.id === catId)) {
                categories.push({ id: catId, name: catId, items: [] });
            }
        } else if (full === '</category>') {
            catStack.pop();
            currentCat = catStack.length ? catStack[catStack.length - 1] : null;
        } else if (full.startsWith('<sep')) {
            // 直前ブロックの後に区切りマークを付ける
            if (currentCat && blocksByCat.has(currentCat)) {
                const list = blocksByCat.get(currentCat);
                if (list.length) list[list.length - 1] = list[list.length - 1]; // no-op
            }
        } else if (full.startsWith('<block')) {
            if (blkType && currentCat) {
                const list = blocksByCat.get(currentCat);
                if (list && !list.includes(blkType)) list.push(blkType);
                curBlock = blkType;
                curValue = null; curShadow = null;
            }
        } else if (full.startsWith('<value')) {
            curValue = valName;
            curShadow = null;
        } else if (full.startsWith('<shadow')) {
            curShadow = { type: shadowType, fields: {} };
            // 自己閉じ（<shadow type="..."/> — メニュー shadow は field を持たない）
            if (shadowSelfClose === '/' && curValue && curBlock) {
                if (!shadowDefaults.has(curBlock)) shadowDefaults.set(curBlock, {});
                shadowDefaults.get(curBlock)[curValue] = {
                    shadow: curShadow.type, field: '', value: ''
                };
                curShadow = null;
            }
        } else if (full.startsWith('<field')) {
            if (curShadow && curValue && curBlock) {
                curShadow.fields[fieldName] = fieldVal;
            }
        } else if (full === '</shadow>') {
            if (curShadow && curValue && curBlock) {
                if (!shadowDefaults.has(curBlock)) shadowDefaults.set(curBlock, {});
                const entry = shadowDefaults.get(curBlock);
                const fnames = Object.keys(curShadow.fields);
                entry[curValue] = {
                    shadow: curShadow.type,
                    field: fnames[0] || '',
                    value: fnames.length ? curShadow.fields[fnames[0]] : ''
                };
            }
            curShadow = null;
        }
    }
    // カテゴリ表示順（Scratch 既定）。toolboxitemid → 色キーへ正規化。
    const TOOLBOX_TO_COLOUR = {
        motion: 'motion', looks: 'looks', sound: 'sounds', events: 'event',
        control: 'control', sensing: 'sensing', operators: 'operators',
        variables: 'data', lists: 'data_lists'
    };
    const order = ['motion', 'looks', 'sounds', 'event', 'control', 'sensing', 'operators'];
    const ordered = [];
    for (const id of order) {
        // 色キーに一致する toolboxitemid を探す
        let foundId = null;
        for (const [tid, key] of Object.entries(TOOLBOX_TO_COLOUR)) {
            if (key === id && blocksByCat.has(tid)) { foundId = tid; break; }
        }
        if (foundId) {
            ordered.push({ id, items: blocksByCat.get(foundId) });
        }
    }
    return { ordered, shadowDefaults };
}

// ---------------------------------------------------------------------------
// C++ ヘッダ生成
// ---------------------------------------------------------------------------
function cppStr(s) {
    return '"' + String(s)
        .replace(/\\/g, '\\\\')
        .replace(/"/g, '\\"')
        .replace(/\n/g, '\\n') + '"';
}
function cppBool(b) { return b ? 'true' : 'false'; }

function shapeEnum(shape) {
    switch (shape) {
        case 'Hat': return 'Shape::Hat';
        case 'End': return 'Shape::End';
        case 'Reporter': return 'Shape::Reporter';
        case 'Boolean': return 'Shape::Boolean';
        default: return 'Shape::Stack';
    }
}
function kindEnum(kind) {
    return 'ArgKind::' + kind;
}

// 複数 message/args（message0/args0, message1/args1, ...）をマージし、
// ヘッダ行の引数（value/field/image）と C-block の substack に分離する。
function buildBlockModel(def) {
    const args = [];       // 非 statement 引数（%1..%n に対応）
    const subNames = [];
    const subLabels = [];
    let pending = [];
    for (let n = 0; ; n++) {
        const hasM = def['message' + n] !== undefined;
        const hasA = def['args' + n] !== undefined;
        if (!hasM && !hasA) break;
        const msg = def['message' + n] || '';
        const as = def['args' + n] || [];
        if (n >= 1 && msg && msg !== '%1') pending.push(msg);
        for (const a of as) {
            if (a.type === 'input_statement') {
                subLabels.push(pending.join(' '));
                subNames.push(a.name);
                pending = [];
            } else {
                args.push(argDef(a));
            }
        }
    }
    const sub2Label = subLabels.length >= 2 ? subLabels[1] : '';
    return { args, sub2Label, cblock: subNames.length };
}

function emit() {
    const { defs, errors } = parseBlocks();
    const xml = evalToolbox();
    const { ordered, shadowDefaults } = parseToolbox(xml);

    // データカテゴリ（ツールボックスは動的生成のため手動）
    const dataItems = [
        'data_variable', 'data_setvariableto', 'data_changevariableby',
        'data_showvariable', 'data_hidevariable',
        'data_listcontents', 'data_addtolist', 'data_deleteoflist',
        'data_deletealloflist', 'data_insertatlist', 'data_replaceitemoflist',
        'data_itemoflist', 'data_itemnumoflist', 'data_lengthoflist',
        'data_showlist', 'data_hidelist'
    ].filter(op => defs.has(op));
    if (dataItems.length) ordered.push({ id: 'data', items: dataItems });

    // カテゴリ名
    const catNames = {
        motion: 'Motion', looks: 'Looks', sounds: 'Sound', event: 'Events',
        control: 'Control', sensing: 'Sensing', operators: 'Operators',
        data: 'Variables', more: 'My Blocks'
    };

    const lines = [];
    lines.push('// AUTO-GENERATED by tools/gen_block_catalog.mjs — DO NOT EDIT.');
    lines.push('// scratch-blocks (Blockly fork) + scratch-gui toolbox から生成。');
    lines.push('#pragma once');
    lines.push('#include <cstdint>');
    lines.push('#include <string>');
    lines.push('#include <string_view>');
    lines.push('#include <vector>');
    lines.push('');
    lines.push('namespace xyo { namespace edit {');
    lines.push('');
    lines.push('enum class Shape : uint8_t { Stack, Hat, End, Reporter, Boolean };');
    lines.push('enum class ArgKind : uint8_t { Value, Statement, Number, Angle, Dropdown, Variable, BooleanField, Image, Label };');
    lines.push('');
    lines.push('struct ArgDef {');
    lines.push('    ArgKind kind = ArgKind::Value;');
    lines.push('    std::string name;         // input/field 名（STEPS, NUM, VARIABLE…）');
    lines.push('    std::string dflt;         // field の既定値（数値/角度/ドロップダウン値）');
    lines.push('    std::vector<std::string> options; // ドロップダウン [label,value,...]');
    lines.push('    std::string shadow;       // value input の既定 shadow opcode');
    lines.push('    std::string shadowField;  // shadow 側の既定 field 名');
    lines.push('    std::string shadowDefault;// shadow 側の既定 field 値');
    lines.push('};');
    lines.push('');
    lines.push('struct BlockDef {');
    lines.push('    std::string opcode;');
    lines.push('    std::string label;        // message0（%1..%n プレースホルダ）');
    lines.push('    Shape shape = Shape::Stack;');
    lines.push('    uint8_t cblock = 0;       // 0=通常, 1=Loop(substack), 2=IfElse(substack2)');
    lines.push('    uint32_t primary = 0, secondary = 0, tertiary = 0;');
    lines.push('    std::string sub2Label;    // if-else の else ラベル（"" = なし）');
    lines.push('    std::vector<ArgDef> args;');
    lines.push('};');
    lines.push('');
    lines.push('struct CategoryDef {');
    lines.push('    std::string id;');
    lines.push('    std::string name;');
    lines.push('    uint32_t primary = 0;');
    lines.push('    std::vector<std::string> items; // パレット内の opcode 順');
    lines.push('};');
    lines.push('');
    lines.push('inline const std::vector<BlockDef> kBlockDefs = {');
    for (const [opcode, def] of defs) {
        const cls = classify(def);
        const cat = categoryOf(def);
        const col = colorFor(cat);
        const model = buildBlockModel(def);
        const args = model.args;
        // shadow 既定値を value input に注入
        const sd = shadowDefaults.get(opcode) || {};
        for (const a of args) {
            if (a.kind === 'Value' && sd[a.name]) {
                a.shadow = sd[a.name].shadow;
                a.shadowField = sd[a.name].field;
                a.shadowDefault = sd[a.name].value;
            }
        }
        // data_* は custom="VARIABLE" で動的生成（scratch-blocks data_category.js
        // の既定 shadow を手動で注入: 'thing' は DEFAULT_LIST_ITEM）
        const dataShadows = {
            'data_setvariableto':    { VALUE:  ['text', 'TEXT', '0'] },
            'data_changevariableby': { VALUE:  ['math_number', 'NUM', '1'] },
            'data_addtolist':        { ITEM:   ['text', 'TEXT', 'thing'] },
            'data_deleteoflist':     { INDEX:  ['math_integer', 'NUM', '1'] },
            'data_insertatlist':     { INDEX:  ['math_integer', 'NUM', '1'],
                                       ITEM:   ['text', 'TEXT', 'thing'] },
            'data_replaceitemoflist':{ INDEX:  ['math_integer', 'NUM', '1'],
                                       ITEM:   ['text', 'TEXT', 'thing'] },
            'data_itemoflist':       { INDEX:  ['math_integer', 'NUM', '1'] },
            'data_itemnumoflist':    { ITEM:   ['text', 'TEXT', 'thing'] },
            'data_listcontainsitem': { ITEM:   ['text', 'TEXT', 'thing'] },
        };
        // procedures_definition の %1 = custom_block 入力（定義 xml では
        // <field> なので parseBlocklyBlocks からは取れない — 手動注入）
        if (opcode === 'procedures_definition') {
            args.push({ kind: 'Value', name: 'custom_block', dflt: '',
                        options: [], shadow: '', shadowField: '',
                        shadowDefault: '' });
        }
        const ds = dataShadows[opcode];
        if (ds) {
            for (const a of args) {
                if (a.kind === 'Value' && ds[a.name]) {
                    const [sh, sf, sv] = ds[a.name];
                    a.shadow = sh; a.shadowField = sf; a.shadowDefault = sv;
                }
            }
        }
        lines.push('    { ' + cppStr(opcode) + ', ' + cppStr(def.message0 || '') + ', ' +
            shapeEnum(cls.shape) + ', ' + String(model.cblock) + ', ' +
            '0x' + col.primary + ', 0x' + col.secondary + ', 0x' + col.tertiary + ', ' +
            cppStr(model.sub2Label) + ', {');
        for (const a of args) {
            let opt = '';
            if (a.options.length) {
                opt = ', {';
                for (const o of a.options) opt += cppStr(o) + ', ';
                opt += '}';
            } else {
                opt = ', {}';
            }
            lines.push('        { ' + kindEnum(a.kind) + ', ' + cppStr(a.name) + ', ' +
                cppStr(a.dflt) + opt + ', ' + cppStr(a.shadow) + ', ' +
                cppStr(a.shadowField) + ', ' + cppStr(a.shadowDefault) + ' },');
        }
        lines.push('    } },');
    }
    lines.push('};');
    lines.push('');
    lines.push('inline const std::vector<CategoryDef> kCategories = {');
    for (const c of ordered) {
        const col = colorFor(c.id);
        lines.push('    { ' + cppStr(c.id) + ', ' + cppStr(catNames[c.id] || c.id) +
            ', 0x' + col.primary + ', {');
        for (const it of c.items) lines.push('        ' + cppStr(it) + ',');
        lines.push('    } },');
    }
    lines.push('};');
    lines.push('');
    // 拡張ブロック（pen/music — scratch-vm extensions/scratch3_* から手動同期。
    // 実 sb3 の opcode は `pen_`/`music_` プレフィックス。飛び込みで project.json の
    // extensions に応じてツールボックスへ追加する）
    lines.push('inline const std::vector<BlockDef> kExtensionDefs = {');
    const EXT_COL = {
        pen:    ['0x0FBD8C', '0x0DA57A', '0x0B8E69'],
        music:  ['0xCF63CF', '0xC94FC9', '0xBD42BD'],
    };
    const EXT_ITEMS = [
        { op: 'pen_clear', label: 'erase all', shape: 'Stack', cat: 'pen', args: [] },
        { op: 'pen_stamp', label: 'stamp', shape: 'Stack', cat: 'pen', args: [] },
        { op: 'pen_penDown', label: 'pen down', shape: 'Stack', cat: 'pen', args: [] },
        { op: 'pen_penUp', label: 'pen up', shape: 'Stack', cat: 'pen', args: [] },
        { op: 'pen_setPenColorToColor', label: 'set pen color to %1', shape: 'Stack', cat: 'pen',
          args: [{ kind: 'Value', name: 'COLOR', dflt: '', options: [], shadow: 'colour_picker', shadowField: 'COLOUR', shadowDefault: '#9966FF' }] },
        { op: 'pen_changePenColorParamBy', label: 'change pen %1 by %2', shape: 'Stack', cat: 'pen',
          args: [{ kind: 'Dropdown', name: 'COLOR_PARAM', dflt: 'color', options: ['color', 'color', 'saturation', 'saturation', 'brightness', 'brightness', 'transparency', 'transparency'] },
                 { kind: 'Value', name: 'VALUE', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '10' }] },
        { op: 'pen_setPenColorParamTo', label: 'set pen %1 to %2', shape: 'Stack', cat: 'pen',
          args: [{ kind: 'Dropdown', name: 'COLOR_PARAM', dflt: 'color', options: ['color', 'color', 'saturation', 'saturation', 'brightness', 'brightness', 'transparency', 'transparency'] },
                 { kind: 'Value', name: 'VALUE', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '50' }] },
        { op: 'pen_changePenSizeBy', label: 'change pen size by %1', shape: 'Stack', cat: 'pen',
          args: [{ kind: 'Value', name: 'SIZE', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '1' }] },
        { op: 'pen_setPenSizeTo', label: 'set pen size to %1', shape: 'Stack', cat: 'pen',
          args: [{ kind: 'Value', name: 'SIZE', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '1' }] },
        { op: 'music_playNoteForBeats', label: 'play note %1 for %2 beats', shape: 'Stack', cat: 'music',
          args: [{ kind: 'Value', name: 'NOTE', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '60' },
                 { kind: 'Value', name: 'BEATS', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '0.5' }] },
        { op: 'music_restForBeats', label: 'rest for %1 beats', shape: 'Stack', cat: 'music',
          args: [{ kind: 'Value', name: 'BEATS', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '0.5' }] },
        { op: 'music_setInstrument', label: 'set instrument to %1', shape: 'Stack', cat: 'music',
          args: [{ kind: 'Dropdown', name: 'INSTRUMENT', dflt: '1', options: ['(1) Piano', '1', '(2) Bass', '2', '(3) Harp', '3'] }] },
        { op: 'music_setTempo', label: 'set tempo to %1', shape: 'Stack', cat: 'music',
          args: [{ kind: 'Value', name: 'TEMPO', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '60' }] },
        { op: 'music_changeTempo', label: 'change tempo by %1', shape: 'Stack', cat: 'music',
          args: [{ kind: 'Value', name: 'TEMPO', dflt: '', options: [], shadow: 'math_number', shadowField: 'NUM', shadowDefault: '20' }] },
        { op: 'music_getTempo', label: 'tempo', shape: 'Reporter', cat: 'music', args: [] },
    ];
    for (const it of EXT_ITEMS) {
        const col = EXT_COL[it.cat];
        lines.push('    { ' + cppStr(it.op) + ', ' + cppStr(it.label) + ', ' +
            shapeEnum(it.shape) + ', 0, ' +
            col[0] + ', ' + col[1] + ', ' + col[2] + ', "", {');
        for (const a of it.args) {
            let opt = ', {}';
            if (a.options.length) {
                opt = ', {';
                for (const o of a.options) opt += cppStr(o) + ', ';
                opt += '}';
            }
            lines.push('        { ' + kindEnum(a.kind) + ', ' + cppStr(a.name) + ', ' +
                cppStr(a.dflt) + opt + ', ' + cppStr(a.shadow || '') + ', ' +
                cppStr(a.shadowField || '') + ', ' + cppStr(a.shadowDefault || '') + ' },');
        }
        lines.push('    } },');
    }
    lines.push('};');
    lines.push('');
    lines.push('inline const std::vector<CategoryDef> kExtensionCategories = {');
    lines.push('    { "pen", "Pen", 0x0FBD8C, {');
    for (const it of EXT_ITEMS) if (it.cat === 'pen') lines.push('        ' + cppStr(it.op) + ',');
    lines.push('    } },');
    lines.push('    { "music", "Music", 0xCF63CF, {');
    for (const it of EXT_ITEMS) if (it.cat === 'music') lines.push('        ' + cppStr(it.op) + ',');
    lines.push('    } },');
    lines.push('};');
    lines.push('');
    lines.push('inline const BlockDef* findBlock(std::string_view opcode) {');
    lines.push('    for (const auto& b : kBlockDefs) if (b.opcode == opcode) return &b;');
    lines.push('    for (const auto& b : kExtensionDefs) if (b.opcode == opcode) return &b;');
    lines.push('    return nullptr;');
    lines.push('}');
    lines.push('');
    lines.push('}} // namespace xyo::edit');
    lines.push('');

    fs.writeFileSync(OUT, lines.join('\n'));
    if (errors.length) {
        console.error('parse errors:');
        for (const e of errors) console.error('  ' + e);
    }
    console.log('blocks: ' + defs.size + '  categories: ' + ordered.length);
    for (const c of ordered) console.log('  ' + c.id + ': ' + c.items.length);
}

emit();
