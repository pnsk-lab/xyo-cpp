// xyo-cpp: UTF-16 文字列操作 + SStr 生成。
#include "sstr.hpp"
#include "cast.hpp"
#include "jsnum.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>
#include <mutex>
#include <new>
#include <limits>

namespace xyo {

struct CasePropRange { uint32_t lo, hi; };
#include "case_props.inc"

// 診断（XYO_STR_DEBUG=1）: cons 生成数と flatten 回数を終了時に stderr へ。
namespace {
struct StrDebug {
    bool on = std::getenv("XYO_STR_DEBUG") != nullptr;
    std::atomic<uint64_t> cons{0}, flat{0};
    ~StrDebug() {
        if (on)
            std::fprintf(stderr, "[str] cons=%llu flatten=%llu\n",
                         (unsigned long long)cons.load(std::memory_order_relaxed),
                         (unsigned long long)flat.load(std::memory_order_relaxed));
    }
};
StrDebug& strDebug() { static StrDebug d; return d; }
} // namespace


// Experimental append-only log. Each immutable SStr snapshot holds (log, prefix
// length). Later appends only write *past* that prefix; a fork from an old
// snapshot allocates a new log. Fixed chunks avoid invalidating existing bytes
// when the vector grows, and preserve UTF-16 code-unit indexing.
struct AppendLog {
    static constexpr uint32_t kChunk = 256;

    // Raw chunks and packed repeated literals share one ordered sequence.
    // A repeated literal is kept as (pattern, length) instead of allocating
    // 512 bytes for every 256 UTF-16 units produced by a JIT batch.
    struct Segment {
        uint32_t end = 0;      // exclusive end in the logical UTF-16 string
        uint32_t size = 0;
        uint64_t packed = 0;   // up to four UTF-16 code units
        uint8_t period = 0;    // 0 = raw chunk; otherwise repeated literal
        std::unique_ptr<char16_t[]> raw;
    };

    std::mutex mutex;
    std::vector<Segment> segments;
    uint32_t length = 0;
    bool ascii = true;

    void append(std::u16string_view text) { // lock must be held
        size_t pos = 0;
        while (pos < text.size()) {
            if (length == UINT32_MAX) std::abort();
            if (segments.empty() || segments.back().period ||
                segments.back().size == kChunk) {
                Segment segment;
                segment.raw = std::make_unique<char16_t[]>(kChunk);
                segments.push_back(std::move(segment));
                Gc::get().noteExternalAllocation(kChunk * sizeof(char16_t));
            }
            Segment& segment = segments.back();
            const size_t n = std::min<size_t>(
                {size_t(kChunk - segment.size), text.size() - pos,
                 size_t(UINT32_MAX - length)});
            if (!n) std::abort();
            const char16_t* src = text.data() + pos;
            std::memcpy(segment.raw.get() + segment.size, src,
                        n * sizeof(char16_t));
            if (ascii)
                for (size_t i = 0; i < n; ++i)
                    if (src[i] >= 128) { ascii = false; break; }
            segment.size += static_cast<uint32_t>(n);
            length += static_cast<uint32_t>(n);
            segment.end = length;
            pos += n;
        }
    }

    void appendAscii(std::string_view text) { // lock must be held
        size_t pos = 0;
        while (pos < text.size()) {
            if (length == UINT32_MAX) std::abort();
            if (segments.empty() || segments.back().period ||
                segments.back().size == kChunk) {
                Segment segment;
                segment.raw = std::make_unique<char16_t[]>(kChunk);
                segments.push_back(std::move(segment));
                Gc::get().noteExternalAllocation(kChunk * sizeof(char16_t));
            }
            Segment& segment = segments.back();
            const size_t n = std::min<size_t>(
                {size_t(kChunk - segment.size), text.size() - pos,
                 size_t(UINT32_MAX - length)});
            if (!n) std::abort();
            for (size_t i = 0; i < n; ++i)
                segment.raw[segment.size + i] =
                    static_cast<unsigned char>(text[pos + i]);
            segment.size += static_cast<uint32_t>(n);
            length += static_cast<uint32_t>(n);
            segment.end = length;
            pos += n;
        }
    }

    void appendPackedRepeat(uint64_t packed, uint32_t period,
                            uint32_t repeats) { // lock must be held
        if (!period || period > 4 || !repeats || repeats > 64)
            std::abort();
        const uint64_t units = uint64_t(period) * repeats;
        if (units > UINT32_MAX - length) std::abort();
        if (ascii)
            for (uint32_t i = 0; i < period; ++i)
                if (((packed >> (16 * i)) & 0xffffu) >= 128) {
                    ascii = false;
                    break;
                }
        // Adjacent batches of the same literal coalesce to a single segment.
        // Existing prefix snapshots stay immutable: only bytes beyond their
        // length are logically appended.
        if (!segments.empty() && segments.back().period == period &&
            segments.back().packed == packed) {
            Segment& tail = segments.back();
            tail.size += uint32_t(units);
            length += uint32_t(units);
            tail.end = length;
            return;
        }
        Segment segment;
        segment.period = static_cast<uint8_t>(period);
        segment.packed = packed;
        segment.size = static_cast<uint32_t>(units);
        length += segment.size;
        segment.end = length;
        segments.push_back(std::move(segment));
        Gc::get().noteExternalAllocation(sizeof(Segment));
    }

    void copy(char16_t* out, uint32_t n) const { // lock must be held
        uint32_t pos = 0;
        for (const Segment& segment : segments) {
            if (pos >= n) break;
            const uint32_t take = std::min(segment.size, n - pos);
            if (!segment.period) {
                std::memcpy(out + pos, segment.raw.get(),
                            size_t(take) * sizeof(char16_t));
            } else {
                for (uint32_t i = 0; i < take; ++i)
                    out[pos + i] = static_cast<char16_t>(
                        (segment.packed >> (16 * (i % segment.period))) & 0xffffu);
            }
            pos += take;
        }
    }

    char16_t unitAt(uint32_t i) const { // lock must be held
        const auto it = std::lower_bound(
            segments.begin(), segments.end(), i,
            [](const Segment& segment, uint32_t pos) {
                return segment.end <= pos;
            });
        if (it == segments.end()) std::abort();
        const uint32_t start = it->end - it->size;
        const uint32_t offset = i - start;
        if (it->period)
            return static_cast<char16_t>(
                (it->packed >> (16 * (offset % it->period))) & 0xffffu);
        return it->raw[offset];
    }

    size_t externalBytes() const {
        size_t bytes = segments.capacity() * sizeof(Segment);
        for (const Segment& segment : segments)
            if (!segment.period) bytes += kChunk * sizeof(char16_t);
        return bytes;
    }
};
void destroyAppendLog(void* ptr) {
    static_cast<AppendLog*>(ptr)->~AppendLog();
}
size_t appendLogExternalBytes(void* ptr) {
    auto* log = static_cast<AppendLog*>(ptr);
    // GC may read accounting while another worker is appending chunks.
    std::lock_guard lock(log->mutex);
    return log->externalBytes();
}

const char16_t* SStr::appendBody() const {
    auto* self = const_cast<SStr*>(this);
    if (auto* ready = std::atomic_ref<char16_t*>(self->body)
                          .load(std::memory_order_acquire))
        return ready;
    const size_t n = len;
    auto* buf = static_cast<char16_t*>(std::malloc((n + 1) * sizeof(char16_t)));
    if (!buf) std::abort();
    auto* log = reinterpret_cast<AppendLog*>(left);
    {
        std::lock_guard lock(log->mutex);
        log->copy(buf, len);
    }
    buf[n] = 0;
    char16_t* expected = nullptr;
    if (!std::atomic_ref<char16_t*>(self->body).compare_exchange_strong(
            expected, buf, std::memory_order_acq_rel, std::memory_order_acquire)) {
        std::free(buf);
        return expected;
    }
    return buf;
}

// Important: allocation can trigger GC, so hold the collector's deferral guard
// while constructing the log and publishing the snapshot.
static SStr* makeAppendSnapshot(AppendLog* log, uint32_t length, bool ascii) {
    auto* result = static_cast<SStr*>(Gc::get().alloc(GcType::Str, sizeof(SStr)));
    result->len = length;
    result->hash = 0;
    result->initCaches();
    result->flags = SStr::kAppendFlag | SStr::kAsciiKnown |
                    (ascii ? SStr::kAsciiValue : 0);
    result->left = reinterpret_cast<SStr*>(log); // GC-owned AppendLog pointer
    return result;
}

SStr* sstrConsOrAppend(SStr* a, SStr* b) {
    static const bool enabled = [] {
        const char* v = std::getenv("XYO_STRING_APPEND_LOG");
        return v && v[0] == '1';
    }();
    if (!enabled || !a || !b || b->len > 256 ||
        size_t(a->len) + b->len > std::numeric_limits<uint32_t>::max())
        return sstrCons(a, b);

    // Extract RHS before locking a log (RHS might refer to that same log).
    // The small append limit bounds temporary copying.
    std::u16string rhs(b->view());
    const bool ascii = a->isAscii() && b->isAscii();
    Gc::DeferCollect defer;
    if (a->isAppend()) {
        auto* log = reinterpret_cast<AppendLog*>(a->left);
        std::lock_guard lock(log->mutex);
        if (log->length == a->len) {
            log->append(rhs);
            return makeAppendSnapshot(log, log->length, ascii);
        }
        // The old prefix has since been extended; fork to preserve both values.
    }
    auto* log = new (Gc::get().alloc(GcType::AppendLog, sizeof(AppendLog)))
        AppendLog();
    const auto prefix = a->view();
    {
        std::lock_guard lock(log->mutex);
        log->append(prefix);
        log->append(rhs);
    }
    return makeAppendSnapshot(log, log->length, ascii);
}

// These helpers are only emitted for a structurally proven single-variable
// self-append loop. No observer can see the intermediate value until a Scratch
// yield. In the JIT, a transient is a GC-owned AppendLog rooted through an
// immutable prefix snapshot stored in the variable. Only a real yield or loop
// exit publishes the grown prefix. The log's chunks never modify old indices.
extern "C" {

void* xyo_transient_begin(uint64_t initial, uint64_t* variable) {
    Gc::DeferCollect defer;
    const Value current{initial};
    SStr* source = cast::toString(current);
    auto* log = new (Gc::get().alloc(GcType::AppendLog, sizeof(AppendLog)))
        AppendLog();
    if (source && source->len) {
        std::lock_guard lock(log->mutex);
        log->append(source->view());
    }
    SStr* root = makeAppendSnapshot(log, log->length, log->ascii);
    *variable = Value::string(root).bits;
    return log;
}

void xyo_transient_append(void* pointer, uint64_t suffixBits) {
    auto* log = static_cast<AppendLog*>(pointer);
    const Value suffix{suffixBits};
    SStr* text = suffix.asStrPtr(); // the JIT proved an immutable text literal
    if (!text || !text->len) return;
    const auto characters = text->view();
    std::lock_guard lock(log->mutex);
    log->append(characters);
}

// JIT has proved RHS is a side-effect-free reporter independent of the
// destination variable. This handles dynamic arithmetic and numeric inputs
// while preserving JS number-to-string formatting and UTF-16 semantics.
void xyo_transient_append_value(void* pointer, uint64_t bits) {
    auto* log = static_cast<AppendLog*>(pointer);
    const Value value{bits};
    if (value.isNumber()) {
        const double d = value.asNumberUnchecked();
        // Small integral values dominate Scratch variable arithmetic. Format
        // directly into stack UTF-16 units, avoiding an intermediate std::string
        // or SStr allocation. Casting only after proving a safe finite range
        // also preserves NaN, infinity, exponent and -0 formatting in the
        // general JavaScript number-to-string path.
        if (d >= -999999999.0 && d <= 999999999.0) {
            const int32_t n = static_cast<int32_t>(d);
            if (double(n) == d) {
                char16_t digits[12];
                unsigned len = 0;
                uint32_t magnitude = static_cast<uint32_t>(n < 0 ? -n : n);
                do {
                    digits[len++] = char16_t(u'0' + (magnitude % 10));
                    magnitude /= 10;
                } while (magnitude);
                if (n < 0) digits[len++] = u'-';
                std::reverse(digits, digits + len);
                std::lock_guard lock(log->mutex);
                log->append(std::u16string_view(digits, len));
                return;
            }
        }
        const std::string ascii = jsnum::toString(d);
        std::lock_guard lock(log->mutex);
        log->appendAscii(ascii);
        return;
    }
    SStr* str = cast::toString(value);
    const auto characters = str->view();
    std::lock_guard lock(log->mutex);
    log->append(characters);
}

// Specialized for compile-time literals up to 4 UTF-16 code units: avoids
// dereferencing an SStr and calling view()/data() on every loop iteration.
void xyo_transient_append_packed(void* pointer, uint64_t packed, uint32_t length) {
    if (length == 0 || length > 4) std::abort();
    char16_t units[4];
    for (uint32_t i = 0; i < length; ++i)
        units[i] = char16_t((packed >> (16 * i)) & 0xffff);
    auto* log = static_cast<AppendLog*>(pointer);
    std::lock_guard lock(log->mutex);
    log->append(std::u16string_view(units, length));
}

// Repeated literal appends share one FFI call and mutex acquisition for up to
// 64 logical warp iterations. The JIT only groups iterations between ordinary
// 64-iteration yield checkpoints; it advances the same logical poll counter.
void xyo_transient_append_repeat_packed(void* pointer, uint64_t packed,
                                        uint64_t metadata) {
    const uint32_t period = uint32_t(metadata >> 32);
    const uint32_t repeats = uint32_t(metadata);
    auto* log = static_cast<AppendLog*>(pointer);
    std::lock_guard lock(log->mutex);
    log->appendPackedRepeat(packed, period, repeats);
}

// Batch arbitrary constant UTF-16 text without paying for a lock or SStr
// lookup on each logical iteration. The JIT keeps normal warp checkpoints.
void xyo_transient_append_repeat_string(void* pointer, uint64_t suffixBits,
                                        uint32_t repeats) {
    if (repeats == 0 || repeats > 64) std::abort();
    const Value suffix{suffixBits};
    SStr* text = suffix.asStrPtr();
    if (!text || !text->len) return;
    const auto characters = text->view();
    auto* log = static_cast<AppendLog*>(pointer);
    std::lock_guard lock(log->mutex);
    for (uint32_t i = 0; i < repeats; ++i)
        log->append(characters);
}

// When the loop body only appends the value of a pure reporter, evaluate
// that reporter once per 64-iteration scheduling group. The JIT re-evaluates
// it after each possible yield, so writes by other Scratch scripts are seen
// at the same checkpoints as the scalar implementation.
void xyo_transient_append_repeat_value(void* pointer, uint64_t bits,
                                       uint32_t repeats) {
    if (repeats == 0 || repeats > 64) std::abort();
    const Value value{bits};
    SStr* str = cast::toString(value);
    if (!str || !str->len) return;
    const auto data = str->view();
    auto* log = static_cast<AppendLog*>(pointer);
    std::lock_guard lock(log->mutex);
    for (uint32_t i = 0; i < repeats; ++i)
        log->append(data);
}

void xyo_transient_flush(void* pointer, uint64_t* variable) {
    auto* log = static_cast<AppendLog*>(pointer);
    // The preceding published snapshot keeps the log rooted while allocating.
    Gc::DeferCollect defer;
    std::lock_guard lock(log->mutex);
    const Value prior{*variable};
    if (prior.isString()) {
        SStr* s = prior.asStrPtr();
        if (s && s->isAppend() &&
            reinterpret_cast<AppendLog*>(s->left) == log && s->len == log->length)
            return;
    }
    *variable = Value::string(makeAppendSnapshot(log, log->length, log->ascii)).bits;
}

// End-of-loop publication: short strings become flat, restoring the JIT's
// inline letter-of path and avoiding a per-character append-log mutex.
void xyo_transient_finish(void* pointer, uint64_t* variable) {
    auto* log = static_cast<AppendLog*>(pointer);
    if (log->length > 4096) {
        xyo_transient_flush(pointer, variable);
        return;
    }
    Gc::DeferCollect defer;
    std::lock_guard lock(log->mutex);
    const auto n = log->length;
    auto* result = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str, sizeof(SStr) + (size_t(n) + 1) * 2));
    result->len = n;
    result->hash = 0;
    result->initCaches();
    result->flags = SStr::kAsciiKnown | (log->ascii ? SStr::kAsciiValue : 0);
    log->copy(result->inlineBody(), n);
    result->inlineBody()[n] = 0;
    *variable = Value::string(result).bits;
}

// A yield is an observer/GC boundary. Recover the log exclusively through the
// *current* immutable snapshot, which roots it. Never dereference oldLog: the
// suspended frame's raw pointer may have become stale when another script
// overwrote the variable and GC reclaimed the old log. If the current prefix
// is not the tail, fork so that old snapshots remain immutable.
void* xyo_transient_resume(void* /*oldLog*/, uint64_t* variable) {
    const Value observed{*variable};
    if (observed.isString()) {
        SStr* snapshot = observed.asStrPtr();
        if (snapshot && snapshot->isAppend()) {
            auto* rootedLog = reinterpret_cast<AppendLog*>(snapshot->left);
            std::lock_guard lock(rootedLog->mutex);
            if (snapshot->len == rootedLog->length)
                return rootedLog;
        }
    }
    return xyo_transient_begin(observed.bits, variable);
}

} // extern "C"

// --- 連結ノード（cons string） -------------------------------------
//
// Scratch の文字列は不変なので `s = s ++ "ab"` を素直に実装すると
// 反復ごとに全体をコピーする O(n²) になる（TurboWarp/V8 は cons string で
// O(1) 連結 → 遅延 flatten）。ここでも同じ表現を持ち、**本文が初めて
// 必要になったときだけ** materialize する。
//
// レイアウト: flat は構造体直後に char16_t data[len+1]。cons は
// left/right を持ち、body に flatten 結果（malloc）を遅延 publish する。
// GC は cons の子を辿り（gc.cpp）、sweep で body を free する。

const char16_t* SStr::consBody() const {
    auto* self = const_cast<SStr*>(this);
    if (char16_t* done = std::atomic_ref<char16_t*>(self->body)
                             .load(std::memory_order_acquire))
        return done;
    if (strDebug().on) ++strDebug().flat;
    const size_t n = self->len;
    auto* buf = static_cast<char16_t*>(
        std::malloc((n + 1) * sizeof(char16_t)));
    if (!buf) return reinterpret_cast<const char16_t*>(u"");   // OOM

    // 反復 DFS で左から順に書き込む（深い連鎖で再帰スタックを溢れさせない）。
    std::vector<const SStr*> stack;
    stack.reserve(64);
    stack.push_back(self);
    size_t at = 0;
    while (!stack.empty()) {
        const SStr* s = stack.back();
        stack.pop_back();
        if (s->isAppend()) {
            std::memcpy(buf + at, s->data(), size_t(s->len) * sizeof(char16_t));
            at += s->len;
            continue;
        }
        if (s->isCons()) {
            // 既に materialize 済みならそれを丸ごとコピー（部分木を辿らない）
            if (char16_t* pb = std::atomic_ref<char16_t*>(
                    const_cast<SStr*>(s)->body)
                                   .load(std::memory_order_acquire)) {
                std::memcpy(buf + at, pb, size_t(s->len) * sizeof(char16_t));
                at += s->len;
                continue;
            }
            stack.push_back(s->right);   // 後入れ先出し → 左を先に処理
            stack.push_back(s->left);
            continue;
        }
        std::memcpy(buf + at, s->inlineBody(),
                    size_t(s->len) * sizeof(char16_t));
        at += s->len;
    }
    buf[n] = 0;
    if (at != n) {
        // 不変条件（len == 子の len 合計）が壊れている — 破棄して空を返す。
        std::free(buf);
        return reinterpret_cast<const char16_t*>(u"");
    }
    // 他スレッドと競合したら負けた側を捨てる（内容は同じ）。
    char16_t* expected = nullptr;
    if (!std::atomic_ref<char16_t*>(self->body).compare_exchange_strong(
            expected, buf, std::memory_order_acq_rel, std::memory_order_acquire)) {
        std::free(buf);
        return expected;
    }
    return buf;
}

char16_t SStr::unitAt(uint32_t index) const {
    if (isAppend()) {
        if (auto* flat = std::atomic_ref<char16_t*>(
                const_cast<SStr*>(this)->body).load(std::memory_order_acquire))
            return flat[index];
        auto* log = reinterpret_cast<AppendLog*>(left);
        std::lock_guard lock(log->mutex);
        return log->unitAt(index);
    }
    const uint32_t originalIndex = index;
    const SStr* node = this;
    for (unsigned depth = 0; depth < 32; ++depth) {
        if (node->isAppend()) return node->unitAt(index);
        if (!node->isCons()) return node->inlineBody()[index];
        if (auto* flat = std::atomic_ref<char16_t*>(
                const_cast<SStr*>(node)->body).load(std::memory_order_acquire))
            return flat[index];
        if (index < node->left->len) {
            node = node->left;
        } else {
            index -= node->left->len;
            node = node->right;
        }
    }
    // Cache the original rope, not just a descendant: repeated reads of the
    // same deep rope must not pay the bounded walk on every call.
    return data()[originalIndex];
}

SStr* sstrCons(SStr* a, SStr* b) {
    if (strDebug().on) ++strDebug().cons;
#if defined(XYO_STRING_CONS_POOL)
    auto* s = static_cast<SStr*>(Gc::get().allocCons(sizeof(SStr)));
#else
    auto* s = static_cast<SStr*>(Gc::get().alloc(GcType::Str, sizeof(SStr)));
#endif
    s->len = a->len + b->len;
    s->hash = 0;
    s->initCaches();
    s->flags = SStr::kConsFlag;
    s->left = a;
    s->right = b;
    return s;
}

// 生成テーブル（BMP 小写化・大写化）
#include "lower_table.inc"
#include "upper_table.inc"

static bool inRanges(const CasePropRange* r, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].lo) hi = mid;
        else if (cp > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool isCased(uint32_t cp) {
    return inRanges(kCasedRanges, std::size(kCasedRanges), cp);
}
static bool isCaseIgnorable(uint32_t cp) {
    return inRanges(kCaseIgnorableRanges, std::size(kCaseIgnorableRanges), cp);
}

SStr* sstrCreate(std::u16string_view sv) {
    return SStr::create(sv);
}

SStr* sstrFromAscii(std::string_view sv) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str, sizeof(SStr) + (sv.size() + 1) * 2));
    s->len = static_cast<uint32_t>(sv.size());
    s->hash = 0;
    s->initCaches();
    s->flags = SStr::kAsciiKnown | SStr::kAsciiValue;   // ASCII by construction
    for (size_t i = 0; i < sv.size(); ++i) s->data()[i] = static_cast<char16_t>(sv[i]);
    s->data()[sv.size()] = 0;
    return s;
}

SStr* sstrConcat(std::u16string_view a, std::u16string_view b) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str,
                        sizeof(SStr) + (a.size() + b.size() + 1) * 2));
    s->len = static_cast<uint32_t>(a.size() + b.size());
    s->hash = 0;
    s->initCaches();
    std::memcpy(s->data(), a.data(), a.size() * 2);
    std::memcpy(s->data() + a.size(), b.data(), b.size() * 2);
    s->data()[s->len] = 0;
    return s;
}

SStr* sstrConcatAscii(std::u16string_view a, std::string_view b) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str,
                        sizeof(SStr) + (a.size() + b.size() + 1) * 2));
    s->len = static_cast<uint32_t>(a.size() + b.size());
    s->hash = 0;
    s->initCaches();
    std::memcpy(s->data(), a.data(), a.size() * 2);
    for (size_t i = 0; i < b.size(); ++i)
        s->data()[a.size() + i] = static_cast<char16_t>(b[i]);
    s->data()[s->len] = 0;
    return s;
}

// --- 小写化 ---

/// c (BMP char16_t) の小写化。多くは 1 文字→1 文字。
/// U+0130 のみ 2 文字展開（'i' + U+0307）で *extra に第 2 文字を返す。
/// 戻り値: 第 1 文字。
char16_t lowerUnit(char16_t c, char16_t* extra) {
    *extra = 0;
    if (c >= u'A' && c <= u'Z') return c + 32;
    if (c < 0x80) return c;
    if (c == 0x0130) { *extra = 0x0307; return u'i'; }
    // レンジテーブル（二分探索するほど大きくないので線形でもよいが…29件なので線形）
    for (const auto& r : kLowerRanges) {
        if (c >= r.lo && c <= r.hi) return c + r.delta;
    }
    // 単独テーブル: 二分探索（ソート済みとは限らないので毎回線形…645件は重い）
    // ソート済み前提で二分探索
    {
        int lo = 0, hi = static_cast<int>(std::size(kLowerSingles)) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (kLowerSingles[mid].cp == c) return kLowerSingles[mid].lo;
            if (kLowerSingles[mid].cp < c) lo = mid + 1; else hi = mid - 1;
        }
    }
    return c;
}

char32_t lowerCp(uint32_t cp, char16_t* extra) {
    *extra = 0;
    if (cp <= 0xFFFF) return lowerUnit(static_cast<char16_t>(cp), extra);
    for (const auto& r : kLowerAstralRanges) {
        if (cp >= r.lo && cp <= r.hi) return cp + r.delta;
    }
    return cp;
}

static void appendCp(std::u16string& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        const uint32_t v = cp - 0x10000u;
        out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    }
}

std::u16string u16ToLower(std::u16string_view sv) {
    // JS String.prototype.toLowerCase はコードポイント単位で畳み込み、
    // Greek capital sigma は語末だけ final sigma (U+03C2) になる
    // （Unicode Final_Sigma: 直前が Cased、直後が Cased でない）。
    bool ascii = true;
    for (char16_t c : sv) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return u16ToLowerAscii(sv);

    std::vector<uint32_t> cps;
    cps.reserve(sv.size());
    for (size_t i = 0; i < sv.size();) {
        const char16_t c = sv[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < sv.size()) {
            const char16_t d = sv[i + 1];
            if (d >= 0xDC00 && d <= 0xDFFF) {
                cps.push_back(0x10000u +
                    (static_cast<uint32_t>(c - 0xD800) << 10) +
                    static_cast<uint32_t>(d - 0xDC00));
                i += 2;
                continue;
            }
        }
        cps.push_back(c);
        ++i;
    }

    std::u16string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t cp = cps[i];
        char16_t extra = 0;
        uint32_t l = lowerCp(cp, &extra);
        if (cp == 0x03A3) {
            bool casedBefore = false, casedAfter = false;
            for (size_t j = i; j-- > 0;) {
                if (isCaseIgnorable(cps[j])) continue;
                casedBefore = isCased(cps[j]);
                break;
            }
            for (size_t j = i + 1; j < cps.size(); ++j) {
                if (isCaseIgnorable(cps[j])) continue;
                casedAfter = isCased(cps[j]);
                break;
            }
            if (casedBefore && !casedAfter) l = 0x03C2;  // ς
        }
        appendCp(out, l);
        if (extra) out.push_back(extra);
    }
    return out;
}

std::u16string u16ToLowerAscii(std::u16string_view sv) {
    std::u16string out;
    out.reserve(sv.size());
    for (char16_t c : sv) {
        out.push_back((c >= u'A' && c <= u'Z') ? c + 32 : c);
    }
    return out;
}

bool SStr::isAscii() const {
    auto loadFlags = [](const SStr* s) {
        return std::atomic_ref<uint32_t>(const_cast<SStr*>(s)->flags)
            .load(std::memory_order_acquire);
    };
    auto publish = [](const SStr* s, bool ascii) {
        std::atomic_ref<uint32_t>(const_cast<SStr*>(s)->flags)
            .fetch_or(kAsciiKnown | (ascii ? kAsciiValue : 0),
                      std::memory_order_release);
    };

    uint32_t f = loadFlags(this);
    if (f & kAsciiKnown) return (f & kAsciiValue) != 0;

    // The usual case is a new cons whose children were already examined.
    // Avoid a temporary stack (and a heap allocation) for this fast path.
    if (f & kConsFlag) {
        const uint32_t lf = loadFlags(left);
        if (lf & kAsciiKnown) {
            if (!(lf & kAsciiValue)) {
                publish(this, false);
                return false;
            }
            const uint32_t rf = loadFlags(right);
            if (rf & kAsciiKnown) {
                const bool ascii = (rf & kAsciiValue) != 0;
                publish(this, ascii);
                return ascii;
            }
        }
    }

    // An unflattened rope can be tens of thousands of nodes deep. Resolve
    // children in postorder, caching each result without native recursion.
    std::vector<const SStr*> pending{this};
    while (!pending.empty()) {
        const SStr* node = pending.back();
        f = loadFlags(node);
        if (f & kAsciiKnown) {
            pending.pop_back();
        } else if (!(f & kConsFlag)) {
            bool ascii = true;
            const char16_t* d = node->inlineBody();
            for (uint32_t i = 0; i < node->len; ++i) {
                if (d[i] >= 0x80) { ascii = false; break; }
            }
            publish(node, ascii);
            pending.pop_back();
        } else {
            const uint32_t lf = loadFlags(node->left);
            if (!(lf & kAsciiKnown)) {
                pending.push_back(node->left);
                continue;
            }
            if (!(lf & kAsciiValue)) {
                publish(node, false); // right child need not be scanned
                pending.pop_back();
                continue;
            }
            const uint32_t rf = loadFlags(node->right);
            if (!(rf & kAsciiKnown)) {
                pending.push_back(node->right);
                continue;
            }
            publish(node, (rf & kAsciiValue) != 0);
            pending.pop_back();
        }
    }
    return (loadFlags(this) & kAsciiValue) != 0;
}

static bool isAsciiOnly(std::u16string_view s) {
    for (char16_t c : s) if (c >= 0x80) return false;
    return true;
}

static int compareLoweredViews(std::u16string_view a, std::u16string_view b);
static int compareAsciiCaseInsensitive(std::u16string_view a,
                                       std::u16string_view b);

int compareStrings(std::u16string_view a, std::u16string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

int compareStringsCaseInsensitive(std::u16string_view a, std::u16string_view b) {
    // ASCII 高速パス
    if (isAsciiOnly(a) && isAsciiOnly(b)) return compareAsciiCaseInsensitive(a, b);
    // Avoid allocating two temporary strings for the common non-ASCII
    // comparison. Lowercase expansion (U+0130) is handled as a virtual unit.
    return compareLoweredViews(a, b);
}

int compareStringsCaseInsensitive(const SStr* a, const SStr* b) {
    // The ASCII property is cached on the string, so repeated comparisons of
    // the same operands do not rescan them before every compare.
    if (a->isAscii() && b->isAscii())
        return compareAsciiCaseInsensitive(a->view(), b->view());
    return compareStringsCaseInsensitive(a->view(), b->view());
}

bool sstrLetterAt(std::u16string_view s, int64_t idx1, char16_t& out) {
    if (idx1 < 1 || static_cast<uint64_t>(idx1) > s.size()) return false;
    out = s[static_cast<size_t>(idx1 - 1)];
    return true;
}

static constexpr char16_t foldAscii(char16_t c) {
    return (c >= u'A' && c <= u'Z') ? char16_t(c + 32) : c;
}

// Boyer-Moore-Horspool over case-folded ASCII. Scratch contains checks often
// search a short word near the end of a long string; skipping by the needle
// length avoids comparing every candidate position in that workload.
static bool containsAscii(std::u16string_view hay, std::u16string_view needle) {
    const size_t n = needle.size();
    if (n == 0) return true;
    if (n == 1) {
        const char16_t wanted = foldAscii(needle[0]);
        const char16_t* p = hay.data();
        const char16_t* end = p + hay.size();
        while (p < end) if (foldAscii(*p++) == wanted) return true;
        return false;
    }
    // Short needles are cheaper with the straightforward scan; constructing
    // a skip table costs more than it saves for the common two/three chars.
    if (n < 4) {
        const char16_t first = foldAscii(needle[0]);
        const size_t last = hay.size() - n;
        for (size_t i = 0; i <= last; ++i) {
            if (foldAscii(hay[i]) != first) continue;
            size_t j = 1;
            for (; j < n; ++j)
                if (foldAscii(hay[i + j]) != foldAscii(needle[j])) break;
            if (j == n) return true;
        }
        return false;
    }
    size_t skip[128];
    for (size_t i = 0; i < std::size(skip); ++i) skip[i] = n;
    for (size_t i = 0; i + 1 < n; ++i)
        skip[foldAscii(needle[i])] = n - 1 - i;

    size_t pos = 0;
    while (pos <= hay.size() - n) {
        size_t j = n;
        while (j && foldAscii(hay[pos + j - 1]) == foldAscii(needle[j - 1])) --j;
        if (j == 0) return true;
        pos += skip[foldAscii(hay[pos + n - 1])];
    }
    return false;
}

static int compareLoweredViews(std::u16string_view a, std::u16string_view b) {
    // Unicode case mappings can expand (e.g. U+0130 → i + combining dot) or
    // map astral code points to astral code points. Decode one code point at a
    // time and compare the folded UTF-16 units as streams, without allocating.
    size_t ia = 0, ib = 0;
    uint32_t remA = 0, remB = 0;
    char16_t pendingA = 0, pendingB = 0;
    auto next = [](std::u16string_view s, size_t& i, uint32_t& rem,
                   char16_t& pending, char16_t& out) {
        if (pending) { out = pending; pending = 0; return true; }
        if (rem) {
            const uint32_t cp = rem; rem = 0;
            out = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            return true;
        }
        if (i >= s.size()) return false;
        uint32_t cp = s[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && i < s.size() &&
            s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i++] - 0xDC00);
        }
        char16_t extra = 0;
        uint32_t lo = lowerCp(cp, &extra);
        // String comparison's lowercasing is context-sensitive for capital
        // sigma. The operands here are compared as complete strings, so check
        // the preceding/following cased context for the current code point.
        if (cp == 0x03A3) {
            bool before = false, after = false;
            for (size_t j = i - 1; j-- > 0;) {
                uint32_t prior = s[j];
                if (prior >= 0xDC00 && prior <= 0xDFFF && j > 0 &&
                    s[j - 1] >= 0xD800 && s[j - 1] <= 0xDBFF) {
                    prior = 0x10000 + ((s[j - 1] - 0xD800) << 10) +
                            (s[j] - 0xDC00);
                    --j;
                }
                if (isCaseIgnorable(prior)) continue;
                before = isCased(prior); break;
            }
            for (size_t j = i; j < s.size(); ++j) {
                uint32_t following = s[j];
                if (following >= 0xD800 && following <= 0xDBFF && j + 1 < s.size() &&
                    s[j + 1] >= 0xDC00 && s[j + 1] <= 0xDFFF) {
                    following = 0x10000 + ((s[j] - 0xD800) << 10) +
                                (s[++j] - 0xDC00);
                }
                if (isCaseIgnorable(following)) continue;
                after = isCased(following); break;
            }
            if (before && !after) lo = 0x03C2;
        }
        if (lo > 0xFFFF) {
            const uint32_t v = lo - 0x10000;
            out = static_cast<char16_t>(0xD800 + (v >> 10));
            rem = lo;
        } else {
            out = static_cast<char16_t>(lo);
        }
        pending = extra;
        return true;
    };
    for (;;) {
        char16_t ca = 0, cb = 0;
        const bool ha = next(a, ia, remA, pendingA, ca);
        const bool hb = next(b, ib, remB, pendingB, cb);
        if (!ha || !hb) return ha == hb ? 0 : (ha ? 1 : -1);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
}

static int compareAsciiCaseInsensitive(std::u16string_view a,
                                       std::u16string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        char16_t ca = foldAscii(a[i]), cb = foldAscii(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool sstrContains(std::u16string_view hay, std::u16string_view needle) {
    if (needle.empty()) return true;
    // Unicode lowercasing can expand code units (U+0130 -> i + U+0307),
    // so source lengths cannot reject a Unicode match.
    // ASCII だけなら小文字化用の u16string を 2 本作らず、その場で
    // 畳みながら走査する（`contains(長い文字列, 短い語)` はループ内で
    // 呼ばれやすく、毎回 2 本のコピーは無駄 — TurboWarp の 3 倍差の主因）。
    bool ascii = true;
    for (char16_t c : hay) if (c >= 0x80) { ascii = false; break; }
    if (ascii)
        for (char16_t c : needle) if (c >= 0x80) { ascii = false; break; }
    if (ascii) {
        if (needle.size() > hay.size()) return false;
        return containsAscii(hay, needle);
    }
    // Unicode case folding can expand characters and applies Final_Sigma
    // contextually. Keep the established exact path for those uncommon inputs.
    std::u16string h = u16ToLower(hay);
    std::u16string n = u16ToLower(needle);
    return h.find(n) != std::u16string::npos;
}

bool sstrContains(const SStr* hay, const SStr* needle) {
    if (needle->len == 0) return true;
    // Do not compare unlowercased UTF-16 lengths here: Unicode lowercase
    // expansion can make a shorter haystack contain a longer needle.
    // The ASCII property is cached, so a repeated contains() on the same
    // haystack does not rescan it before the search itself.
    if (hay->isAscii() && needle->isAscii()) {
        if (needle->len > hay->len) return false;
        return containsAscii(hay->view(), needle->view());
    }
    return sstrContains(hay->view(), needle->view());
}

SStr* sstrLower(std::u16string_view sv) {
    return SStr::create(u16ToLower(sv));
}

// --- 大写化（JS toUpperCase 相当、多:1/1:多マッピング対応）---

std::u16string u16ToUpper(std::u16string_view sv) {
    std::u16string out;
    out.reserve(sv.size());
    for (char16_t c : sv) {
        if (c >= u'a' && c <= u'z') { out.push_back(c - 32); continue; }
        if (c < 0x80) { out.push_back(c); continue; }
        int lo = 0, hi = int(std::size(kUpperMap)) - 1;
        bool found = false;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (kUpperMap[mid].cp == c) {
                for (uint8_t i = 0; i < kUpperMap[mid].len; ++i)
                    out.push_back(char16_t(kUpperMap[mid].u[i]));
                found = true;
                break;
            }
            if (kUpperMap[mid].cp < c) lo = mid + 1; else hi = mid - 1;
        }
        if (!found) out.push_back(c);
    }
    return out;
}

// --- UTF-8 相互変換（I/O 境界用。内部は UTF-16）---

static void encodeUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string SStr::toUtf8(std::u16string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        char16_t c = sv[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < sv.size()) {
            char16_t lo = sv[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                encodeUtf8(out, 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
        }
        // 孤立サロゲートは WTF-8 的にそのままエンコード（JS 的に正しい挙動）
        encodeUtf8(out, c);
    }
    return out;
}

SStr* SStr::fromUtf8(std::string_view sv) {
    // UTF-8 → UTF-16（サロゲートペア対応、不正シーケンスは U+FFFD）
    std::u16string tmp;
    tmp.reserve(sv.size());
    for (size_t i = 0; i < sv.size();) {
        unsigned char c = sv[i];
        char32_t cp;
        size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; adv = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; adv = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; adv = 4; }
        else { cp = 0xFFFD; adv = 1; }
        if (i + adv > sv.size()) { cp = 0xFFFD; adv = 1; }
        if (adv > 1) {
            bool ok = true;
            for (size_t j = 1; j < adv; ++j)
                if ((sv[i + j] & 0xC0) != 0x80) { ok = false; break; }
            if (!ok) { cp = 0xFFFD; adv = 1; }
            else {
                for (size_t j = 1; j < adv; ++j)
                    cp = (cp << 6) | (sv[i + j] & 0x3F);
            }
        }
        i += adv;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            tmp.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            tmp.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            tmp.push_back(static_cast<char16_t>(cp));
        }
    }
    return create(tmp);
}

} // namespace xyo
