# `app_life`: MistWarp/V8 vs xyo-cpp/LLVM machine code

Date: 2026-11-10. Host: i7-12700, Node.js v26.4.0; same ScratchBench source-derived Life `.sb3` and same CPU affinity (CPU8) for the benchmark pairs.

## Source and optimized-code artifacts

- MistWarp generated JavaScript: `build/life-codecompare-20261110/mist-generated-1.js`; generated function `fun2_k_app_life`.
- V8 TurboFan **OSR** disassembly: `build/life-codecompare-20261110/mist-v8-assembly.log` (obtained with `node --print-opt-code --print-opt-code-filter=fun2_k_app_life`). The printed code identifies `kind=TURBOFAN_JS`, `name=fun2_k_app_life`, `Instructions (size = 12920)`.
- xyo-cpp LLVM ORC JIT disassembly: `build/life-codecompare-20261110/xyo-llvm-assembly.txt`, from `build/perf-life-jit-objects/with-cache-2.o`, symbol `xyo_script_0_p2` (ELF symbol size **32,065 bytes**).
- LLVM optimized IR: `build/perf-life-ir-analysis-20261110/app_life_full.ll`.
- Parse statistics: `build/life-codecompare-20261110/asm-stats.txt`; parsing script `build/life-codecompare-20261110/parse_asm.py`.
- Benchmark evidence: `build/life-codecompare-20261110/benchmark-paired.{json,log}`.

## Equivalent logical operation

One neighbor accumulation in the `repeat 3 × repeat 3` Life cell loop:

`neighbor = ((y + dy + 12) % 12) * 12 + ((x + dx + 12) % 12) + 1`, then `sum += list[neighbor]`.

V8's generated JS uses `+`, `% 12`, `(index | 0) - 1`, and direct `b0.value[...]` arrays. It includes Scratch conversion and modulo semantics, but TurboFan specializes the observed input to Smi integers and `PACKED_SMI_ELEMENTS` with guards/deopt on violations. The sample V8 machine code reads an array element via `movq [rax+rcx*8+0xf]` then Smi-untags with `sarq rcx,32` and adds with `addl`. V8 `% 12` is optimized with magic division (`0x55555556`, `mull`, `imull`, `subl`) in its integer path; positive/negative Scratch modulo adjustment still includes two `vdivsd` operations in the inspected code path.

xyo-cpp emits numeric SSA values that frequently remain doubles. Its modulo-by-12 path verifies finite numeric ranges, converts `double → signed i32`, converts back to `double` to prove exactness (`vcvttsd2si`, `vcvtsi2sd`, `vucomisd`), then uses a multiplicative inverse (`0xaaaaaaab`, `imulq`, `shrq`, `subl`) and converts back to `double`. For inputs outside the small/exact range it also provides the signed i64 or `fmod` path. The two source-level modulos compose the index and feed a list length check, tag check, and subsequent f64 accumulation.

## Static disassembly region comparison

| Region | MistWarp / V8 | xyo-cpp / LLVM |
|---|---:|---:|
| Representative machine-code offset | `+0x20e` to `+0x3bb` | `+0x12df` to `+0x1629` |
| Static instructions within this span | **105** | **190** |
| Conditional branch instructions | **17** | **25** |
| Call instructions inside span | **0** | **2** (`fmod` fallback) |
| `double`→integer conversions | **0** | **5** |
| Integer multiplication instructions | **9** | **4** |
| Floating-point division instructions | **2** | **2** (fallback) |

These are **statically selected corresponding regions**, including cold/alternate code paths; they are *not* identical basic-block boundaries or an executed-instruction trace. xyo-cpp's two `fmod` calls are the cold fallback and should not be claimed as repeated helper calls on normal in-range inputs. The V8 code is **OSR** code, and its generated logic uses speculative guards/deoptimization. Do not interpret a ratio of static instructions as a runtime instructions-per-iteration ratio or a compiler-quality score.

The full machine-code sizes, 12,920 bytes for V8's OSR function and 32,065 bytes for xyo-cpp's LLVM procedure, are likewise **not apples-to-apples**: OSR entry/scope and deoptimization machinery differ.

### Data dependence / high-latency paths

- Both implementations must compute two dependent remainders, multiply the first by 12, add the second, access the array, and then accumulate the cell. This serial dependency limits out-of-order overlap.
- V8's observed-integer fast path keeps index and accumulator as Smi/int32, with map guard, array-length guard and deopt exits. Its common inner region contains **no ordinary function call**.
- xyo-cpp has more branch/convert edges between the two modulos and the eventual index: check numeric/NaN and representable range → convert f64 to int → exactness check (int to f64) → integer remainder → convert back to f64 → combine → convert to integer index. The shape and numeric tags cannot safely be removed without additional facts.
- The code sizes and static branches point to **value-range/type propagation and loop-wide integer representation** as the next optimization target, rather than eliminating the already-rare `xyo_to_num` external calls. Direct dynamic branch-frequency or instruction-retirement measurements were not taken here.

## Same-core paired performance

Six alternating A/B pairs on the exact same project, CPU8, 30fps, xyo HotSpot native Release and MistWarp Node VM (with JS compiler enabled), both output checksum `53`:

- xyo-cpp: median **27.839** diagnostic ops/ms
- MistWarp: median **40.3115** diagnostic ops/ms
- Paired geometric-mean MistWarp/xyo-cpp: **1.45069×**; MistWarp wins **6/6**.

This specific diagnostic workload's units are not the aggregate ScratchBench score.

## Important correction to earlier advice

`y + dy + 12`, with y∈[0,11] and dy∈[-1,1], lies in **[11,24]**, not [0,23]. A single subtraction `n >= 12 ? n-12 : n` is **wrong** for `n=24` (which must wrap to 0). A value-range-proven rewrite could use `n>=24 ? n-24 : (n>=12 ? n-12 : n)`; or on the original pre-shift coordinate `p∈[-1,12]`, use `p<0 ? p+12 : (p>=12 ? p-12 : p)`. Either change needs a sound proof of integer ranges and Scratch modulo semantics before IR lowering.

The repository source was not modified for this examination, only analysis artifacts were created. No commit or push.
