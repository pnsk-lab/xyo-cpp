// START-013: glibc allocator snapshot for opt-in AOT memory diagnostics.
// malloc_info/mallinfo2 track libc arenas and mmap allocations, not GC
// object semantics or all native allocator mappings. Never infer exact live
// Scratch memory from these figures alone.
#pragma once

#include <cstdio>
#include <cstring>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace xyo {

inline void reportAotMallocState(const char* phase) {
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
    const auto state = mallinfo2();
    std::fprintf(stderr,
        "[aot-malloc-diag] phase=%s arena=%zu mmap=%zu used=%zu "
        "free=%zu top_releasable=%zu\n",
        phase, size_t(state.arena), size_t(state.hblkhd),
        size_t(state.uordblks), size_t(state.fordblks),
        size_t(state.keepcost));
#else
    (void)phase;
#endif
#else
    (void)phase;
#endif

#if defined(__linux__)
    // RSS includes resident file/code mappings not counted by mallinfo2.
    // Print both to avoid incorrectly equating heap-used with RSS.
    if (std::FILE* file = std::fopen("/proc/self/status", "r")) {
        size_t rss = 0, highWater = 0, anon = 0, fileRss = 0;
        size_t shmem = 0;
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            if (std::sscanf(line, "VmRSS: %zu kB", &rss) == 1) continue;
            if (std::sscanf(line, "VmHWM: %zu kB", &highWater) == 1) continue;
            if (std::sscanf(line, "RssAnon: %zu kB", &anon) == 1) continue;
            if (std::sscanf(line, "RssFile: %zu kB", &fileRss) == 1) continue;
            (void)std::sscanf(line, "RssShmem: %zu kB", &shmem);
        }
        std::fclose(file);
        std::fprintf(stderr,
            "[aot-rss-diag] phase=%s rss_kib=%zu peak_kib=%zu "
            "anon_kib=%zu file_kib=%zu shmem_kib=%zu\n",
            phase, rss, highWater, anon, fileRss, shmem);
    }

    // smaps separates the regular [heap] arena from unnamed anonymous
    // mappings used by GC, JIT code, libc, and other subsystems.
    if (std::FILE* maps = std::fopen("/proc/self/smaps", "r")) {
        enum : unsigned { Unnamed, Heap, Stack, File, Other, Groups };
        size_t rssBy[Groups]{};
        size_t anonBy[Groups]{};
        unsigned group = Unnamed;
        char line[512];
        while (std::fgets(line, sizeof(line), maps)) {
            unsigned long long begin = 0, end = 0;
            char perms[5]{};
            if (std::sscanf(line, "%llx-%llx %4s",
                            &begin, &end, perms) == 3) {
                if (std::strstr(line, "[heap]")) group = Heap;
                else if (std::strstr(line, "[stack]")) group = Stack;
                else if (std::strchr(line, '/')) group = File;
                else if (std::strchr(line, '[')) group = Other;
                else group = Unnamed;
            } else if (std::strncmp(line, "Rss:", 4) == 0) {
                size_t kib = 0;
                if (std::sscanf(line, "Rss: %zu kB", &kib) == 1)
                    rssBy[group] += kib;
            } else if (std::strncmp(line, "Anonymous:", 10) == 0) {
                size_t kib = 0;
                if (std::sscanf(line, "Anonymous: %zu kB", &kib) == 1)
                    anonBy[group] += kib;
            }
        }
        std::fclose(maps);
        std::fprintf(stderr,
            "[aot-smaps-diag] phase=%s heap_rss=%zu "
            "unnamed_rss=%zu stack_rss=%zu file_rss=%zu other_rss=%zu "
            "heap_anon=%zu unnamed_anon=%zu file_anon=%zu\n",
            phase, rssBy[Heap], rssBy[Unnamed], rssBy[Stack],
            rssBy[File], rssBy[Other],
            anonBy[Heap], anonBy[Unnamed], anonBy[File]);
    }
#endif
}

} // namespace xyo
