// CPU-only GL VM sizing tests: no context, allocations or GPU required.
#include <cstdio>
#include <cstdint>
#include <limits>

#include "gpu/gpu_analysis.hpp"
#include "gpu/gpu_isa.hpp"

namespace {
int checks = 0, failures = 0;
#define CHECK_PLAN(c) do { ++checks; if (!(c)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
constexpr uint64_t MiB = uint64_t(1024) * 1024;
xyo::gpu::ExecRequest request(uint32_t lanes = 1000) {
    xyo::gpu::ExecRequest r;
    r.lanes = lanes;
    r.codeLen = 2;
    r.maxPerLane = 32;
    r.nWb = 6;
    return r;
}
uint64_t fixed(const xyo::gpu::ExecRequest& r) {
    return uint64_t(r.codeLen) * 4 + uint64_t(28 + r.nWb) * 4 +
           uint64_t(r.nGlobals ? r.nGlobals : 1) * 4;
}
uint64_t perLane(const xyo::gpu::ExecRequest& r) {
    return uint64_t(56) * r.maxPerLane + 8 +
           uint64_t(xyo::gpu::laneOutStride(r.nWb)) * 4;
}
} // namespace

int test_gl_chunk_plan_main() {
    using xyo::gpu::planGlChunks;
    checks = failures = 0;
    auto r = request();
    auto p = planGlChunks(r, 0, 0);
    CHECK_PLAN(p.ok && p.lanesPerChunk == r.lanes);
    CHECK_PLAN(p.hostOutputBytes == uint64_t(r.lanes) * 64);
    CHECK_PLAN(p.chunkBufferBytes == fixed(r) + perLane(r) * r.lanes);
    p = planGlChunks(r, 0, 0, 17);
    CHECK_PLAN(p.ok && p.lanesPerChunk == 17);
    CHECK_PLAN((r.lanes + p.lanesPerChunk - 1) / p.lanesPerChunk == 59);
    CHECK_PLAN(r.lanes % p.lanesPerChunk == 14); // final partial chunk
    CHECK_PLAN(!planGlChunks(r, 0, 0, 17, false).ok);
    CHECK_PLAN(planGlChunks(r, 0, 0, 2000, false).lanesPerChunk == r.lanes);

    r = request(1000000);
    p = planGlChunks(r, 0, 0);
    CHECK_PLAN(p.ok && p.lanesPerChunk < r.lanes);
    CHECK_PLAN(p.hostOutputBytes == 64000000);
    CHECK_PLAN(p.chunkBufferBytes <= 256 * MiB);
    CHECK_PLAN(p.chunkBufferBytes + perLane(r) > 256 * MiB);
    CHECK_PLAN(!planGlChunks(r, 0, 0, 0, false).ok);
    r.nWb = 128;
    CHECK_PLAN(!planGlChunks(r, 0, 0).ok); // 564 MB full host laneOut
    r.lanes = uint32_t((512 * MiB) / (141 * 4));
    p = planGlChunks(r, 0, 0);
    CHECK_PLAN(p.ok && p.hostOutputBytes <= 512 * MiB);
    ++r.lanes;
    CHECK_PLAN(!planGlChunks(r, 0, 0).ok);

    r = request(1u << 24);
    r.nWb = 128;
    CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r.nWb = 0;
    r.lanes = 1000000;
    r.maxPerLane = 2048;
    p = planGlChunks(r, 0, 0);
    CHECK_PLAN(p.ok && p.lanesPerChunk > 0);
    CHECK_PLAN(p.chunkBufferBytes <= 256 * MiB);

    r = request(1000);
    CHECK_PLAN(planGlChunks(r, 0, 1).lanesPerChunk == 64);
    CHECK_PLAN(!planGlChunks(r, 0, 1, 0, false).ok);
    CHECK_PLAN(planGlChunks(r, 0, UINT32_MAX).lanesPerChunk == r.lanes);
    CHECK_PLAN(planGlChunks(r, 1536 * 5, 0).lanesPerChunk == 5);
    CHECK_PLAN(!planGlChunks(r, 1535, 0).ok); // not even one record lane
    CHECK_PLAN(!planGlChunks(r, 135, 0).ok);  // fixed params exceed SSBO
    r.nGlobals = 10000;
    CHECK_PLAN(!planGlChunks(r, 39999, 0).ok);

    r = request(1);
    const uint64_t one = fixed(r) + perLane(r);
    p = planGlChunks(r, 0, 0, 0, true, 512 * MiB - one);
    CHECK_PLAN(p.ok && p.chunkBufferBytes == 512 * MiB);
    CHECK_PLAN(!planGlChunks(r, 0, 0, 0, true, 512 * MiB - one + 1).ok);
    CHECK_PLAN(!planGlChunks(r, 0, 0, 0, true, UINT64_MAX).ok);
    r = request(1000);
    p = planGlChunks(r, 0, 0, 1, true, 256 * MiB - fixed(r) - perLane(r));
    CHECK_PLAN(p.ok && p.lanesPerChunk == 1 && p.chunkBufferBytes == 256 * MiB);
    CHECK_PLAN(!planGlChunks(r, 0, 0, 1, true,
                             256 * MiB - fixed(r) - perLane(r) + 1).ok);
    r.nGlobals = UINT32_MAX;
    CHECK_PLAN(!planGlChunks(r, 0, 0).ok); // fixed input alone cannot fit

    // Restrict output storage to expose the conservative vertex-count bound
    // independently from the 256 MiB budget (both limits must hold).
    r = request(1000000);
    r.maxPerLane = 2048;
    p = planGlChunks(r, 0, 0);
    CHECK_PLAN(p.ok && uint64_t(p.lanesPerChunk) * r.maxPerLane <=
                           uint64_t(2147483647) / 12);
    // At these caps the byte budget is always stricter than the vertex bound.
    CHECK_PLAN(!planGlChunks(r, 0, 0, 0, false).ok);

    r = request();
    r.lanes = 0; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r.lanes = (1u << 24) + 1; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r = request(); r.maxPerLane = 0; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r.maxPerLane = 2049; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r = request(); r.nSlots = 97; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r = request(); r.nWb = 129; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r = request(); r.codeLen = 0; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r.codeLen = 3; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r.codeLen = 131074; CHECK_PLAN(!planGlChunks(r, 0, 0).ok);
    r = request(); r.nSlots = 96; r.nWb = 128; r.codeLen = 131072;
    CHECK_PLAN(planGlChunks(r, 0, 0).ok);

    std::printf("GL chunk plan: %d checks, %d failures\n", checks, failures);
    return failures;
}
