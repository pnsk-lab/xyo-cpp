// xyo-cpp: ミニ mark-sweep GC（design.md §5.4）。
// 対象オブジェクト: SStr / SList。非移動、小文字列は固定サイズスラブ、
// その他は個別 malloc。全オブジェクトを侵入リストで追跡する。
// ルートは登録プロバイダ経由で走査し、
// ファイバスタックは保守的ワード走査で捕捉する。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "value.hpp"

namespace xyo {

enum class GcType : uint16_t { Str = 1, List = 2, AppendLog = 3 };

struct GcHeader {
    GcHeader* next;    // 全オブジェクト侵入リスト
    uint32_t size;     // ヘッダ除くペイロードバイト数
    GcType type;
    uint16_t mark;     // GC マークビット
};

/// ポインタ/整数キーのフラットな集合（オープンアドレッシング、線形探査）。
/// std::unordered_set は要素ごとにノードを malloc するため、GC では
/// 「オブジェクト 1 個の確保 = 2 回 malloc + ハッシュ 3 回」になっていた
/// （文字列 1 個 ≈ 200ns の主因）。挿入と参照だけを O(1) 平均で行い、
/// 削除は持たない — sweep の後に生存オブジェクトで作り直す。
class PtrSet {
public:
    static constexpr uintptr_t kEmpty = 0;
    void insert(uintptr_t k) {
        if (k == kEmpty) return;
        if (slots_.empty()) grow(1024);
        if ((size_ + 1) * 10 >= slots_.size() * 7) grow(slots_.size() * 2);
        size_t mask = slots_.size() - 1;
        size_t i = hash(k) & mask;
        while (slots_[i] != kEmpty) {
            if (slots_[i] == k) return;
            i = (i + 1) & mask;
        }
        slots_[i] = k;
        ++size_;
    }
    bool count(uintptr_t k) const {
        if (slots_.empty() || k == kEmpty) return false;
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(k) & mask;; i = (i + 1) & mask) {
            if (slots_[i] == k) return true;
            if (slots_[i] == kEmpty) return false;
        }
    }
    void clear() {
        if (size_) std::fill(slots_.begin(), slots_.end(), kEmpty);
        size_ = 0;
    }
    // Rebuilding after sweep must not retain the largest historical table.
    // Shrink only after a 4x drop to avoid allocation churn near a boundary.
    void resetForRebuild(size_t expected) {
        size_t want = 1024;
        while (want * 7 / 10 < expected) want *= 2;
        if (slots_.size() > want && slots_.size() / want >= 4) {
            std::vector<uintptr_t> fresh(want, kEmpty);
            slots_.swap(fresh);
            size_ = 0;
        } else {
            clear();
        }
    }
    size_t capacity() const { return slots_.size(); }
    void reserve(size_t n) {
        size_t want = 1024;
        while (want * 7 / 10 < n) want *= 2;
        if (want > slots_.size()) grow(want);
    }
    size_t size() const { return size_; }
private:
    static size_t hash(uintptr_t k) {
        return size_t((k * 0x9E3779B97F4A7C15ull) >> 17);
    }
    void grow(size_t cap) {
        std::vector<uintptr_t> old;
        old.swap(slots_);
        slots_.assign(cap, kEmpty);
        size_t saved = size_;
        size_ = 0;
        for (uintptr_t k : old)
            if (k != kEmpty) insert(k);
        size_ = saved;
    }
    std::vector<uintptr_t> slots_;
    size_t size_ = 0;
};

class Gc {
public:
    static Gc& get();
    Gc();
    ~Gc();
    Gc(const Gc&) = delete;
    Gc& operator=(const Gc&) = delete;

    /// bytes バイトのペイロードを持つオブジェクトを割り当てる。
    /// 閾値超過時は collect を先行実行する。
    enum class PoolKind : uint8_t { General = 0, Cons = 1 };
    void* alloc(GcType type, uint32_t bytes);
#if defined(XYO_STRING_CONS_POOL)
    // Used only by sstrCons in the allocator-specialized build; keep the
    // common two-argument allocation ABI identical in the baseline variant.
    void* allocCons(uint32_t bytes);
#endif

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
    /// マーク済みオブジェクトの子を反復で消化する（深い cons 連鎖・
    /// 入れ子リストでマーク中に再帰しないため）。
    void drainPending();

    /// 保守的ワード走査: [begin,end) の u64 を Value タグ付き・
    /// 生オブジェクトポインタ双方の候補として走査（ファイバスタック用）
    void scanRange(const void* begin, const void* end);

    /// Exact GC HEADER membership (not a payload/interior address).
    bool isObject(void* p) const;

    size_t bytesAllocated() const { return bytesAllocated_.load(std::memory_order_relaxed); }
    // Append-log chunks live outside the GC allocation payload.
    void noteExternalAllocation(size_t bytes) {
        bytesAllocated_.fetch_add(bytes, std::memory_order_relaxed);
    }
    /// Diagnostic counts require a quiescent collector (workers joined, no
    /// concurrent allocation/collection), as do the slab and malloc registries.
    size_t objectCount() const {
        return objSet_.size() + smallPoolObjectCount();
    }
    bool smallPoolEnabled() const { return poolEnabled_; }
    size_t smallPoolReservedBytes() const { return poolSlabs_.size() * kSlabBytes; }
    size_t smallPoolReservedBytes(PoolKind pool) const;
    size_t smallPoolObjectCount(PoolKind pool) const;
    size_t smallPoolSlabCount(PoolKind pool) const;
    size_t smallPoolObjectCount() const {
        size_t count = 0;
        for (const SmallSlab* slab : poolSlabs_) count += slab->live;
        return count;
    }
    size_t collectCount() const { return collectCount_; }
    void setThreshold(size_t b) { threshold_ = b; }

    /// SList の内部バッファ解放など、型別の破棄を sweep 時に行う
    static void destroyPayload(GcHeader* h);

private:
    // Small strings share aligned slabs; their bitmap is the exact membership
    // authority, so allocation does not insert each cell into objSet_. Header
    // and payload ABIs remain unchanged. Lists/large strings retain malloc.
    static constexpr size_t kSlabBytes = 64 * 1024;
    static constexpr size_t kCellBytes[3] = {80, 96, 128};
    struct SmallSlab {
        SmallSlab* availableNext;
        uint32_t classIndex;
        PoolKind pool;
        uint32_t capacity;
        uint32_t live;
        uint32_t firstFreeWord;
        // While a thread owns the slab, [nextFreeCell, freeRunEnd) contains
        // exclusively unallocated cells. GC invalidates every run.
        uint32_t nextFreeCell;
        uint32_t freeRunEnd;
        uint64_t allocated[16];
    };
    static constexpr size_t kSlabDataOffset = (sizeof(SmallSlab) + 15) & ~size_t(15);
    static_assert((kSlabBytes - kSlabDataOffset) / kCellBytes[0] <= 16 * 64);
    static_assert(alignof(GcHeader) <= 16);
    static SmallSlab* slabBase(uintptr_t p) {
        return reinterpret_cast<SmallSlab*>(p & ~(uintptr_t(kSlabBytes) - 1));
    }
    SmallSlab* acquireSlabLocked(unsigned cls, PoolKind pool);
    GcHeader* allocSmallTls(size_t total, PoolKind pool);
    void sweepSmallSlabs(size_t& live);
    void rebuildSmallSlabs();
    void markHeader(uintptr_t address);
    bool validHeader(uintptr_t address) const;
    bool poolEnabled_ = true;
    // Contiguous free-run allocation is enabled by default. The environment
    // can restore the original bitmap-per-cell allocator for diagnostics.
    bool poolRunAlloc_ = true;
    // Rope children are produced by the runtime, not discovered by the
    // conservative stack scanner. Avoid revalidating their slab cell bits
    // while retaining safe fallback for malloc / foreign collector strings.
    bool fastConsChildMark_ = true;
    std::vector<SmallSlab*> poolSlabs_;
    PtrSet slabBases_;
    SmallSlab* availableSlabs_[2][3] = {};
    /// Bumped after every collection. A thread's cached slab pointer is only
    /// usable while its generation matches; a stale pointer is never
    /// dereferenced (the slab may have been released).
    std::atomic<uint64_t> poolGen_{0};
    GcHeader* allObjects_ = nullptr;   // malloc objects only
    PtrSet objSet_;
    /// scanRange 用のページフィルタ — オブジェクトのヘッダとペイロード
    /// 開始が乗るページ (addr>>12) の集合。積まれた全ワードの objSet_
    /// ルックアップを1回のページ確認に減らす。権威は objSet_ で、
    /// こちらは棄却専用のため collect ごとに再構築すれば十分。
    PtrSet pageSet_;
    struct RootProvider {
        void* owner;
        std::function<void(Gc&)> fn;
    };
    std::vector<RootProvider> rootProviders_;
    /// マーク待ち（cons string の鎖・リストの入れ子を反復で辿る）
    std::vector<GcHeader*> pending_;
    std::atomic<size_t> bytesAllocated_{0};
    size_t minThreshold_ = 8 << 20;   // XYO_GC_THRESHOLD (MiB) で上書き可
    size_t threshold_ = 8 << 20;   // 8MiB 起点（ファイバ走査修正後も
                                  // 文字列ループの collect 回数が支配的だったため）
    size_t collectCount_ = 0;      // collect 実施回数（診断用）
    uintptr_t lastPage_ = ~uintptr_t(0);   // pageSet_ 挿入の間引き用
    std::atomic<bool> collecting_{false};
    /// alloc の侵入リスト/objSet_ 更新を直列化（並列 worker 用）。
    /// collect が同 mutex 保持下で走り、ルート走査内の割当
    /// （Mutation::parseArgs → SStr::create 等）が再入するため再帰型。
    std::recursive_mutex allocMtx_;
    std::atomic<bool> deferredCollect_{false};
};

} // namespace xyo
