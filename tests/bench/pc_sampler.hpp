#pragma once

// Bench-only native PC sampling; no runtime/JIT hooks and no setitimer use.
// Enable with XYO_BENCH_SAMPLE=<output prefix> (project mode only).
// Optional XYO_BENCH_SAMPLE_US is a positive integer, default 2000 us.
// Sub-millisecond periods may coalesce on Linux CPU-timer tick accounting.
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace xyo::bench {

// Construct/start and stop on the main thread. One instance at a time.
// The ring has process lifetime, including on early returns. The chosen signal
// is reserved for this bench while sampling: do not send it from other sources.
class PcSampler {
public:
    PcSampler() = default;
    PcSampler(const PcSampler&) = delete;
    PcSampler& operator=(const PcSampler&) = delete;
    ~PcSampler() { stop(); }

    void start() {
        const char* prefix = std::getenv("XYO_BENCH_SAMPLE");
        if (!prefix || !*prefix) return; // No handlers/timers/ring touches when off.
#if defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
        if (started_ || active_) return;
        prefix_ = prefix;
        if (const char* text = std::getenv("XYO_BENCH_SAMPLE_US")) {
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(text, &end, 10);
            if (errno || end == text || *end || value < 1 || value > 1000000000L) {
                std::fprintf(stderr, "PC sampler disabled: invalid XYO_BENCH_SAMPLE_US (1..1000000000)\n");
                return;
            }
            periodUs_ = value;
        }
        signal_ = SIGRTMIN + 6;
        if (signal_ > SIGRTMAX) { fail("no dedicated real-time signal"); return; }
        sigemptyset(&set_);
        sigaddset(&set_, signal_);
        const int maskError = pthread_sigmask(SIG_BLOCK, &set_, &oldMask_);
        if (maskError) { fail("cannot block sampling signal"); return; }
        maskSaved_ = true;
        sigset_t pending;
        if (sigpending(&pending) != 0 || sigismember(&pending, signal_) != 0 ||
            sigismember(&oldMask_, signal_) != 0) {
            fail("sampling signal was blocked or already pending"); restore(); return;
        }
        if (sigaction(signal_, nullptr, &oldAction_) != 0) {
            fail("cannot inspect sampling signal"); restore(); return;
        }
        if (oldAction_.sa_handler != SIG_DFL && oldAction_.sa_handler != SIG_IGN) {
            fail("sampling signal already has an owner"); restore(); return;
        }
        count_ = 0;
        overruns_ = 0;
        dropped_ = 0;
        // Prefault the ring outside the benchmark interval (8 MB of static BSS).
        for (int i = 0; i < capacity_; ++i)
            pcs_[i].store(0, std::memory_order_relaxed);
        struct sigaction action{};
        action.sa_sigaction = &handle;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask); // This signal auto-blocks itself in handle.
        if (sigaction(signal_, &action, nullptr) != 0) {
            fail("cannot install sampling handler"); restore(); return;
        }
        actionSaved_ = true;
        struct sigevent event{};
        event.sigev_notify = SIGEV_THREAD_ID;
        event.sigev_signo = signal_;
        event.sigev_value.sival_ptr = &cookie_;
        // Linux libc exposes the kernel target TID in this sigevent union.
        event._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
        tid_ = event._sigev_un._tid;
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer_) != 0) {
            fail("timer_create(CLOCK_THREAD_CPUTIME_ID) failed"); restore(); return;
        }
        timerCreated_ = true;
        struct itimerspec interval{};
        interval.it_interval.tv_sec = periodUs_ / 1000000;
        interval.it_interval.tv_nsec = (periodUs_ % 1000000) * 1000;
        interval.it_value = interval.it_interval;
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuStart_) != 0) {
            fail("cannot read main-thread CPU clock"); stop(); return;
        }
        active_ = 1;
        if (timer_settime(timer_, 0, &interval, nullptr) != 0) {
            fail("cannot arm sampling timer"); stop(); return;
        }
        started_ = true;
        pthread_sigmask(SIG_SETMASK, &oldMask_, nullptr);
#else
        std::fprintf(stderr, "PC sampler disabled: requires native Linux x86_64\n");
#endif
    }

    // Call after saving the benchmark's end timestamps, before any output.
    // No file I/O here; the destructor also safely handles early loop exits.
    void stop() {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
        if (!maskSaved_) return;
        pthread_sigmask(SIG_BLOCK, &set_, nullptr);
        active_ = 0; // Never let a late delivery record or access instance data.
        if (started_) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuEnd_);
        if (timerCreated_) {
            struct itimerspec zero{};
            timer_settime(timer_, 0, &zero, nullptr);
            if (timer_delete(timer_) != 0) fail("timer_delete failed");
            timerCreated_ = false;
            // A POSIX timer has at most one queued signal. No waiting, spinning,
            // or draining other signals; start refused preexisting pending ones.
            struct timespec noWait{};
            siginfo_t info{};
            // Retry only an interruption by an unrelated handler, never poll
            // for a future delivery. An empty queue returns EAGAIN immediately.
            int result;
            do { result = sigtimedwait(&set_, &info, &noWait); }
            while (result < 0 && errno == EINTR);
        }
        restore();
#endif
    }

    void write() const {
#if defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
        if (!started_ || maskSaved_) return;
        const int count = count_;
        const double cpuMs = double(cpuEnd_.tv_sec - cpuStart_.tv_sec) * 1000.0 +
            double(cpuEnd_.tv_nsec - cpuStart_.tv_nsec) / 1e6;
        const std::string pcsPath = prefix_ + ".pcs";
        FILE* out = std::fopen(pcsPath.c_str(), "wb");
        bool pcsOk = false;
        if (out) {
            pcsOk = true;
            std::uintptr_t chunk[1024];
            for (int begin = 0; begin < count; begin += 1024) {
                const int n = count - begin < 1024 ? count - begin : 1024;
                for (int i = 0; i < n; ++i)
                    chunk[i] = pcs_[begin + i].load(std::memory_order_relaxed);
                if (std::fwrite(chunk, sizeof(chunk[0]), size_t(n), out) != size_t(n)) {
                    pcsOk = false;
                    break;
                }
            }
            if (std::fclose(out) != 0) pcsOk = false;
        }
        const std::string mapsPath = prefix_ + ".maps";
        FILE* maps = std::fopen("/proc/self/maps", "rb");
        out = std::fopen(mapsPath.c_str(), "wb");
        bool mapsOk = maps && out;
        if (maps && out) {
            char buffer[8192];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), maps)) != 0)
                if (std::fwrite(buffer, 1, n, out) != n) { mapsOk = false; break; }
            if (std::ferror(maps)) mapsOk = false;
        }
        if (maps) std::fclose(maps);
        if (out && std::fclose(out) != 0) mapsOk = false;
        const std::string jsonPath = prefix_ + ".json";
        out = std::fopen(jsonPath.c_str(), "wb");
        bool jsonOk = false;
        if (out) {
            jsonOk = std::fprintf(out,
                "{\n  \"format\": \"packed uint64 little-endian PCs\",\n"
                "  \"clock\": \"CLOCK_THREAD_CPUTIME_ID\",\n"
                "  \"scope\": \"project timed loop, main thread only\",\n"
                "  \"tid\": %ld,\n  \"signal\": %d,\n"
                "  \"sample_count\": %d,\n  \"capacity\": %d,\n  \"period_us\": %ld,\n"
                "  \"thread_cpu_ms\": %.6f,\n  \"expected_samples\": %.6f,\n"
                "  \"overruns\": %d,\n  \"dropped\": %d,\n"
                "  \"counters_saturate_at\": %d,\n"
                "  \"pcs_write_ok\": %s,\n  \"maps_write_ok\": %s,\n"
                "  \"note\": \"Sampling perturbs timing; tiny start/stop boundary overhead included in CPU coverage. Do not use sampled runs for A/B timing.\"\n}\n",
                long(tid_), signal_, count, capacity_, periodUs_, cpuMs,
                cpuMs * 1000.0 / double(periodUs_), int(overruns_), int(dropped_),
                int(SIG_ATOMIC_MAX), pcsOk ? "true" : "false", mapsOk ? "true" : "false") > 0;
            if (std::fclose(out) != 0) jsonOk = false;
        }
        std::fprintf(stderr,
            "PC sampler: count=%d main_cpu=%.3fms period=%ldus expected=%.1f overruns=%d dropped=%d prefix=%s%s\n",
            count, cpuMs, periodUs_, cpuMs * 1000.0 / double(periodUs_),
            int(overruns_), int(dropped_), prefix_.c_str(),
            pcsOk && mapsOk && jsonOk ? "" : " (output write failed)");
#endif
    }

private:
#if defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
    static constexpr int capacity_ = 1000000;
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
    inline static std::atomic<std::uintptr_t> pcs_[capacity_];
    inline static volatile sig_atomic_t active_ = 0, count_ = 0;
    inline static volatile sig_atomic_t overruns_ = 0, dropped_ = 0;
    inline static char cookie_;
    static_assert(sizeof(std::uintptr_t) == 8);
    static void handle(int, siginfo_t* info, void* context) {
        // No allocation, library calls, mutex, instance pointers, or errno use.
        if (!active_ || info->si_code != SI_TIMER || info->si_value.sival_ptr != &cookie_) return;
        const sig_atomic_t n = count_;
        if (n < capacity_) {
            pcs_[n].store(static_cast<std::uintptr_t>(
                static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]),
                std::memory_order_relaxed);
            count_ = n + 1;
        } else if (dropped_ < SIG_ATOMIC_MAX) {
            dropped_ = dropped_ + 1;
        }
        const int missed = info->si_overrun;
        if (missed > 0) {
            const sig_atomic_t old = overruns_;
            overruns_ = missed > SIG_ATOMIC_MAX - old ? SIG_ATOMIC_MAX : old + missed;
        }
    }
    static void fail(const char* message) {
        std::fprintf(stderr, "PC sampler: %s\n", message);
    }
    void restore() {
        if (actionSaved_) {
            sigaction(signal_, &oldAction_, nullptr);
            actionSaved_ = false;
        }
        if (maskSaved_) {
            pthread_sigmask(SIG_SETMASK, &oldMask_, nullptr);
            maskSaved_ = false;
        }
    }
    std::string prefix_;
    long periodUs_ = 2000;
    int signal_ = 0;
    pid_t tid_ = 0;
    timer_t timer_{};
    sigset_t set_{}, oldMask_{};
    struct sigaction oldAction_{};
    struct timespec cpuStart_{}, cpuEnd_{};
    bool maskSaved_ = false, actionSaved_ = false;
    bool timerCreated_ = false, started_ = false;
#endif
};

} // namespace xyo::bench
