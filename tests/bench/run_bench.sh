#!/usr/bin/env bash
# xyo-cpp: ペン描画ベンチの一括実行。
#   tests/bench/run_bench.sh [frames] [outdir]
# CPU ラスタ（XYO_PEN_CPU=1）と GL バックエンド（既定）を同一ワークロードで
# 比較し、各ワークロードの PPM を outdir に保存する。
# 参照実装は CPU 経路（TurboWarp 互換の画素モデル）。
set -u
BIN="${XYO_PEN_BENCH:-./build/tests/xyo-pen-bench}"
FRAMES="${1:-10}"
OUT="${2:-/tmp/xyo-pen-bench}"
DRIVER="${XYO_BENCH_DRIVER:-offscreen}"
WORKLOADS="${XYO_BENCH_WORKLOADS:-sparse_dots dense_dots scanline tri_fill dense_alpha opaque_overdraw random_lines short_strokes wide_brush}"
mkdir -p "$OUT"
echo "# backend: cpu = XYO_PEN_CPU=1 (CPU ラスタ), gl = OpenGL ペン層"
printf '%-18s %-4s %10s %10s %10s %12s\n' workload be feed_ms frame_ms total_ms stroke_rate
for w in $WORKLOADS; do
  for m in cpu gl; do
    env_extra=""
    [ "$m" = cpu ] && env_extra="XYO_PEN_CPU=1"
    line=$(timeout 900 env SDL_VIDEODRIVER="$DRIVER" SDL_AUDIODRIVER=dummy \
      XYO_PENPROF=1 $env_extra "$BIN" --workload "$w" --frames "$FRAMES" \
      --shot "$OUT/$w.$m.ppm" 2>/dev/null | grep BENCH)
    feed=$(echo "$line" | sed -n 's/.*feed=\([0-9.]*\)ms.*/\1/p')
    frame=$(echo "$line" | sed -n 's/.*frame=\([0-9.]*\)ms.*/\1/p')
    total=$(echo "$line" | sed -n 's/.*total=\([0-9.]*\)ms.*/\1/p')
    rate=$(echo "$line" | sed -n 's/.*stroke_rate=\([0-9.]*\)\/s.*/\1/p')
    printf '%-18s %-4s %10s %10s %10s %12s\n' "$w" "$m" "$feed" "$frame" \
      "$total" "$rate"
  done
done
echo "# PPM: $OUT/<workload>.<backend>.ppm"
