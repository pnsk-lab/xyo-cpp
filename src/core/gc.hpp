// xyo-cpp: ミニ mark-sweep GC（design.md §5.4）。
// 対象オブジェクト: SStr / SList。非移動・個別 malloc、全オブジェクトを
// 侵入リストで追跡。ルートは登録プロバイダ経由で走査し、
// ファイバスタックは保守的ワード走査で捕捉する。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "value.hpp"

namespace xyo {

enum class GcType : uint16_t { Str = 1, List = 2 };

struct GcHeader {
    GcHeader* next;    // 全オブジェクト侵入リスト
    uint32_t size;     // ヘッダ除くペイロードバイト数
    GcType type;
    uint16_t mark;     // GC マークビット
};

class Gc {
public:
    static Gc& get();

    /// bytes バイトのペイロードを持つオブジェクトを割り当てる。
    /// 閾値超過時は collect を先行実行する。
    void* alloc(GcType type, uint32_t bytes);

    /// ルートプロバイダ登録: fn(gc) 内で gc.markValue/markPtr を呼ぶ。
    /// owner は removeRootProvider での解除キー（例: Runtime*）。
    void addRootProvider(void* owner, std::function<void(Gc&)> fn) {
        rootProviders_.push_back({owner, std::move(fn)});
    }
    void removeRootProvider(void* owner);

    /// 手動回収（割当閾値で自動的にも呼ばれる）
    void collect();

    // --- 並列フェーズ対応 ---
    /// true の間は alloc が閾値超過しても collect を行わない。
    /// スクリプトの並列実行中に worker スレッド上で collect が走ると
    /// メインスタックスキャン/ルート走査が破綻するため、フェーズ区間は
    /// メインスレッドが setDeferred(true/false) で囲む。
    void setDeferred(bool d) { deferredCollect_.store(d); }
    /// 遅延中に閾値超過があれば回収（メインスレッドからのみ呼ぶこと）
    void collectIfNeeded();

    /// スコープ単位の collect 遅延。GC オブジェクトがルート未接続の
    /// コンテナ（ロード中の未登録 Target 等）に格納される区間で使う。
    /// 終了時に以前の状態へ復帰する（ネスト可）。
    struct DeferCollect {
        DeferCollect() : prev(Gc::get().deferredCollect_.exchange(true)) {}
        ~DeferCollect() { Gc::get().deferredCollect_.store(prev); }
        DeferCollect(const DeferCollect&) = delete;
        DeferCollect& operator=(const DeferCollect&) = delete;
        bool prev;
    };

    // --- マーカ ---
    void markValue(Value v);
    void markPtr(void* p);          // 生ポインタ（obj 先頭判定）
    void markObject(GcHeader* h);

    /// 保守的ワード走査: [begin,end) の u64 を Value タグ付き・
    /// 生オブジェクトポインタ双方の候補として走査（ファイバスタック用）
    void scanRange(const void* begin, const void* end);

    bool isObject(void* p) const { return objSet_.count(static_cast<GcHeader*>(p)) != 0; }

    size_t bytesAllocated() const { return bytesAllocated_; }
    size_t objectCount() const { return objSet_.size(); }
    size_t collectCount() const { return collectCount_; }
    void setThreshold(size_t b) { threshold_ = b; }

    /// SList の内部バッファ解放など、型別の破棄を sweep 時に行う
    static void destroyPayload(GcHeader* h);

private:
    GcHeader* allObjects_ = nullptr;
    std::unordered_set<GcHeader*> objSet_;
    /// scanRange 用のページフィルタ — オブジェクトのヘッダとペイロード
    /// 開始が乗るページ (addr>>12) の集合。積まれた全ワードの objSet_
    /// ルックアップを1回のページ確認に減らす。権威は objSet_ で、
    /// こちらは棄却専用のため collect ごとに再構築すれば十分。
    std::unordered_set<uintptr_t> pageSet_;
    struct RootProvider {
        void* owner;
        std::function<void(Gc&)> fn;
    };
    std::vector<RootProvider> rootProviders_;
    size_t bytesAllocated_ = 0;
    size_t threshold_ = 2 << 20;   // 2MiB 起点
    size_t collectCount_ = 0;      // collect 実施回数（診断用）
    bool collecting_ = false;
    /// alloc の侵入リスト/objSet_ 更新を直列化（並列 worker 用）。
    /// collect が同 mutex 保持下で走り、ルート走査内の割当
    /// （Mutation::parseArgs → SStr::create 等）が再入するため再帰型。
    std::recursive_mutex allocMtx_;
    std::atomic<bool> deferredCollect_{false};
};

} // namespace xyo
