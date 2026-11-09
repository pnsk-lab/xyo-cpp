// xyo-cpp: par-for ランタイム — 反復独立に変形した append ループの並列実行。
//
// JIT は検出済みのループ本体を
//   void body(JitCtx*, int64_t iter, double ctr, void** lists, int64_t* bases)
// へ emit し、カウンタは反復ローカル、append は「base+iter 番目のスロット書き込み」
// にしてある。ここでは反復範囲をチャンク分割してワーカープールへ流す。
//
// 意味論の要点:
//  * append 順 = 反復順なので、各リストの最終内容は逐次実行と同一。
//  * リストの len/cap は**並列領域の前に**確定させる（ワーカーは slot を
//    書くだけで、len/cap の共有書き込みは発生しない）。
//  * カウンタの値は `init + iter*delta` で厳密（init は整数・delta は整数・
//    |init| + delta*trip < 2^53 を実行時に検証）。満たさなければ 0 を返して
//    呼び出し側を逐次ループへフォールバックさせる。
//  * GC は領域中 collect を遅延する（ワーカーは alloc するが、書いた値は
//    リストの slot から到達可能。領域終了後の通常 collect で回収される）。
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "../core/gc.hpp"
#include "../core/slist.hpp"
#include "../core/value.hpp"
#include "../engine/pool.hpp"
#include "jit.hpp"

namespace {

using ParForBody = void (*)(xyo::JitCtx*, int64_t, double, void**, int64_t*);

bool parForEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("XYO_PARFOR");
        return !(v && v[0] == '0');
    }();
    return on;
}

/// 1 チャンクの下限反復数
int64_t minChunk() {
    static const int64_t n = [] {
        const char* v = std::getenv("XYO_PARFOR_CHUNK");
        int64_t r = v ? std::strtoll(v, nullptr, 10) : 512;
        return r > 0 ? r : int64_t(512);
    }();
    return n;
}

// A 1000-item append often takes less time on one core than it costs to
// enqueue worker tasks, wake them and join. The compiler may still emit
// this fused batch at 512 iterations: it only starts parallel workers for
// sufficiently large batches.
int64_t minParallelTrip() {
    static const int64_t n = [] {
        const char* v = std::getenv("XYO_PARFOR_PARALLEL_MIN");
        const int64_t requested = v ? std::strtoll(v, nullptr, 10) : 2048;
        return requested > 0 ? requested : int64_t(2048);
    }();
    return n;
}

/// par-for 専用プール。sequencer のバッチ用プールとは分離する
/// （WorkerPool::run は再入不可のため、こちらは try_lock で直列化する）。
xyo::WorkerPool& parPool() {
    static xyo::WorkerPool* p = new xyo::WorkerPool();
    return *p;
}

std::mutex& parMutex() {
    static std::mutex m;
    return m;
}

struct Chunk {
    xyo::JitCtx* ctx;
    double init;
    double delta;
    void** lists;
    int64_t* bases;
    ParForBody body;
    int64_t lo, hi;
};

void runChunk(void* p) {
    const Chunk* c = static_cast<const Chunk*>(p);
    for (int64_t it = c->lo; it < c->hi; ++it)
        c->body(c->ctx, it, c->init + double(it) * c->delta, c->lists,
                c->bases);
}

}  // namespace

extern "C" {

/// テスト用カウンタ: 実際に並列ディスパッチした回数 / 反復数の合計
int64_t xyo_parfor_runs = 0;
int64_t xyo_parfor_iters = 0;

/// リストを newLen まで伸ばし、増えた分を 0 で埋める。
/// 並列領域の前に len/cap を確定させる用途（ワーカーは slot を書くだけ）。
void xyo_list_reserve(void* listPtr, int64_t newLen) {
    auto* l = static_cast<xyo::SList*>(listPtr);
    if (!l || newLen <= int64_t(l->len)) return;
    const uint32_t n = uint32_t(newLen);
    l->numericOnly = false; // concurrent slot writers may store strings
    l->ensure(n);
    for (uint32_t i = l->len; i < n; ++i) l->items[i] = xyo::Value::num(0);
    l->len = n;
    l->monitorDirty = true;
}

// The analyzer guarantees one append to each destination per iteration,
// and the body writes every reserved slot in increasing iteration order.
// No helper caller may read the unfinished range while this batch executes.
// Every destination slot is written by the JIT body before GC can run;
// set XYO_PARFOR_SKIP_ZERO_FILL=0 to restore defensive initialization.
static void reserveParForDestination(xyo::SList* list, int64_t newLen,
                                     bool skipZeroFill) {
    if (!list || newLen <= int64_t(list->len)) return;
    const uint32_t n = uint32_t(newLen);
    if (!skipZeroFill) {
        xyo_list_reserve(list, newLen);
        return;
    }
    list->numericOnly = false;
    list->ensure(n);
    list->len = n;
    list->monitorDirty = true;
}

/// ワーカーから 1 スロットを書く（len は確保済み）。
void xyo_par_slot(void* listPtr, int64_t idx, int64_t bits) {
    auto* l = static_cast<xyo::SList*>(listPtr);
    if (!l || idx < 0 || idx >= int64_t(l->len)) return;
    xyo::Value v;
    v.bits = uint64_t(bits);
    // Workers may write concurrently; invalidate type speculation before
    // dispatch instead of performing a racy per-worker flag update.
    l->items[idx] = v;
}

/// 成功したら 1（リストは trip 個伸びて埋まっている）、
/// 何もせず逐次ループへ戻すなら 0。
int xyo_par_for(xyo::JitCtx* ctx, int64_t trip, double init, double delta,
                void** lists, int64_t* bases, int64_t nl, ParForBody body) {
    if (trip <= 0) return 1;   // 0 回 = 何もしないで成功
    if (!ctx || !body || nl <= 0 || !lists || !bases) return 0;
    if (!parForEnabled()) return 0;
    if (xyo::xyo_list_storage_mode >= 4) return 0; // No parallel direct writes in adaptive mode.
    if (!std::isfinite(init) || init != std::floor(init)) return 0;
    if (!std::isfinite(delta) || delta != std::floor(delta)) return 0;
    if (!(std::fabs(init) + std::fabs(delta) * double(trip) < 9.0e15))
        return 0;
    for (int64_t k = 0; k < nl; ++k)
        if (!lists[k]) return 0;
    // SList の len は uint32 — 収まらない規模は逐次へ
    for (int64_t k = 0; k < nl; ++k)
        if (uint64_t(bases[k]) + uint64_t(trip) > 0xFFFFFFF0ull) return 0;
    // リスト長上限があるときは、予約で上限を超えるなら逐次（SList::push が
    // 上限を見て落とす）へ戻す。
    if (xyo::xyo_list_limit)
        for (int64_t k = 0; k < nl; ++k)
            if (uint64_t(bases[k]) + uint64_t(trip) >
                uint64_t(xyo::xyo_list_limit))
                return 0;

    static const bool skipZeroFill = [] {
        const char* env = std::getenv("XYO_PARFOR_SKIP_ZERO_FILL");
        return !env || env[0] != '0';
    }();
    // GC must not inspect a partially filled list between increasing len
    // and the last body store. This also protects allocated SStr values.
    xyo::Gc::DeferCollect defer;
    // 予約は直列区間で（len/cap の共有書き込みを作らない）
    for (int64_t k = 0; k < nl; ++k) {
        // A worker may write any Scratch Value, including strings.
        // Invalidate before dispatch (even when reserve is a no-op).
        auto* list = static_cast<xyo::SList*>(lists[k]);
        list->numericOnly = false;
        reserveParForDestination(list, bases[k] + trip, skipZeroFill);
    }

    // The common small-batch path must not initialize the worker pool or
    // acquire the global try-lock. These overheads dominate append loops
    // that execute thousands of times. Keep exactly the same body ordering
    // and GC deferral as the old one-chunk fallback.
    const int64_t chunk = minChunk();
    const int64_t candidateChunks = std::max<int64_t>(1, trip / chunk);
    if (trip < minParallelTrip() || candidateChunks <= 1) {
        if (std::getenv("XYO_PARFOR_DEBUG"))
            std::fprintf(stderr, "parfor: sequential trip=%lld chunks=1\n",
                         (long long)trip);
        for (int64_t it = 0; it < trip; ++it)
            body(ctx, it, init + double(it) * delta, lists, bases);
        return 1;
    }

    std::unique_lock<std::mutex> lk(parMutex(), std::try_to_lock);
    const int workers = parPool().workers() + 1;
    const int64_t nChunks = std::min<int64_t>(candidateChunks, int64_t(workers) * 2);
    if (!lk.owns_lock() || workers <= 1 || nChunks <= 1) {
        if (std::getenv("XYO_PARFOR_DEBUG"))
            std::fprintf(stderr,
                         "parfor: sequential trip=%lld workers=%d chunks=%lld "
                         "lock=%d\n",
                         (long long)trip, workers, (long long)nChunks,
                         int(lk.owns_lock()));
        for (int64_t it = 0; it < trip; ++it)
            body(ctx, it, init + double(it) * delta, lists, bases);
        return 1;
    }

    if (std::getenv("XYO_PARFOR_DEBUG"))
        std::fprintf(stderr, "parfor: parallel trip=%lld workers=%d chunks=%lld\n",
                     (long long)trip, workers, (long long)nChunks);
    ++xyo_parfor_runs;
    xyo_parfor_iters += trip;

    std::vector<Chunk> chunks;
    chunks.resize(size_t(nChunks));
    std::vector<xyo::WorkerPool::Task> tasks;
    tasks.resize(size_t(nChunks));
    const int64_t per = trip / nChunks;
    for (int64_t c = 0; c < nChunks; ++c) {
        const int64_t lo = c * per;
        const int64_t hi = (c + 1 == nChunks) ? trip : lo + per;
        chunks[size_t(c)] = Chunk{ctx, init, delta, lists, bases, body, lo, hi};
        tasks[size_t(c)] =
            xyo::WorkerPool::Task{&runChunk, &chunks[size_t(c)]};
    }
    parPool().run(tasks.data(), tasks.size());
    return 1;
}

}  // extern "C"
