#!/bin/bash
# Bundle non-system shared libraries alongside xyo for portable releases.
# Usage: bundle_linux_libs.sh <xyo-binary> <pkg-lib-dir>
# Copies every DT_NEEDED lib except the glibc core set (libc/m/dl/pthread/rt,
# ld-linux, vdso), then the binary finds them via $ORIGIN rpath (set at build
# time with -DCMAKE_INSTALL_RPATH='$ORIGIN/lib'). The release build emits
# DT_RPATH (not DT_RUNPATH) via --disable-new-dtags so the transitive
# dependency closure (e.g. libQt6Qml.so.6 needing libQt6QmlModels.so.6) also
# resolves to lib/ instead of falling back to a mismatched system Qt.
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
echo "bundled $(ls "$LIBDIR" | wc -l) libs into $LIBDIR"
