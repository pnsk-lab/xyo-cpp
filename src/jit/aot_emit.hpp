// xyo-cpp: AOT（オブジェクト事前生成）向けのポインタ間接化。
//
// 生成コードは通常、ホストプロセスの絶対アドレスを IR の即値として
// 埋め込む — 変数 id の C 文字列、BlockCached*、SStr* リテラル、
// ASCII キャッシュ配列、pen 移動フック。JIT では同一プロセスなので
// そのままで正しいが、オブジェクトを別プロセスでロードする AOT では
// 全て無効になる。
//
// AOT モードでは各ポインタをモジュール内のグローバル slot
// （xyo_aot_slot_N）に置き換え、生成コードは slot を load する。
// ロード時に実アドレスを slot へ書き込むことで、同一オブジェクトを
// 何度でも（ASLR の下でも）実行できる。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xyo {

struct SStr;

/// slot の解決方法。ロード側（JitEngine::loadAot）がこの種別を見て
/// 実アドレスを決める。
enum class AotSlotKind : uint8_t {
    VarId = 0,        // 変数 id の C 文字列（ポインタ同一性を保つ）
    Name = 1,         // 入力名の C 文字列
    BlockCached = 2,  // BlockCached*（compat 呼出し・手続きフレーム）
    StrLit = 3,       // SStr* リテラル（GC ルート）
    AsciiCache = 4,   // 1 文字 ASCII キャッシュ配列の先頭
    MovedHook = 5,    // pen 移動フック関数
    GpuProgram = 6,   // GPU 並列ループの Program*（ロード時に再コンパイル）
};

struct AotSlot {
    AotSlotKind kind = AotSlotKind::VarId;
    /// VarId/Name: UTF-8、BlockCached: block id、StrLit: UTF-16 の生バイト
    std::string key;
    /// モジュール内グローバル名（xyo_aot_slot_N）
    std::string global;
};

/// emit 中の slot 採番。同一 (kind,key) は同じ slot を共有する。
struct AotEmit {
    std::vector<AotSlot> slots;
    std::unordered_map<std::string, uint32_t> index;
    /// 現在 emit 中のターゲットのインデックス（Runtime::targets 順）。
    /// ブロック id はターゲットを跨いで一意とは限らない（生成器が
    /// "hat" 等を再利用し得る）ため、BlockCached slot のキーに混ぜる。
    uint32_t targetIndex = 0;

    const std::string& slot(AotSlotKind kind, const std::string& key) {
        std::string k;
        k.reserve(key.size() + 1);
        k.push_back(static_cast<char>(kind));
        k += key;
        auto it = index.find(k);
        if (it != index.end()) return slots[it->second].global;
        uint32_t i = static_cast<uint32_t>(slots.size());
        slots.push_back(
            {kind, key, "xyo_aot_slot_" + std::to_string(i)});
        index.emplace(std::move(k), i);
        return slots.back().global;
    }
};

/// 生成コードが参照する動的リソース（JIT: emit 中の文字列を GC ルート化）
struct JitPinned {
    std::vector<SStr*> strings;
};

} // namespace xyo
