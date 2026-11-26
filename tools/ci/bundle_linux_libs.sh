#!/bin/bash
# Bundle non-system shared libraries alongside xyo for portable releases.
# Usage: bundle_linux_libs.sh <xyo-binary> <pkg-lib-dir>
# Copies every DT_NEEDED lib except the glibc core set (libc/m/dl/pthread/rt,
# ld-linux, vdso), then the binary finds them via $ORIGIN rpath (set at build
# time with -DCMAKE_INSTALL_RPATH='$ORIGIN/lib'). The release build emits
# DT_RPATH (not DT_RUNPATH) via --disable-new-dtags so transitive deps that
# lack their own RUNPATH also resolve to lib/.
set -euo pipefail
BIN="$1"
LIBDIR="$2"
mkdir -p "$LIBDIR"
# System libs that are always present (kernel/glibc ABI stable). Never bundle.
SYS_RE='^(linux-vdso|ld-linux|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so)'
ldd "$BIN" | awk '/=>/ {print $3}' | grep -E '^/' | sort -u | while read -r lib; do
  base=$(basename "$lib")
  if echo "$base" | grep -qE "$SYS_RE"; then
    echo "skip system: $base"
    continue
  fi
  echo "bundle: $base"
  cp -L "$lib" "$LIBDIR/"
done

# Some libraries carry an absolute RUNPATH pointing at a distro-private
# subdirectory (e.g. libpulse.so.0 → /usr/lib/x86_64-linux-gnu/pulseaudio,
# libproxy → .../libproxy). That RUNPATH masks the executable's own RPATH
# for their children, so libpulsecommon/libpxbackend copied into lib/ would
# never be found on end-user machines. Replace every bundled library's
# RUNPATH with $ORIGIN so siblings in lib/ are found first.
if ! command -v patchelf >/dev/null 2>&1; then
  echo "error: patchelf is required to fix RUNPATH on bundled libs" >&2
  exit 1
fi
for f in "$LIBDIR"/*.so*; do
  [ -f "$f" ] || continue
  patchelf --set-rpath '$ORIGIN' "$f"
done

echo "bundled $(ls "$LIBDIR" | wc -l) libs into $LIBDIR"
