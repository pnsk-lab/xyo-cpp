// Async local PLaMo translation bridge, no bundled model or remote service.
#pragma once
#include <cstdint>
#include <chrono>
#include <atomic>
#include <memory>
#include <future>
#include <string>
#include <vector>

namespace xyo {
struct Runtime;
struct Thread;

class TranslationService {
public:
    ~TranslationService() = default;
    void request(Runtime& runtime, Thread* thread, std::string text,
                 std::string language);
    void poll(Runtime& runtime);
    void cancelThread(const Thread* thread) noexcept;
    void cancelAll() noexcept;
    bool hasPending() const;
    // Includes canceled workers still waiting for curl to exit.
    size_t outstandingCount() const noexcept { return pending_.size(); }
    void waitForPending(std::chrono::milliseconds maxWait) const;

private:
    struct Pending {
        Thread* thread;  // identity only; never accessed on worker
        uint64_t requestId;
        std::string input;
        std::string language;
        std::future<std::string> result;
        std::shared_ptr<std::atomic<bool>> canceled;
    };
    std::vector<Pending> pending_;
    uint64_t nextRequest_ = 0;
    std::string lastSource_, lastLanguage_, lastTranslation_;
};
} // namespace xyo
