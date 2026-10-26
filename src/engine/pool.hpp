// xyo-cpp: stepThreads 並列バッチ用ワーカープール。
// 世代カウンタ方式の dispatch-and-join — 呼び出しスレッドも作業に参加する。
// 各バッチは独立した Batch オブジェクトを持つため、寝坊した worker が
// 次世代のタスクを誤って消費することはない。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xyo {

class WorkerPool {
public:
    struct Task {
        void (*fn)(void*);
        void* arg;
    };

    WorkerPool() {
        unsigned hw = std::thread::hardware_concurrency();
        int n = hw > 1 ? int(hw) - 1 : 0;
        if (n > 7) n = 7;   // 十分な上限（Scratch スレッド数の粒度を考慮）
        workers_.reserve(n);
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this] { worker(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
            ++gen_;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int workers() const { return int(workers_.size()); }

    /// tasks[0..n) を並列実行して全完了までブロックする。
    /// 呼び出しスレッドも drain に参加する。One dispatcher only: run()
    /// must not be called concurrently or recursively on the same pool.
    void run(const Task* tasks, size_t n) {
        if (n == 0) return;
        if (n == 1 || workers_.empty()) { // no dispatch for a single task
            for (size_t i = 0; i < n; ++i) tasks[i].fn(tasks[i].arg);
            return;
        }
        auto b = std::make_shared<Batch>(tasks, n);
        {
            std::lock_guard lk(m_);
            cur_ = b;
            ++gen_;
        }
        // The caller already participates. Do not wake seven workers just
        // for a two-task batch; sleepers can safely skip completed generations.
        if (n - 1 >= workers_.size()) cv_.notify_all();
        else for (size_t i = 0; i < n - 1; ++i) cv_.notify_one();
        drain(*b);
        {
            std::unique_lock lk(m_);
            doneCv_.wait(lk, [&] { return b->done.load() >= n; });
            cur_.reset();
        }
    }

private:
    struct Batch {
        Batch(const Task* t, size_t n) : tasks(t), total(n) {}
        const Task* tasks;
        size_t total;
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
    };

    void drain(Batch& b) {
        for (;;) {
            size_t i = b.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= b.total) return;
            b.tasks[i].fn(b.tasks[i].arg);
            if (b.done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                b.total) {
                std::lock_guard lk(m_);
                doneCv_.notify_all();
            }
        }
    }

    void worker() {
        uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Batch> b;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                b = cur_;
            }
            if (b) drain(*b);
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, doneCv_;
    std::shared_ptr<Batch> cur_;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

} // namespace xyo
