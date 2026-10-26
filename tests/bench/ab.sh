#!/usr/bin/env bash
# xyo-cpp: ペン描画ベンチの A/B 比較。
#   tests/bench/ab.sh <base-bin> <cand-bin> [frames] [reps]
# 同一ワークロード・同一ドライバで交互に実行し、total ms の中央値を比較する。
# 環境変数は XYO_BENCH_DRIVER / XYO_BENCH_MODE (cpu|gl) / XYO_BENCH_EXTRA。
set -u
BASE="${1:?base binary}"
CAND="${2:?cand binary}"
FRAMES="${3:-5}"
REPS="${4:-3}"
DRIVER="${XYO_BENCH_DRIVER:-offscreen}"
MODE="${XYO_BENCH_MODE:-gl}"
WORKLOADS="${XYO_BENCH_WORKLOADS:-sparse_dots dense_dots scanline tri_fill dense_alpha opaque_overdraw random_lines short_strokes wide_brush}"
EXTRA="${XYO_BENCH_EXTRA:-}"
ARGS="${XYO_BENCH_ARGS:-}"
if [ "$MODE" = cpu ]; then EXTRA="XYO_PEN_CPU=1 $EXTRA"; fi

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

printf '%-18s %10s %10s %8s\n' workload base_ms cand_ms speedup
for w in $WORKLOADS; do
  bt=""; ct=""
  for i in $(seq 1 "$REPS"); do
    t=$(timeout 900 env SDL_VIDEODRIVER="$DRIVER" SDL_AUDIODRIVER=dummy $EXTRA \
        "$BASE" --workload "$w" --frames "$FRAMES" $ARGS 2>/dev/null \
        | sed -n 's/.*total=\([0-9.]*\)ms.*/\1/p')
    bt="$bt $t"
    t=$(timeout 900 env SDL_VIDEODRIVER="$DRIVER" SDL_AUDIODRIVER=dummy $EXTRA \
        "$CAND" --workload "$w" --frames "$FRAMES" $ARGS 2>/dev/null \
        | sed -n 's/.*total=\([0-9.]*\)ms.*/\1/p')
    ct="$ct $t"
  done
  b=$(median $bt); c=$(median $ct)
  sp=$(awk -v b="$b" -v c="$c" 'BEGIN{printf "%.3f", (c>0? b/c : 0)}')
  printf '%-18s %10s %10s %8s\n' "$w" "$b" "$c" "$sp"
done
