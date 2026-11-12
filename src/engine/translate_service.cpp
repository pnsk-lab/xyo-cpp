// PLaMo 3 Translate: localhost-only, opt-in OpenAI-compatible API.
// Copyright (c) xyo-cpp contributors. Model is downloaded/run by the user.
#include "translate_service.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <utility>

#include "../core/json.hpp"
#include "../core/sstr.hpp"
#include "runtime.hpp"
#include "sequencer.hpp"
#include "thread.hpp"

#if defined(XYO_HAVE_TRANSLATE_CURL)
#include <curl/curl.h>
#endif

namespace xyo {
namespace {
std::string translateRequest(const std::string& text,
                             const std::string& lang,
                             const std::atomic<bool>& canceled) {
    if (canceled.load(std::memory_order_relaxed)) return text;
#if defined(XYO_HAVE_TRANSLATE_CURL)
    // Fixed loopback address: never connect to untrusted remote services,
    // never use the process's HTTP_PROXY or user-specific .curlrc.
    const char* portEnv = std::getenv("XYO_PLAMO_PORT");
    int port = 8000;
    if (portEnv && *portEnv) {
        char* end = nullptr;
        long requested = std::strtol(portEnv, &end, 10);
        if (*end || requested < 1 || requested > 65535) return text;
        port = static_cast<int>(requested);
    }
    const std::string url = "http://127.0.0.1:" +
                            std::to_string(port) + "/v1/chat/completions";
    const JsonValue payload = JsonObject{
        {"model", std::string("plamo-3-translate")},
        {"messages", JsonArray{
            JsonObject{
                {"role", std::string("system")},
                {"content", "[plamo-translate]\ntarget_lang=" + lang}
            },
            JsonObject{
                {"role", std::string("user")},
                {"content", text}
            }
        }},
        {"temperature", 0},
        {"max_tokens", 512},
        {"stream", false}
    };
    const std::string body = jsonStringify(payload);
    // libcurl requires global initialization before workers call easy_init.
    // Initialization is serialized even if multiple Scratch runtimes coexist.
    static std::once_flag curlInitFlag;
    static CURLcode curlInitStatus = CURLE_FAILED_INIT;
    std::call_once(curlInitFlag, [] {
        curlInitStatus = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (curlInitStatus != CURLE_OK) return text;
    CURL* easy = curl_easy_init();
    if (!easy) return text;
    std::string response;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_PROXY, "");
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 700L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    // Abort in-flight localhost inference after stop/restart/clone deletion.
    // The worker observes only a shared atomic flag, never Scratch objects.
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t,
            curl_off_t, curl_off_t) -> int {
            return static_cast<const std::atomic<bool>*>(context)
                ->load(std::memory_order_relaxed) ? 1 : 0;
        });
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &canceled);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
        +[](char* data, size_t size, size_t n, void* context) -> size_t {
            auto& output = *static_cast<std::string*>(context);
            const size_t bytes = size * n;
            if (bytes > 1'048'576 || output.size() > 1'048'576 - bytes)
                return 0;  // cap untrusted response size
            output.append(data, bytes);
            return bytes;
        });
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);
    // curl_easy_perform() can wait inside a socket read without invoking
    // the progress callback until the server eventually responds. Drive
    // the same transfer using curl_multi_poll with a bounded 50ms interval,
    // so canceled Scratch threads release their workers promptly even if
    // llama-server stops producing data.
    CURLcode status = CURLE_ABORTED_BY_CALLBACK;
    long httpCode = 0;
    CURLM* multi = curl_multi_init();
    if (multi) {
        if (curl_multi_add_handle(multi, easy) == CURLM_OK) {
            int running = 0;
            CURLMcode mc = curl_multi_perform(multi, &running);
            while (mc == CURLM_OK && running > 0 &&
                   !canceled.load(std::memory_order_relaxed)) {
                int numfds = 0;
                mc = curl_multi_poll(multi, nullptr, 0, 50, &numfds);
                if (mc == CURLM_OK)
                    mc = curl_multi_perform(multi, &running);
            }
            if (mc == CURLM_OK && !canceled.load(std::memory_order_relaxed)) {
                int messages = 0;
                while (CURLMsg* msg = curl_multi_info_read(multi, &messages)) {
                    if (msg->msg == CURLMSG_DONE && msg->easy_handle == easy) {
                        status = msg->data.result;
                        break;
                    }
                }
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);
            }
            curl_multi_remove_handle(multi, easy);
        }
        curl_multi_cleanup(multi);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    if (std::getenv("XYO_PLAMO_TRACE")) {
        // Avoid printing potentially private translation content to logs.
        std::fprintf(stderr, "[plamo] curl=%d http=%ld response_bytes=%zu\n",
                     int(status), httpCode, response.size());
    }
    if (status != CURLE_OK || httpCode != 200) return text;
    // xyo builds with -fno-exceptions. Its JSON parser returns null on
    // invalid input, and the chained accessors return null on missing keys.
    const JsonValue parsed = jsonParse(response);
    const JsonValue& answer = parsed.get("choices").at(0)
                                   .get("message").get("content");
    return answer.isString() && !answer.asString().empty()
        ? answer.asString() : text;
#else
    (void)lang;
    return text;
#endif
}
} // namespace

void Runtime::requestTranslation(Thread* thread, std::string text,
                                 std::string language) {
    if (!translationService)
        translationService = std::make_unique<TranslationService>();
    translationService->request(*this, thread, std::move(text),
                                std::move(language));
}

void TranslationService::request(Runtime& runtime, Thread* thread,
                                 std::string text, std::string language) {
    if (!thread) return;
    // Avoid cutting a UTF-8 code point in half or sending huge untrusted
    // input to inference. Preserve the original reporter output on failure.
    const bool tooLarge = text.size() > 16384 || language.size() > 16;
    // Never fetch a model or transmit text unless the user explicitly opted
    // in to the local API. The model weights are not bundled.
    const char* opt = std::getenv("XYO_PLAMO_TRANSLATE");
    if (tooLarge || std::strcmp(opt ? opt : "", "1") != 0 ||
        (!text.empty() && std::all_of(text.begin(), text.end(),
            [](unsigned char c) { return c >= '0' && c <= '9'; })) ||
        text.empty()) {
        // The normal reporter runs synchronously in the absence of the model.
        // request() is only used after the extension decides to suspend.
        Runtime::WaitEntry wait;
        wait.thread = thread;
        wait.nextTick = true;
        wait.resolveValue = text.empty()
            ? Value::emptyStr() : Value::string(SStr::fromUtf8(text));
        runtime.enqueueWait(wait);
        runtime.transitionThread(*thread, STATUS_PROMISE_WAIT);
        return;
    }
    if (text == lastSource_ && language == lastLanguage_) {
        Runtime::WaitEntry wait;
        wait.thread = thread;
        wait.nextTick = true;
        wait.resolveValue = Value::string(SStr::fromUtf8(lastTranslation_));
        runtime.enqueueWait(wait);
        runtime.transitionThread(*thread, STATUS_PROMISE_WAIT);
        return;
    }
    // Canceled HTTP workers are intentionally retained until their
    // futures are ready, so erasing them never blocks the Scratch thread.
    // They must not occupy the four *active* inference slots after a stop,
    // green-flag restart, or clone deletion.
    std::erase_if(pending_, [](const Pending& p) {
        return p.canceled->load(std::memory_order_relaxed) &&
               p.result.wait_for(std::chrono::milliseconds(0)) ==
                   std::future_status::ready;
    });
    const size_t activeRequests = std::count_if(
        pending_.begin(), pending_.end(), [](const Pending& p) {
            return !p.canceled->load(std::memory_order_relaxed);
        });
    // A canceled request can still be inside curl_easy_perform while
    // libcurl waits for socket progress. Without a separate hard ceiling,
    // rapid clone creation/deletion could spawn unbounded std::async threads.
    // Reserve four normal requests plus a bounded cancellation backlog.
    constexpr size_t kMaxActiveRequests = 4;
    constexpr size_t kMaxOutstandingWorkers = 16;
    if (activeRequests >= kMaxActiveRequests ||
        pending_.size() >= kMaxOutstandingWorkers) {
        Runtime::WaitEntry wait;
        wait.thread = thread;
        wait.nextTick = true;
        wait.resolveValue = Value::string(SStr::fromUtf8(text));
        runtime.enqueueWait(wait);
        runtime.transitionThread(*thread, STATUS_PROMISE_WAIT);
        return;
    }
    const uint64_t id = ++nextRequest_;
    // Guard against stale Thread* whose storage was recycled for a new script.
    // This token lives in the suspended Scratch stack frame, not in the worker.
    StackFrame* frame = thread->peekStackFrame();
    if (!frame) return;
    if (!frame->executionContext)
        frame->executionContext = std::make_unique<ExecContext>();
    frame->executionContext->vals["plamo_request"] =
        Value::num(static_cast<double>(id));
    runtime.transitionThread(*thread, STATUS_PROMISE_WAIT);
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    auto future = std::async(std::launch::async,
        [source = text, target = language, canceled] {
            return translateRequest(source, target, *canceled);
        });
    pending_.push_back(Pending{
        thread, id, std::move(text), std::move(language),
        std::move(future), std::move(canceled)
    });
}

void TranslationService::cancelThread(const Thread* thread) noexcept {
    for (auto& p : pending_)
        if (p.thread == thread)
            p.canceled->store(true, std::memory_order_relaxed);
}

void TranslationService::cancelAll() noexcept {
    for (auto& p : pending_)
        p.canceled->store(true, std::memory_order_relaxed);
}

bool TranslationService::hasPending() const {
    return std::any_of(pending_.begin(), pending_.end(),
        [](const Pending& p) {
            return !p.canceled->load(std::memory_order_relaxed);
        });
}

void TranslationService::waitForPending(
    std::chrono::milliseconds maxWait) const {
    for (const auto& p : pending_) {
        if (p.canceled->load(std::memory_order_relaxed)) continue;
        p.result.wait_for(maxWait);
        break;
    }
}

void TranslationService::poll(Runtime& runtime) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->result.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
            ++it;
            continue;
        }
        // The worker uses xyo's null-on-error JSON parser and libcurl
        // status codes; malformed responses never throw here.
        const std::string translated = it->result.get();
        const bool canceled = it->canceled->load(std::memory_order_relaxed);
        if (canceled) {
            it = pending_.erase(it);
            continue;
        }
        Thread* thread = it->thread;
        const uint64_t id = it->requestId;
        const std::string input = std::move(it->input);
        const std::string language = std::move(it->language);
        it = pending_.erase(it);
        if (std::find(runtime.threads.begin(), runtime.threads.end(), thread)
                == runtime.threads.end())
            continue;
        if (thread->isKilled || thread->status != STATUS_PROMISE_WAIT)
            continue;
        StackFrame* frame = thread->peekStackFrame();
        if (!frame || !frame->executionContext) continue;
        const auto token = frame->executionContext->vals.find("plamo_request");
        if (token == frame->executionContext->vals.end() ||
            !token->second.isNumber() ||
            token->second.asNumberUnchecked() != static_cast<double>(id))
            continue;
        // Cache only results that are actually delivered to a live
        // Scratch continuation. A detached script must not poison the
        // next run's cache with a result it never observed.
        if (translated != input) {
            lastSource_ = input;
            lastLanguage_ = language;
            lastTranslation_ = translated;
        }
        const Value result = Value::string(SStr::fromUtf8(translated));
        resolveWait(*runtime.sequencer, *thread, result);
    }
}
} // namespace xyo
