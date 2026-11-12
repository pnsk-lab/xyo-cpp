#!/usr/bin/env bash
# Benchmark the current xyo-cpp working tree, including uncommitted changes,
# on performance cores only. Requires Linux, taskset, Bun, CMake, and Ninja.
set -Eeuo pipefail

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'HELP'
Usage: ./tools/run-scratch-bench-pcore.sh [options]

Options:
  --runs N           Performance-suite repetitions (default: 3)
  --cpus LIST        Override auto-detected P cores (e.g. 8-15 or 8,10,12)
  --jobs N           Maximum concurrent build jobs (default: 2)
  --no-build         Use an already-built binary in the script's build dir
  --out DIR          Results directory, relative to scratch-bench/ or absolute
  -h, --help         Show this help

The script uses the local source tree (including uncommitted changes), builds
Release + LLVM JIT, and runs scratch-bench performance / HotSpot / headless.
It does not download an xyo release or change any source files.
HELP
}

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
bench_dir="$repo_root/scratch-bench"
build_dir="$bench_dir/.cache/runtimes/xyo-build/pcore-local"
binary="$build_dir/xyo"
runs=3
jobs=2
cpus=""
out=""
do_build=1

positive_int() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }

while (( $# )); do
  case "$1" in
    --runs|--jobs|--cpus|--out)
      opt="$1"
      (( $# >= 2 )) || die "$opt requires a value"
      val="$2"
      shift 2
      case "$opt" in
        --runs) runs="$val" ;;
        --jobs) jobs="$val" ;;
        --cpus) cpus="$val" ;;
        --out) out="$val" ;;
      esac
      ;;
    --no-build) do_build=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done

positive_int "$runs" || die "--runs must be a positive integer"
positive_int "$jobs" || die "--jobs must be a positive integer"
for cmd in taskset bun sha256sum git; do
  command -v "$cmd" >/dev/null 2>&1 || die "missing executable: $cmd"
done
[[ -d "$bench_dir" ]] || die "scratch-bench directory not found"

# Alder Lake/Raptor Lake: P cores have significantly higher maximum
# frequencies than E cores. Include all online CPUs >= 90% of the maximum.
# Fail closed if frequency data is unavailable or cannot distinguish cores.
detect_pcores() {
  local file cpu freq max=0
  local -a ids=() speeds=()
  for file in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq; do
    [[ -f "$file" ]] || continue
    cpu="${file#/sys/devices/system/cpu/cpu}"
    cpu="${cpu%%/*}"
    [[ -e "/sys/devices/system/cpu/cpu$cpu/online" ]] &&
      [[ "$(cat "/sys/devices/system/cpu/cpu$cpu/online")" == 1 ]] || {
        [[ -e "/sys/devices/system/cpu/cpu$cpu/online" ]] && continue
      }
    read -r freq < "$file"
    [[ "$freq" =~ ^[0-9]+$ ]] || continue
    ids+=("$cpu")
    speeds+=("$freq")
    (( freq > max )) && max="$freq"
  done
  (( ${#ids[@]} > 0 && max > 0 )) || return 1
  local threshold=$(( max * 90 / 100 )) result="" i
  for i in "${!ids[@]}"; do
    if (( speeds[i] >= threshold )); then
      result+="${result:+,}${ids[i]}"
    fi
  done
  [[ -n "$result" ]] || return 1
  printf '%s\n' "$result" | tr ',' '\n' | sort -n | paste -sd, -
}

if [[ -z "$cpus" ]]; then
  cpus="$(detect_pcores)" || die "could not detect P cores; specify --cpus explicitly"
fi
[[ "$cpus" =~ ^[0-9,-]+$ ]] || die "invalid CPU list: $cpus"
taskset -c "$cpus" true 2>/dev/null ||
  die "CPU set '$cpus' is not permitted by the current CPU affinity/cgroup"

if (( do_build )); then
  for cmd in cmake ninja; do
    command -v "$cmd" >/dev/null 2>&1 || die "missing build executable: $cmd"
  done
  cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DXYO_JIT=ON \
    -DXYO_BUILD_TESTS=OFF -DXYO_SDL2=ON
  cmake --build "$build_dir" --target xyo -j "$jobs"
fi
[[ -x "$binary" ]] || die "no executable at $binary (remove --no-build)"

if [[ -z "$out" ]]; then
  out="results/pcore-$(date -u +%Y%m%dT%H%M%SZ)-$$"
fi
case "$out" in
  /*) out_dir="$out" ;;
  *) out_dir="$bench_dir/$out" ;;
esac
[[ ! -e "$out_dir" ]] || die "output directory already exists: $out_dir"
mkdir -p "$out_dir"

{
  printf 'timestamp_utc=%s\n' "$(date -u +%FT%TZ)"
  printf 'git_head=%s\n' "$(git -C "$repo_root" rev-parse HEAD)"
  printf 'git_dirty=%s\n' "$(if [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=no)" ]]; then echo true; else echo false; fi)"
  printf 'binary=%s\n' "$binary"
  printf 'binary_sha256=%s\n' "$(sha256sum "$binary" | cut -d' ' -f1)"
  printf 'cpu_affinity=%s\n' "$cpus"
  printf 'cpu_model=%s\n' "$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n 1)"
  printf 'runs=%s\n' "$runs"
  printf 'build_jobs=%s\n' "$jobs"
  printf 'suite=performance\nvariant=hotspot\n'
} > "$out_dir/bench-info.txt"

printf 'P-core affinity: %s\nBinary: %s\nResults: %s\n' "$cpus" "$binary" "$out_dir"
cd "$bench_dir"
# The taskset affinity is inherited by Bun, spawned xyo, and helper processes.
# Avoid accidental release downloads by overriding any inherited XYO_SOURCE.
taskset -c "$cpus" env -u XYO_SOURCE -u XYO_SRC XYO_BIN="$binary" \
  bun src/cli.ts --runtime xyo-cpp --suite performance \
  --runs "$runs" --offline --no-variants --out "$out_dir" \
  2>&1 | tee "$out_dir/bench.log"
