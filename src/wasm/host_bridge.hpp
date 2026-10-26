// xyo-cpp: wasm JIT のホストブリッジ（Phase C）。
//
// ランタイム（wasm）が生成した wasm モジュールを JS 側で instantiate し、
// その entry を呼ぶための橋。実装は src/wasm/host_bridge.cpp（EM_JS）。
// ネイティブビルドでは未対応（instantiate は -1 を返す）。
#pragma once

#include <cstdint>

namespace xyo::wasm {

/// 生成した wasm バイト列を JS で instantiate し、ハンドルを返す。
/// imports は env.memory（ランタイムの線形メモリ）と env.h（ヘルパ
/// ディスパッチャ）。失敗時は -1。
int instantiate(const void* bytes, int len);

/// 生成モジュールの entry `f(ctxPtr)` を呼ぶ。handle が無効なら何もしない。
void invoke(int handle, int ctxPtr);

} // namespace xyo::wasm

/// ブリッジとエンコーダの自己テスト。EMSCRIPTEN でのみ実処理を行い、
/// ネイティブでは 0 を返す（ビルド・テストの互換のため）。
extern "C" int xyo_wasm_selftest();
