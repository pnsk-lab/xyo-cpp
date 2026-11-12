# PERF-001 — 2026-11-09 15:17–15:30 JST

## Thread reachability stamping

The existing XYO_THREAD_RECLAIM_STAMP=1 branch avoids per-reclaim unordered_set duplicate detection by assigning a monotonic 64-bit retention epoch. No runtime source changes in this review.

Same frozen SDL Release binary, CPU14 fixed, 24 alternating pairs on original source-derived sched_clonechurn.sb3: OFF median 1068.1535 ops/ms; ON median 1086.7255 ops/ms; ON 20/24 wins; paired geometric ratio 1.02464879 (+2.46%). Raw build/perf-clone-stamp-ab.{json,log}.

Correctness: xyo-tests with ON 1/1 PASS (48.28s), OFF 1/1 PASS (45.37s); ScratchBench GUI parity ON and OFF both 2123/2123 (100%). Logs build/perf-clone-stamp-ctest-{on,off}.log and build/perf-clone-stamp-parity-{on,off}.log.

Another PERF-001 agent has already measured regression in other scheduler projects, with fanout about -1.70% and fanwait about -2.01%, so do NOT enable this flag globally. See docs/perf-thread-reclaim-smallset-2026-11-09.md and docs/perf-thread-reclaim-pmr-2026-11-09.md. Their source remains untouched.

## Exact synchronous daysSince2000 alternatives, rejected

Tried opt-in clock_gettime(CLOCK_REALTIME) and gettimeofday instead of std::chrono::system_clock::now, retaining 1ms current wall-time quantization. No cached time or coarser clock. Frozen headless Release executable build/perf-looptick-posix-frozen-xyo, SHA256 a0737f24c7dd41c2e20832263ed3fc93977aa0687f201547e4e88540fb74d071.

CPU14 fixed, same binary, 16 alternated order triples (0=chrono, 1=clock_gettime, 2=gettimeofday). Original sched_looptick median ops/ms 0:7396.8785, 1:7458.081, 2:7429.185. Paired geomean ON/OFF: 1:1.004481 (13/16 wins), 2:1.004620 (14/16 wins). Negative control sched_clonechurn median ops/ms 0:1072.8605, 1:1075.988, 2:1075.622; paired geomeans 1.002569 (8/16) and 1.001881 (7/16). Gains only ~0.45% at best. Raw results build/perf-looptick-posix-ab.{json,log}. All modes completed successfully.

Experiment completely reverted; src/rt/runtime_helpers.cpp has no working-tree diff. No parity run specifically on the rejected clock alternatives; do not promote them. SDL/DLSS source was left untouched. Temporary headless build used compile-time include workaround for a concurrently edited renderer header without altering its sources.

## Verdict

Default reclaim stamp remains OFF. daysSince2000 stays on the original std::chrono path. The much larger sched_looptick deficit (xyo ~7.4k vs historical MistWarp ~12.2k ops/ms) needs changes beyond slight wall-clock helper savings. Existing asynchronous 1ms shared-clock experiment trades time-observation semantics for speed and remains opt-in. PERF-001 47/47 remains unmet; no commits or pushes.
