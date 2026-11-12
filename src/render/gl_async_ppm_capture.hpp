// Opt-in NVIDIA DLSS debug frame capture using GL PBOs and a disk worker.
// Include only after SDL2, desktop GL prototypes and ../core/fileio.hpp.
#pragma once
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>    // open, openat, O_DIRECTORY/O_NOFOLLOW (Linux-only)
#include <sys/file.h> // flock (protect active temporary writers)
#include <sys/stat.h> // fstatat, S_ISREG
#include <unistd.h>   // mkstemp, close, unlinkat, fsync (Linux-only)

namespace xyo::render {

class GlAsyncPpmCapture final {
public:
    GlAsyncPpmCapture()
        : rgbCapture_(rgbReadbackFromEnv()),
          recycleCpuBuffers_(cpuBufferReuseFromEnv()),
          stageProfile_(isOne("XYO_SHOT_ASYNC_STAGE_PROFILE")),
          directMapped_(isOne("XYO_SHOT_ASYNC_DIRECT_MAP")),
          testWriteDelayMs_(testWriteDelayFromEnv()),
          testFailAfterSaved_(testCountFromEnv("XYO_SHOT_ASYNC_TEST_FAIL_AFTER")),
          testEnqueueDelayMs_(testCountFromEnv("XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS")),
          testFailStage_(testFailureStageFromEnv()),
          testAbortBeforeEnqueue_(isOne("XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE")),
          atomicFiles_(isOne("XYO_SHOT_ASYNC_ATOMIC") ||
                       isOne("XYO_SHOT_ASYNC_DURABLE")),
          durableFiles_(isOne("XYO_SHOT_ASYNC_DURABLE")),
          recoverStale_(isOne("XYO_SHOT_ASYNC_RECOVER_STALE")),
          recoveryThreadEnabled_(isOne("XYO_SHOT_ASYNC_RECOVERY_THREAD")),
          seriesLockEnabled_(isOne("XYO_SHOT_ASYNC_LOCK_SERIES")),
          testRecoveryPauseMs_(testCountFromEnv(
              "XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS")),
          testRecoveryEntryDelayMs_(testCountFromEnv(
              "XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS")),
          testPreUnlinkPauseMs_(testCountFromEnv(
              "XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS")),
          testPostUnlinkPauseMs_(testCountFromEnv(
              "XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS")),
          testFailRecoverySync_(isOne("XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC")),
          testAfterRecoveryBatchPauseMs_(testCountFromEnv(
              "XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS")),
          testFailWriteRow_(testCountFromEnv("XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW")),
          testStagePauseMs_(testCountFromEnv("XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS")),
          testFailSync_(testSyncFailureFromEnv()),
          worker_([this] { writeLoop(); }) {}
    ~GlAsyncPpmCapture() {
        stopRecovery();
        stopWorker();
        // Keep the process-wide cooperative lock through the final write.
        // Never unlink the lockfile: another process could have opened
        // its inode and a later pathname replacement would bypass the lock.
        if (seriesLockFd_ >= 0) ::close(seriesLockFd_);
    }
    GlAsyncPpmCapture(const GlAsyncPpmCapture&) = delete;
    GlAsyncPpmCapture& operator=(const GlAsyncPpmCapture&) = delete;

    SDL_GLContext ownerContext() const noexcept { return ownerContext_; }

    bool capture(SDL_Renderer* renderer, const char* filename) {
        if (closed_ || !renderer || !filename || !*filename || failed_) return false;
        if (seriesLockEnabled_ && !seriesLockAttempted_) {
            seriesLockAttempted_ = true;
            if (!acquireSeriesLock(filename)) {
                failed_ = true;
                return false;
            }
        }
        // Foreground recovery retains the old <=128 entries/frame policy.
        // The optional background mode moves *all* directory operations off
        // the render thread. Recovery state is not read here while the
        // recovery worker is running; finish()/destructor join it first.
        if (recoverStale_) {
            const auto recoverySubmitStart = Clock::now();
            if (recoveryThreadEnabled_) {
                if (!recoveryThreadStarted_ && !recoveryFinished_)
                    launchRecovery(filename);
            } else if (!recoveryFinished_) {
                if (!recoveryStarted_) startRecovery(filename);
                stepRecovery();
            }
            recoverySubmitMs_ += elapsedMs(recoverySubmitStart);
            if (recoveryThreadEnabled_ && !testRecoveryHandshakeDone_ &&
                (testPreUnlinkPauseMs_ || testPostUnlinkPauseMs_)) {
                // Test-only synchronization with a worker that has reached
                // the critical phase before (or just after) an unlink.
                testRecoveryHandshakeDone_ = true;
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                while (!(testPreUnlinkPauseMs_
                            ? recoveryPreUnlinkEntered_.load(
                                  std::memory_order_acquire)
                            : recoveryPostUnlinkEntered_.load(
                                  std::memory_order_acquire)) &&
                       Clock::now() < deadline)
                    SDL_Delay(1);
            }
        }
        const auto captureStart = stageProfile_ ? Clock::now() : Clock::time_point{};
        SDL_RendererInfo ri{};
        if (SDL_GetRendererInfo(renderer, &ri) || !ri.name ||
            std::strcmp(ri.name, "opengl") != 0) {
            std::fprintf(stderr, "[async PPM] renderer not desktop GL\n");
            return false;
        }
        int w = 0, h = 0;
        // Cap PBO + queued CPU image memory: <=32 MiB per screenshot.
        if (SDL_GetRendererOutputSize(renderer, &w, &h) ||
            w <= 0 || h <= 0 ||
            size_t(w) * size_t(h) * 4 > 32u * 1024u * 1024u ||
            SDL_RenderFlush(renderer) || !SDL_GL_GetCurrentContext()) {
            std::fprintf(stderr, "[async PPM] invalid renderer dimensions or GL context: %dx%d (%s)\n",
                         w, h, SDL_GetError());
            return false;
        }
        const SDL_GLContext currentContext = SDL_GL_GetCurrentContext();
        if (!ownerContext_) {
            ownerContext_ = currentContext;
        } else if (currentContext != ownerContext_) {
            std::fprintf(stderr,
                "[async PPM] GL context changed during capture; "
                "refusing foreign-context PBO access\n");
            failed_ = true;
            return false;
        }
        if (!reclaimWrittenPbos() || !collectReady(false))
            return false;
        // A directly mapped PBO cannot receive new glReadPixels until the
        // disk worker has finished reading its pointer and GL has unmapped
        // it. The finite pool applies to both pending and mapped buffers.
        while (free_.empty() &&
               pending_.size() + borrowedPbos_ >= kMaxPbos) {
            if (!pending_.empty()) {
                if (!collectReady(true)) return false;
            } else {
                ++pboWorkerWaits_;
                const auto waitStart = Clock::now();
                std::unique_lock<std::mutex> lock(mutex_);
                completedPboReady_.wait(lock, [this] {
                    return !completedPbos_.empty() || failed_.load();
                });
                pboWorkerWaitMs_ += elapsedMs(waitStart);
                if (failed_) return false;
            }
            if (!reclaimWrittenPbos()) return false;
        }

        Buffer buffer{};
        if (!free_.empty()) {
            buffer = free_.back();
            free_.pop_back();
        } else {
            glGenBuffers(1, &buffer.id);
            if (!buffer.id) return false;
            ++pboCreated_;
        }

        GLint oldPbo = 0, oldReadFbo = 0, oldReadBuffer = 0;
        GLint alignment = 0, rowLength = 0, skipRows = 0, skipPixels = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
        glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);

        // RGB PBO is experimental: a 25% smaller readback when PPM's
        // alpha channel is discarded. Alignment=1 makes rows tightly packed.
        const GLsizeiptr bytes = GLsizeiptr(w) * h *
                                  (rgbCapture_ ? 3 : 4);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer.id);
        if (buffer.capacity != bytes) {
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            buffer.capacity = bytes;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        // Keep SDL's GL_READ_BUFFER selection. With the SDL EGL
        // offscreen driver, GL_DOUBLEBUFFER reports false but SDL's
        // drawable uses GL_BACK (0x405); selecting GL_FRONT is invalid.
        const auto readStart = stageProfile_ ? Clock::now() : Clock::time_point{};
        glReadPixels(0, 0, w, h, rgbCapture_ ? GL_RGB : GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        if (stageProfile_) glReadCallMs_ += elapsedMs(readStart);
        const GLenum nativeError = glGetError();
        const GLenum error = testFailStage_ == FailStage::Read
            ? GL_INVALID_OPERATION : nativeError;

        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(oldReadFbo));
        glReadBuffer(GLenum(oldReadBuffer));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);

        if (error != GL_NO_ERROR || !fence) {
            std::fprintf(stderr, "[async PPM] GL readback failed: error=0x%x, fence=%d, oldFbo=%d, oldRead=0x%x\n",
                         unsigned(error), fence ? 1 : 0,
                         int(oldReadFbo), unsigned(oldReadBuffer));
            if (fence) glDeleteSync(fence);
            free_.push_back(buffer);
            failed_ = true;
            return false;
        }
        pending_.push_back({buffer, fence, w, h, filename, rgbCapture_});
        ++submitted_;
        if (stageProfile_) captureMs_ += elapsedMs(captureStart);
        submittedBytes_ += static_cast<size_t>(bytes);
        if (pending_.size() > maxPending_)
            maxPending_ = pending_.size();
        return true;
    }

    bool finish() {
        if (closed_) return !failed_.load();
        // Stop scanning as early as possible. GPU readback and the disk
        // writer may still be draining; stale-file cleanup must not keep
        // competing for I/O throughout the final screenshot flush.
        requestRecoveryStop();
        // Never manipulate PBO names in an unrelated GL context. Buffer
        // object names are local to a context/share group, and GLuint alone
        // is not an ownership proof. SdlRenderer first rebinds the capture
        // context when it still exists.
        const SDL_GLContext current = SDL_GL_GetCurrentContext();
        if (!current || (ownerContext_ && current != ownerContext_)) {
            std::fprintf(stderr,
                "[async PPM] owning GL context unavailable at final flush; "
                "skipping unsafe GL cleanup\n");
            failed_ = true;
            stopRecovery();
            stopWorker();
            closed_ = true;
            return false;
        }
        while (!failed_ && !pending_.empty()) {
            if (!collectReady(true)) {
                failed_ = true;
                break;
            }
        }
        for (const Pending& job : pending_) {
            if (job.fence) glDeleteSync(job.fence);
            free_.push_back(job.buffer);
        }
        pending_.clear();
        // The disk writer may still own mapped PBO memory. Wait for the
        // recovery thread first so it does not contend with outstanding
        // disk writes, then join the writer before GL unmap/delete.
        stopRecovery();
        stopWorker();
        if (!reclaimWrittenPbos()) failed_ = true;
        GLint oldPbo = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (const Buffer& buffer : free_) {
            if (!buffer.id) continue;
            glDeleteBuffers(1, &buffer.id);
            ++pboDeleted_;
        }
        free_.clear();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
        closed_ = true;
        const char* statsFlag = std::getenv("XYO_SHOT_ASYNC_STATS");
        if (statsFlag && statsFlag[0] == '1') {
            std::fprintf(stderr,
                "[async PPM] stats submitted=%zu saved=%zu "
                "max_pbos=%zu max_cpu_queue=%zu "
                "gpu_waits=%zu gpu_wait_ms=%.3f "
                "queue_waits=%zu queue_wait_ms=%.3f "
                "error=%d\n",
                submitted_, saved_.load(), maxPending_, maxQueued_,
                gpuWaits_, gpuWaitMs_, queueWaits_, queueWaitMs_,
                int(failed_.load()));
            std::fprintf(stderr,
                "[async PPM] transfer format=%s bytes_per_pixel=%d "
                "submitted_bytes=%zu\n",
                rgbCapture_ ? "RGB" : "RGBA", rgbCapture_ ? 3 : 4,
                submittedBytes_);
            std::fprintf(stderr,
                "[async PPM] cpu_buffers reuse=%d allocations=%zu "
                "reused=%zu peak_pool=%zu\n",
                int(recycleCpuBuffers_), cpuAllocations_,
                cpuReuses_, maxFreeCpuBuffers_);
        }
        if (stageProfile_) {
            std::fprintf(stderr,
                "[async PPM] stages capture_ms=%.3f gl_read_call_ms=%.3f "
                "pbo_maps=%zu pbo_map_ms=%.3f cpu_copy_ms=%.3f "
                "cpu_copy_bytes=%zu writer_ms=%.3f writer_calls=%zu\n",
                captureMs_, glReadCallMs_, pboMaps_, pboMapMs_,
                cpuCopyMs_, cpuCopyBytes_, writerMs_, writerCalls_);
        }
        if (stageProfile_ || (statsFlag && statsFlag[0] == '1')) {
            std::fprintf(stderr,
                "[async PPM] direct_map enabled=%d mapped_bytes=%zu "
                "max_borrowed_pbos=%zu pbo_worker_waits=%zu "
                "pbo_worker_wait_ms=%.3f\n",
                int(directMapped_), directlyMappedBytes_,
                maxBorrowedPbos_, pboWorkerWaits_, pboWorkerWaitMs_);
            std::fprintf(stderr,
                "[async PPM] lifecycle created=%zu deleted=%zu "
                "borrowed_remaining=%zu completed_remaining=%zu "
                "aborted_before_enqueue=%zu\n",
                pboCreated_, pboDeleted_, borrowedPbos_,
                completedPbos_.size(), abortedBeforeEnqueue_);
            std::fprintf(stderr,
                "[async PPM] atomic_files enabled=%d staged=%zu "
                "published=%zu discarded=%zu\n",
                int(atomicFiles_), atomicStagedFiles_,
                atomicPublishedFiles_, atomicDiscardedFiles_);
            std::fprintf(stderr,
                "[async PPM] durability enabled=%d file_fsyncs=%zu "
                "dir_fsyncs=%zu sync_failures=%zu\n",
                int(durableFiles_), syncedFiles_, syncedDirectories_,
                syncFailures_);
            if (seriesLockEnabled_) {
                std::fprintf(stderr,
                    "[async PPM] series_lock enabled=1 acquired=%d\n",
                    int(seriesLockFd_ >= 0));
            }
            if (recoverStale_) {
                std::fprintf(stderr,
                    "[async PPM] recovery checked=%zu removed=%zu "
                    "recent=%zu locked=%zu unsafe=%zu errors=%zu "
                    "dir_entries=%zu scan_ms=%.3f\n",
                    recoveryChecked_, recoveryRemoved_, recoveryRecent_,
                    recoveryLocked_, recoveryUnsafe_, recoveryErrors_,
                    recoveryEntries_, recoveryTimeMs_);
                std::fprintf(stderr,
                    "[async PPM] recovery_durability enabled=%d "
                    "dir_fsyncs=%zu synced_removals=%zu sync_failures=%zu "
                    "sync_batch=%zu max_unsynced=%zu\n",
                    int(durableFiles_), recoveryDirFsyncs_,
                    recoverySyncedRemovals_, recoverySyncFailures_,
                    kRecoverySyncBatch, recoveryMaxUnsyncedRemovals_);
                std::fprintf(stderr,
                    "[async PPM] recovery_thread enabled=%d started=%d "
                    "joined=%d foreground_ms=%.3f\n",
                    int(recoveryThreadEnabled_), int(recoveryThreadStarted_),
                    int(recoveryThreadJoined_), recoverySubmitMs_);
                std::fprintf(stderr,
                    "[async PPM] recovery_shutdown joined_ms=%.3f "
                    "cancel_observed=%d pending_images_at_stop=%zu "
                    "preunlink_skipped=%zu\n",
                    recoveryJoinMs_, int(recoveryCancellationObserved_),
                    recoveryPendingImagesAtStop_, recoveryPreUnlinkSkipped_);
                std::fprintf(stderr,
                    "[async PPM] recovery_progress complete=%d steps=%zu "
                    "max_step_entries=%zu max_step_ms=%.3f pending=%d\n",
                    int(recoveryFinished_), recoverySteps_, recoveryMaxStepEntries_,
                    recoveryMaxStepMs_, int(!recoveryFinished_));
            }
        }
        // Every owned PBO must have been unmapped and destroyed, also when
        // capture fails in the middle of the disk-writer handoff.
        if (borrowedPbos_ || pboCreated_ != pboDeleted_ ||
            !completedPbos_.empty()) {
            std::fprintf(stderr,
                "[async PPM] GL buffer lifecycle mismatch at finish\n");
            failed_ = true;
        }
        return !failed_.load();
    }

private:
    struct Buffer {
        GLuint id = 0;
        GLsizeiptr capacity = 0;
    };
    struct Pending {
        Buffer buffer;
        GLsync fence = nullptr;
        int width = 0, height = 0;
        std::string filename;
        bool packedRgb = false;
    };
    struct Image {
        std::string filename;
        int width = 0, height = 0;
        bool packedRgb = false;
        std::vector<uint8_t> pixels;
        // Only used with DIRECT_MAP=1. The producer GL context keeps
        // mappedBuffer mapped until the writer returns its completion.
        const uint8_t* mapped = nullptr;
        Buffer mappedBuffer{};
    };
    static constexpr size_t kMaxPbos = 4;
    static constexpr size_t kMaxQueuedImages = 8;
    // At most eight pending writes, one active write, one producer staging
    // image. When the queue drains, keep up to ten buffers for reuse.
    static constexpr size_t kMaxReusableCpuBuffers = kMaxQueuedImages + 2;
    // Recovery defaults to a conservative one-day age cutoff. It never
    // unlinks tempfiles with a live cooperative (flock) writer, even if
    // that writer has been active for more than a day.
    static constexpr std::time_t kStaleFileAgeSeconds = 24 * 60 * 60;
    static bool asciiDigit(char c) { return c >= '0' && c <= '9'; }
    static bool asciiAlphaNum(char c) {
        return asciiDigit(c) || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z');
    }

    static bool captureSeries(const std::string& filename,
                              std::string& directory,
                              std::string& prefix) {
        const size_t slash = filename.find_last_of('/');
        directory = parentPath(filename);
        const std::string base = filename.substr(
            slash == std::string::npos ? 0 : slash + 1);
        if (base.size() < 12 || base.compare(base.size() - 4, 4, ".ppm") ||
            base[base.size() - 11] != '-') return false;
        for (size_t i = base.size() - 10; i < base.size() - 4; ++i)
            if (!asciiDigit(base[i])) return false;
        prefix = base.substr(0, base.size() - 11);
        return !prefix.empty();
    }

    bool acquireSeriesLock(const std::string& filename) {
        std::string directory, prefix;
        if (!captureSeries(filename, directory, prefix)) {
            std::fprintf(stderr,
                "[async PPM] invalid capture-series filename for lock\n");
            return false;
        }
        // Advisory lock *per prefix*, not the whole directory, so unrelated
        // capture series can run concurrently. The lockfile intentionally
        // remains in place after release; do not unlink or replace it.
        const int dirfd = ::open(directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dirfd < 0) {
            std::fprintf(stderr,
                "[async PPM] cannot open capture-series directory for lock: %s\n",
                directory.c_str());
            return false;
        }
        const std::string lockName = "." + prefix + ".xyo-capture.lock";
        const int fd = ::openat(dirfd, lockName.c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        ::close(dirfd);
        if (fd < 0) {
            std::fprintf(stderr,
                "[async PPM] cannot open capture-series lock: %s\n",
                lockName.c_str());
            return false;
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_uid != ::geteuid() || st.st_nlink != 1) {
            std::fprintf(stderr,
                "[async PPM] unsafe capture-series lock file: %s\n",
                lockName.c_str());
            ::close(fd);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            std::fprintf(stderr,
                "[async PPM] capture series already active: %s\n",
                filename.c_str());
            ::close(fd);
            return false;
        }
        seriesLockFd_ = fd;
        return true;
    }

    static constexpr size_t kRecoveryEntriesPerFrame = 128;
    // In durable mode, keep the un-fsynced deletion window bounded even
    // while a long-lived process is still scanning. This is a count limit,
    // not a promise that power loss between two fsyncs is harmless.
    static constexpr size_t kRecoverySyncBatch = 64;
    static constexpr auto kRecoveryTimePerFrame = std::chrono::milliseconds(1);
    static constexpr const char* kRecoveryMarker = ".ppm.part.";
    static constexpr size_t kRecoveryMarkerSize = 10;
    static constexpr size_t kRecoveryTokenSize = 6;

    void syncRecoveredRemovals(bool force) {
        if (!durableFiles_ || !recoveryDir_ ||
            recoveryRemoved_ <= recoverySyncedRemovals_)
            return;
        const size_t pending = recoveryRemoved_ - recoverySyncedRemovals_;
        if (pending > recoveryMaxUnsyncedRemovals_)
            recoveryMaxUnsyncedRemovals_ = pending;
        // After a failed sync the count of visible but not proven-durable
        // deletions continues growing; reflect that in telemetry without
        // repeatedly issuing the known-failing filesystem operation.
        if (recoverySyncFailed_ || (!force && pending < kRecoverySyncBatch))
            return;

        // Synchronize *directory entries*, not the unlinked file's data.
        // A failed fsync cannot roll back a visible unlink. Report it once
        // and stop retrying: repeated failure must not stall every frame
        // and must never count the remaining removals as durable.
        const bool injected = testFailRecoverySync_;
        if (!injected && ::fsync(::dirfd(recoveryDir_)) == 0) {
            ++recoveryDirFsyncs_;
            recoverySyncedRemovals_ = recoveryRemoved_;
            if (!force && recoveryDirFsyncs_ == 1 &&
                testAfterRecoveryBatchPauseMs_) {
                // Crash testing only. A committed 64-unlink batch is now
                // durable; the controller kills this process before the
                // recovery worker can touch another old orphan.
                std::fprintf(stderr,
                    "[async PPM] test first recovery batch fsynced "
                    "removed=%zu\n", recoverySyncedRemovals_);
                std::fflush(stderr);
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    testAfterRecoveryBatchPauseMs_));
            }
        } else {
            recoverySyncFailed_ = true;
            ++recoverySyncFailures_;
            ++recoveryErrors_;
            std::fprintf(stderr,
                "[async PPM] recovered PPM unlink directory fsync "
                "failed%s; deletion already visible\n",
                injected ? " (injected)" : "");
        }
    }

    void closeRecovery() {
        if (!recoveryDir_) return;
        // A partial final batch must also be persisted when the scanner
        // finishes normally or is cancelled during final PBO drain.
        syncRecoveredRemovals(true);
        ::closedir(recoveryDir_);
        recoveryDir_ = nullptr;
    }

    // SDL_Thread creation returns an error instead of throwing. The project
    // is compiled with -fno-exceptions, so std::thread exception handling is
    // not a safe mechanism for a best-effort recovery service.
    static int SDLCALL recoveryThreadMain(void* context) {
        auto* self = static_cast<GlAsyncPpmCapture*>(context);
        self->startRecovery(self->recoveryFilename_);
        while (!self->recoveryStopRequested_.load(std::memory_order_relaxed) &&
               !self->recoveryFinished_) {
            self->stepRecovery();
            SDL_Delay(0);
        }
        self->closeRecovery();
        return 0;
    }

    void launchRecovery(const std::string& filename) {
        recoveryFilename_ = filename;
        recoveryThread_ = SDL_CreateThread(
            &GlAsyncPpmCapture::recoveryThreadMain,
            "xyo-ppm-recovery", this);
        if (!recoveryThread_) {
            std::fprintf(stderr,
                "[async PPM] cannot start recovery thread: %s\n",
                SDL_GetError());
            // The optional scanner must not make ordinary capture fail.
            ++recoveryErrors_;
            recoveryFinished_ = true;
            return;
        }
        recoveryThreadStarted_ = true;
    }

    void requestRecoveryStop() noexcept {
        if (!recoveryThread_) return;
        if (!recoveryStopRecorded_) {
            // This is captured on the render thread, before joining the
            // disk writer. The writer's successful save count is atomic.
            const size_t saved = saved_.load(std::memory_order_acquire);
            recoveryPendingImagesAtStop_ =
                submitted_ >= saved ? submitted_ - saved : 0;
            recoveryStopRecorded_ = true;
        }
        recoveryStopRequested_.store(true, std::memory_order_release);
    }

    void stopRecovery() {
        if (recoveryThread_) {
            const auto joinStarted = Clock::now();
            requestRecoveryStop();
            SDL_WaitThread(recoveryThread_, nullptr);
            recoveryJoinMs_ += elapsedMs(joinStarted);
            recoveryThread_ = nullptr;
            recoveryThreadJoined_ = true;
        }
        closeRecovery();
    }

    void startRecovery(const std::string& filename) {
        recoveryStarted_ = true;
        std::string directory, prefix;
        if (!captureSeries(filename, directory, prefix)) {
            ++recoveryErrors_;
            recoveryFinished_ = true;
            return;
        }
        // One directory cursor; no complete directory snapshot in memory.
        // If the directory changes while scanning, readdir may or may not
        // observe new entries. The next capture session can scan again.
        recoveryBegin_ = prefix + "-";
        recoveryExpectedLength_ = recoveryBegin_.size() + 6 +
                                  kRecoveryMarkerSize + kRecoveryTokenSize;
        const int fd = ::open(directory.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            std::fprintf(stderr,
                "[async PPM] recovery cannot open directory: %s\n",
                directory.c_str());
            ++recoveryErrors_;
            recoveryFinished_ = true;
            return;
        }
        recoveryDir_ = ::fdopendir(fd);
        if (!recoveryDir_) {
            ::close(fd);
            ++recoveryErrors_;
            recoveryFinished_ = true;
        }
    }

    void stepRecovery() {
        if (!recoveryDir_) return;
        const auto started = std::chrono::steady_clock::now();
        // Test-only simulated slow filesystem operation, for verifying
        // capture() does not wait for the background scanner.
        if (testRecoveryPauseMs_)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(testRecoveryPauseMs_));
        size_t entries = 0;
        // Check the elapsed-time target every 16 entries rather than
        // querying the clock once per unrelated filename. Even if a
        // filesystem operation blocks, the 128-entry hard cap still applies.
        while (entries < kRecoveryEntriesPerFrame &&
               (entries == 0 || (entries & 15u) != 0 ||
                std::chrono::steady_clock::now() - started <
                    kRecoveryTimePerFrame)) {
            // The worker must not continue another 128 potentially slow
            // filesystem operations after finish() has requested shutdown.
            // Foreground recovery never sets this stop flag.
            if (recoveryStopRequested_.load(std::memory_order_acquire)) {
                recoveryCancellationObserved_ = true;
                break;
            }
            if (testRecoveryEntryDelayMs_)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(testRecoveryEntryDelayMs_));
            if (recoveryStopRequested_.load(std::memory_order_acquire)) {
                recoveryCancellationObserved_ = true;
                break;
            }
            errno = 0;
            dirent* entry = ::readdir(recoveryDir_);
            if (!entry) {
                if (errno) ++recoveryErrors_;
                recoveryFinished_ = true;
                closeRecovery();
                break;
            }
            ++entries;
            ++recoveryEntries_;
            // Most entries in a large screenshot directory do not belong
            // to this series. A view avoids allocating and copying their
            // names, and time() is only needed for matching candidates.
            const std::string_view name(entry->d_name);
            if (name.size() != recoveryExpectedLength_ ||
                name.compare(0, recoveryBegin_.size(), recoveryBegin_) != 0 ||
                name.compare(recoveryBegin_.size() + 6, kRecoveryMarkerSize, kRecoveryMarker) != 0)
                continue;
            bool valid = true;
            for (size_t i = recoveryBegin_.size(); i < recoveryBegin_.size() + 6; ++i)
                valid = valid && asciiDigit(name[i]);
            for (size_t i = name.size() - kRecoveryTokenSize; i < name.size(); ++i)
                valid = valid && asciiAlphaNum(name[i]);
            if (!valid) continue;
            ++recoveryChecked_;
            const std::time_t now = std::time(nullptr);
            struct stat before{};
            if (::fstatat(::dirfd(recoveryDir_), name.data(), &before,
                          AT_SYMLINK_NOFOLLOW) != 0) {
                ++recoveryErrors_;
                continue;
            }
            if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
                before.st_nlink != 1) {
                ++recoveryUnsafe_;
                continue;
            }
            // Future-dated files are not stale; do not trust d_type.
            if (now < before.st_mtime ||
                now - before.st_mtime < kStaleFileAgeSeconds) {
                ++recoveryRecent_;
                continue;
            }
            if (recoveryStopRequested_.load(std::memory_order_acquire)) {
                recoveryCancellationObserved_ = true;
                break;
            }
            const int fd = ::openat(::dirfd(recoveryDir_), name.data(),
                                   O_RDONLY | O_NONBLOCK | O_NOFOLLOW |
                                   O_CLOEXEC);
            if (fd < 0) {
                ++recoveryErrors_;
                continue;
            }
            struct stat actual{};
            const bool sameInode = ::fstat(fd, &actual) == 0 &&
                S_ISREG(actual.st_mode) && actual.st_nlink == 1 &&
                actual.st_dev == before.st_dev &&
                actual.st_ino == before.st_ino &&
                actual.st_uid == ::geteuid();
            if (!sameInode) {
                ++recoveryUnsafe_;
            } else if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                ++recoveryLocked_;
            } else {
                // Check inode, link count and age again *after* acquiring
                // the exclusive lock to narrow metadata-change races.
                // This is still not a security boundary against malicious
                // concurrent renames in a shared/untrusted directory.
                struct stat locked{};
                struct stat named{};
                const bool verified = ::fstat(fd, &locked) == 0 &&
                    S_ISREG(locked.st_mode) && locked.st_nlink == 1 &&
                    locked.st_uid == ::geteuid() &&
                    ::fstatat(::dirfd(recoveryDir_), name.data(), &named,
                              AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISREG(named.st_mode) && named.st_nlink == 1 &&
                    named.st_dev == locked.st_dev &&
                    named.st_ino == locked.st_ino;
                if (!verified) {
                    ++recoveryUnsafe_;
                } else if (now < locked.st_mtime ||
                           now - locked.st_mtime < kStaleFileAgeSeconds) {
                    ++recoveryRecent_;
                } else {
                    if (testPreUnlinkPauseMs_) {
                        // Fault injection only. The worker owns the file's
                        // exclusive flock while the render thread waits
                        // for this marker, then sends a stop request.
                        recoveryPreUnlinkEntered_.store(
                            true, std::memory_order_release);
                        const auto deadline = Clock::now() +
                            std::chrono::milliseconds(testPreUnlinkPauseMs_);
                        while (Clock::now() < deadline &&
                               !recoveryStopRequested_.load(
                                   std::memory_order_acquire))
                            SDL_Delay(1);
                    }
                    // A stop arriving during the last fstatat/flock must
                    // not cause an additional unnecessary unlinkat I/O.
                    // Close the FD before leaving the current batch.
                    if (recoveryStopRequested_.load(std::memory_order_acquire)) {
                        recoveryCancellationObserved_ = true;
                        ++recoveryPreUnlinkSkipped_;
                    } else if (::unlinkat(::dirfd(recoveryDir_),
                                          name.data(), 0) == 0) {
                        ++recoveryRemoved_;
                        if (testPostUnlinkPauseMs_) {
                            // Test-only cancellation AFTER a successful
                            // unlink but BEFORE the required durable fsync.
                            recoveryPostUnlinkEntered_.store(
                                true, std::memory_order_release);
                            const auto deadline = Clock::now() +
                                std::chrono::milliseconds(testPostUnlinkPauseMs_);
                            while (Clock::now() < deadline &&
                                   !recoveryStopRequested_.load(
                                       std::memory_order_acquire))
                                SDL_Delay(1);
                        }
                        // Every full batch becomes crash-durable without
                        // waiting for the entire directory scan to end.
                        syncRecoveredRemovals(false);
                    } else {
                        ++recoveryErrors_;
                    }
                }
            }
            ::close(fd);
            if (recoveryStopRequested_.load(std::memory_order_acquire)) {
                recoveryCancellationObserved_ = true;
                break;
            }
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        recoveryTimeMs_ += elapsed;
        ++recoverySteps_;
        if (entries > recoveryMaxStepEntries_)
            recoveryMaxStepEntries_ = entries;
        if (elapsed > recoveryMaxStepMs_)
            recoveryMaxStepMs_ = elapsed;
    }

    enum class FailStage { None, Read, Fence, Map };
    enum class FailSync { None, File, Directory };
    static FailSync testSyncFailureFromEnv() {
        const char* stage = std::getenv("XYO_SHOT_ASYNC_TEST_FAIL_SYNC");
        if (!stage) return FailSync::None;
        if (std::strcmp(stage, "file") == 0) return FailSync::File;
        if (std::strcmp(stage, "dir") == 0) return FailSync::Directory;
        return FailSync::None;
    }
    static std::string parentPath(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) return ".";
        if (slash == 0) return "/";
        return path.substr(0, slash);
    }
    bool syncContainingDirectory(const std::string& finalName) {
        // On Linux, rename only persists across a crash after the parent
        // directory's metadata has also been synchronized. A failure here
        // happens AFTER rename, so the final file may already be visible.
        const std::string parent = parentPath(finalName);
        const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            std::fprintf(stderr,
                "[async PPM] cannot open parent directory for fsync: %s\n",
                parent.c_str());
            ++syncFailures_;
            return false;
        }
        const bool forced = testFailSync_ == FailSync::Directory;
        const bool ok = !forced && ::fsync(fd) == 0;
        const bool closeOk = ::close(fd) == 0;
        if (!ok || !closeOk) {
            std::fprintf(stderr,
                "[async PPM] directory fsync failed AFTER rename: %s%s\n",
                parent.c_str(), forced ? " (injected)" : "");
            ++syncFailures_;
            return false;
        }
        ++syncedDirectories_;
        return true;
    }
    static FailStage testFailureStageFromEnv() {
        const char* stage = std::getenv("XYO_SHOT_ASYNC_TEST_FAIL_STAGE");
        if (!stage) return FailStage::None;
        if (std::strcmp(stage, "read") == 0) return FailStage::Read;
        if (std::strcmp(stage, "fence") == 0) return FailStage::Fence;
        if (std::strcmp(stage, "map") == 0) return FailStage::Map;
        return FailStage::None;
    }
    using Clock = std::chrono::steady_clock;
    static double elapsedMs(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
    }
    static bool isOne(const char* variable) {
        const char* value = std::getenv(variable);
        return value && value[0] == '1';
    }

    static bool cpuBufferReuseFromEnv() {
        const char* value = std::getenv("XYO_SHOT_ASYNC_BUFFER_POOL");
        return value && value[0] == '1';
    }

    static bool rgbReadbackFromEnv() {
        const char* value = std::getenv("XYO_SHOT_ASYNC_RGB");
        return value && value[0] == '1';
    }

    // Fault-injection only; unset in production. Delays and simulated
    // writer failure verify GL buffer handoff and cleanup under races.
    static size_t testCountFromEnv(const char* name) {
        const char* value = std::getenv(name);
        if (!value || !*value) return 0;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value && *end == '\0' && parsed > 0 &&
               parsed <= 10000 ? static_cast<size_t>(parsed) : 0;
    }

    static unsigned testWriteDelayFromEnv() {
        const char* value = std::getenv("XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS");
        if (!value || !*value) return 0;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end != value && *end == '\0' && parsed > 0 &&
               parsed <= 1000 ? static_cast<unsigned>(parsed) : 0;
    }

    bool reclaimWrittenPbos() {
        if (!directMapped_) return true;
        std::deque<Buffer> completed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed.swap(completedPbos_);
        }
        if (completed.empty()) return !failed_.load();
        GLint oldPbo = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
        for (const Buffer& buffer : completed) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer.id);
            if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE) {
                std::fprintf(stderr, "[async PPM] direct PBO unmap failed\n");
                failed_ = true;
            }
            free_.push_back(buffer);
            --borrowedPbos_;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
        return !failed_.load();
    }

    bool collectReady(bool wait) {
        if (pending_.empty()) return true;
        const Pending& oldest = pending_.front();
        const auto waitStarted = std::chrono::steady_clock::now();
        const GLenum result = testFailStage_ == FailStage::Fence
            ? GL_WAIT_FAILED
            : glClientWaitSync(
                oldest.fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                wait ? GLuint64(5'000'000'000ULL) : 0);
        if (wait) {
            ++gpuWaits_;
            gpuWaitMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - waitStarted).count();
        }
        if (!wait && result == GL_TIMEOUT_EXPIRED) return true;
        if (result != GL_ALREADY_SIGNALED &&
            result != GL_CONDITION_SATISFIED) {
            std::fprintf(stderr,
                "[async PPM] GL fence wait failed: status=0x%x\n",
                unsigned(result));
            failed_ = true;
            return false;
        }

        GLint oldPbo = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, oldest.buffer.id);
        const size_t length = size_t(oldest.width) * oldest.height *
                              (oldest.packedRgb ? 3 : 4);
        const auto mapStart = stageProfile_ ? Clock::now() : Clock::time_point{};
        void* mapped = testFailStage_ == FailStage::Map
            ? nullptr
            : glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(length), GL_MAP_READ_BIT);
        if (stageProfile_) {
            ++pboMaps_;
            pboMapMs_ += elapsedMs(mapStart);
        }
        if (!mapped) {
            std::fprintf(stderr,
                "[async PPM] GL PBO mapping failed: size=%zu\n", length);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
            failed_ = true;
            return false;
        }
        Image image;
        image.filename = oldest.filename;
        image.width = oldest.width;
        image.height = oldest.height;
        image.packedRgb = oldest.packedRgb;
        if (directMapped_) {
            // CPU pointer remains valid while this buffer is mapped, and
            // the renderer never enqueues another read into this PBO until
            // worker completion is observed and we unmap it here.
            image.mapped = static_cast<const uint8_t*>(mapped);
            image.mappedBuffer = oldest.buffer;
            ++borrowedPbos_;
            if (borrowedPbos_ > maxBorrowedPbos_)
                maxBorrowedPbos_ = borrowedPbos_;
            directlyMappedBytes_ += length;
        } else {
            if (recycleCpuBuffers_) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!freeCpuBuffers_.empty()) {
                    image.pixels = std::move(freeCpuBuffers_.back());
                    freeCpuBuffers_.pop_back();
                    ++cpuReuses_;
                }
            }
            if (image.pixels.capacity() < length) ++cpuAllocations_;
            image.pixels.resize(length);
            const auto copyStart = stageProfile_ ? Clock::now() : Clock::time_point{};
            std::memcpy(image.pixels.data(), mapped, length);
            if (stageProfile_) {
                cpuCopyMs_ += elapsedMs(copyStart);
                cpuCopyBytes_ += length;
            }
            if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
                return false;
            }
            free_.push_back(oldest.buffer);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));

        glDeleteSync(oldest.fence);
        pending_.pop_front();

        // Fault injection for the narrow handoff race: the writer may
        // fail after mapping the PBO but before the image is enqueued.
        if (testEnqueueDelayMs_ && directMapped_)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(testEnqueueDelayMs_));

        // Deterministic fault injection for the narrow mapped-PBO
        // ownership gap. The background writer failure race is nondeterministic
        // by nature; fail at this boundary to prove its cleanup invariant.
        if (testAbortBeforeEnqueue_ && image.mapped) {
            std::fprintf(stderr,
                "[async PPM] injected mapped PBO abort before enqueue\n");
            failed_ = true;
        }

        // Bounded queue prevents runaway memory use if disk writes are slow.
        std::unique_lock<std::mutex> lock(mutex_);
        if (jobs_.size() >= kMaxQueuedImages) {
            ++queueWaits_;
            const auto queueWaitStarted = std::chrono::steady_clock::now();
            space_.wait(lock, [this] {
                return jobs_.size() < kMaxQueuedImages || failed_;
            });
            queueWaitMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - queueWaitStarted).count();
        }
        if (failed_) {
            // This image was never sent to the writer. If it holds a mapped
            // PBO, no worker completion will arrive to reclaim it. Unmap on
            // the GL thread now rather than leaking until context teardown.
            lock.unlock();
            if (image.mapped) {
                GLint oldPbo = 0;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, image.mappedBuffer.id);
                if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE)
                    std::fprintf(stderr,
                        "[async PPM] abandoned mapped PBO unmap failed\n");
                glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(oldPbo));
                free_.push_back(image.mappedBuffer);
                --borrowedPbos_;
                ++abortedBeforeEnqueue_;
            }
            return false;
        }
        jobs_.push_back(std::move(image));
        if (jobs_.size() > maxQueued_)
            maxQueued_ = jobs_.size();
        lock.unlock();
        ready_.notify_one();
        return true;
    }

    bool writeImage(const Image& image) {
        // Explicit opt-in for atomic publication: create a unique temporary
        // sibling with O_EXCL, flush/close the COMPLETE PPM, then rename it
        // over the destination on the same filesystem. A failed write must
        // not leave a truncated final PPM or clobber a previous valid one.
        std::string tempPath;
        std::vector<char> tempName;
        FILE* file = nullptr;
        if (atomicFiles_) {
            tempPath = image.filename + ".part.XXXXXX";
            tempName.assign(tempPath.begin(), tempPath.end());
            tempName.push_back('\0');
            const int fd = ::mkstemp(tempName.data());
            if (fd < 0) return false;
            // The lock remains held until fclose. Another process's
            // opt-in stale recovery can never remove an active writer
            // that participates in this protocol.
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                ::close(fd);
                ::unlink(tempName.data());
                return false;
            }
            file = ::fdopen(fd, "wb");
            if (!file) {
                ::close(fd);
                ::unlink(tempName.data());
                return false;
            }
            ++atomicStagedFiles_;
        } else {
            file = fopenUtf8(image.filename.c_str(), "wb");
            if (!file) return false;
        }
        bool ok = std::fprintf(file, "P6\n%d %d\n255\n",
                               image.width, image.height) >= 0;
        const size_t rgbRowBytes = size_t(image.width) * 3;
        const uint8_t* source = image.mapped
            ? image.mapped : image.pixels.data();
        std::vector<uint8_t> row;
        if (!image.packedRgb) row.resize(rgbRowBytes);
        for (int y = 0; y < image.height && ok; ++y) {
            if (testFailWriteRow_ && static_cast<size_t>(y) == testFailWriteRow_) {
                std::fprintf(stderr,
                    "[async PPM] injected write failure at row %d\n", y);
                ok = false;
                break;
            }
            // GL reads bottom-up regardless of GL_RGB vs GL_RGBA;
            // PPM needs top-down rows, matching SDL_RenderReadPixels.
            const uint8_t* src = source +
                size_t(image.height - 1 - y) * image.width *
                (image.packedRgb ? 3 : 4);
            if (!image.packedRgb) {
                for (int x = 0; x < image.width; ++x) {
                    const size_t i = size_t(x);
                    row[3*i] = src[4*i];
                    row[3*i + 1] = src[4*i + 1];
                    row[3*i + 2] = src[4*i + 2];
                }
            }
            const uint8_t* bytes = image.packedRgb ? src : row.data();
            ok = std::fwrite(bytes, 1, rgbRowBytes, file) == rgbRowBytes;
            if (ok && atomicFiles_ && testStagePauseMs_ && y == 3) {
                // Test-only SIGKILL checkpoint. Flush an *incomplete*
                // image and keep its exclusive flock alive while paused.
                // The test controller observes the partial file, then
                // forcibly kills the writer; production never pauses here.
                ok = std::fflush(file) == 0;
                if (ok) {
                    std::fprintf(stderr,
                        "[async PPM] test staged partial PPM ready\n");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(testStagePauseMs_));
                }
            }
        }
        if (durableFiles_ && ok) {
            // stdio buffers must reach the kernel BEFORE fdatasync/fsync.
            if (std::fflush(file) != 0) {
                std::fprintf(stderr, "[async PPM] flush before fsync failed\n");
                ++syncFailures_;
                ok = false;
            } else if (testFailSync_ == FailSync::File ||
                       ::fsync(::fileno(file)) != 0) {
                std::fprintf(stderr,
                    "[async PPM] file fsync failed before rename%s\n",
                    testFailSync_ == FailSync::File ? " (injected)" : "");
                ++syncFailures_;
                ok = false;
            } else {
                ++syncedFiles_;
            }
        }
        if (std::fclose(file)) ok = false;
        if (atomicFiles_) {
            if (ok && ::rename(tempName.data(), image.filename.c_str()) == 0) {
                ++atomicPublishedFiles_;
                if (durableFiles_ && !syncContainingDirectory(image.filename))
                    ok = false;
            } else {
                // Close first, then remove even a partially written PPM.
                // A pre-existing final image remains untouched.
                ::unlink(tempName.data());
                ++atomicDiscardedFiles_;
                ok = false;
            }
        }
        return ok;
    }

    void writeLoop() {
        for (;;) {
            Image image;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopped_ || !jobs_.empty();
                });
                if (jobs_.empty() && stopped_) return;
                image = std::move(jobs_.front());
                jobs_.pop_front();
            }
            space_.notify_one();
            if (testWriteDelayMs_)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(testWriteDelayMs_));
            const auto writeStart = stageProfile_ ? Clock::now() : Clock::time_point{};
            // Failure injection happens at the start of the first
            // post-threshold write; previously written images remain valid.
            const bool simulateFailure =
                testFailAfterSaved_ && saved_.load() >= testFailAfterSaved_;
            const bool written = !simulateFailure && writeImage(image);
            if (image.mapped) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    completedPbos_.push_back(image.mappedBuffer);
                }
                completedPboReady_.notify_one();
            }
            if (stageProfile_) {
                writerMs_ += elapsedMs(writeStart);
                ++writerCalls_;
            }
            if (recycleCpuBuffers_ && !image.pixels.empty()) {
                // The worker owns the vector until the file is closed;
                // transfer ownership only after the write. The producer
                // can never access a buffer that the worker is still using.
                std::lock_guard<std::mutex> lock(mutex_);
                if (freeCpuBuffers_.size() < kMaxReusableCpuBuffers) {
                    freeCpuBuffers_.push_back(std::move(image.pixels));
                    if (freeCpuBuffers_.size() > maxFreeCpuBuffers_)
                        maxFreeCpuBuffers_ = freeCpuBuffers_.size();
                }
            }
            if (!written) {
                std::fprintf(stderr, "[async PPM] cannot save %s\n",
                             image.filename.c_str());
                failed_ = true;
                space_.notify_all();
            } else {
                ++saved_;
            }
        }
    }

    void stopWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        ready_.notify_all();
        space_.notify_all();
        completedPboReady_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    SDL_GLContext ownerContext_ = nullptr;
    std::deque<Pending> pending_;
    std::vector<Buffer> free_;
    std::mutex mutex_;
    std::condition_variable ready_, space_, completedPboReady_;
    std::deque<Image> jobs_;
    std::deque<Buffer> completedPbos_;
    // All access to this buffer pool is serialized by mutex_. No GL calls
    // are made on the writer thread.
    std::vector<std::vector<uint8_t>> freeCpuBuffers_;
    bool stopped_ = false, closed_ = false;
    std::atomic<bool> failed_{false};
    size_t submitted_ = 0, submittedBytes_ = 0;
    size_t maxPending_ = 0, maxQueued_ = 0;
    size_t cpuAllocations_ = 0, cpuReuses_ = 0;
    size_t maxFreeCpuBuffers_ = 0;
    size_t gpuWaits_ = 0, queueWaits_ = 0;
    double gpuWaitMs_ = 0.0, queueWaitMs_ = 0.0;
    double captureMs_ = 0.0, glReadCallMs_ = 0.0;
    double pboMapMs_ = 0.0, cpuCopyMs_ = 0.0, writerMs_ = 0.0;
    size_t pboMaps_ = 0, cpuCopyBytes_ = 0, writerCalls_ = 0;
    size_t borrowedPbos_ = 0, maxBorrowedPbos_ = 0;
    size_t pboCreated_ = 0, pboDeleted_ = 0, abortedBeforeEnqueue_ = 0;
    size_t directlyMappedBytes_ = 0, pboWorkerWaits_ = 0;
    double pboWorkerWaitMs_ = 0.0;
    std::atomic<size_t> saved_{0};
    const bool rgbCapture_;
    const bool recycleCpuBuffers_;
    const bool stageProfile_;
    const bool directMapped_;
    const unsigned testWriteDelayMs_;
    const size_t testFailAfterSaved_, testEnqueueDelayMs_;
    const FailStage testFailStage_;
    const bool testAbortBeforeEnqueue_;
    const bool atomicFiles_, durableFiles_, recoverStale_;
    const bool recoveryThreadEnabled_;
    const bool seriesLockEnabled_;
    const size_t testRecoveryPauseMs_, testRecoveryEntryDelayMs_;
    const size_t testPreUnlinkPauseMs_, testPostUnlinkPauseMs_;
    const bool testFailRecoverySync_;
    const size_t testAfterRecoveryBatchPauseMs_;
    bool testRecoveryHandshakeDone_ = false;
    std::atomic<bool> recoveryPreUnlinkEntered_{false};
    std::atomic<bool> recoveryPostUnlinkEntered_{false};
    std::atomic<bool> recoveryStopRequested_{false};
    bool recoveryCancellationObserved_ = false;
    size_t recoveryPreUnlinkSkipped_ = 0;
    bool recoveryStopRecorded_ = false;
    size_t recoveryPendingImagesAtStop_ = 0;
    double recoveryJoinMs_ = 0.0;
    SDL_Thread* recoveryThread_ = nullptr;
    std::string recoveryFilename_;
    bool recoveryThreadStarted_ = false, recoveryThreadJoined_ = false;
    double recoverySubmitMs_ = 0.0;
    bool seriesLockAttempted_ = false;
    int seriesLockFd_ = -1;
    bool recoveryStarted_ = false, recoveryFinished_ = false;
    DIR* recoveryDir_ = nullptr;
    std::string recoveryBegin_;
    size_t recoveryExpectedLength_ = 0;
    size_t recoverySteps_ = 0, recoveryMaxStepEntries_ = 0;
    double recoveryMaxStepMs_ = 0.0;
    size_t recoveryChecked_ = 0, recoveryRemoved_ = 0,
           recoveryRecent_ = 0, recoveryLocked_ = 0,
           recoveryUnsafe_ = 0, recoveryErrors_ = 0, recoveryEntries_ = 0;
    size_t recoveryDirFsyncs_ = 0, recoverySyncedRemovals_ = 0,
           recoverySyncFailures_ = 0, recoveryMaxUnsyncedRemovals_ = 0;
    bool recoverySyncFailed_ = false;
    double recoveryTimeMs_ = 0.0;
    const size_t testFailWriteRow_, testStagePauseMs_;
    const FailSync testFailSync_;
    size_t atomicStagedFiles_ = 0, atomicPublishedFiles_ = 0,
           atomicDiscardedFiles_ = 0;
    size_t syncedFiles_ = 0, syncedDirectories_ = 0, syncFailures_ = 0;
    std::thread worker_;
};

} // namespace xyo::render
