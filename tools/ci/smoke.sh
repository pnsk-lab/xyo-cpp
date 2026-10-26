#!/usr/bin/env bash
# CI 用スモークテスト。ビルド済みの xyo バイナリを実際に動かす。
#
#   tools/ci/smoke.sh run  build/xyo             # run → AOT 書き出し → 実行
#   tools/ci/smoke.sh run  build/xyo --jit-log   # 上記 + JIT のコンパイル数
#   tools/ci/smoke.sh play build/xyo             # headless play + PPM 出力
#
# 第3引数以降でプロジェクトを指定できる（既定 examples/play_demo.sb3）。
# 作業ファイルはリポジトリ直下の相対パスに作る（MSYS2 のパス変換を避ける）。
set -euo pipefail

usage() { sed -n '2,10p' "$0" >&2; exit 2; }
[ $# -ge 2 ] || usage

cmd=$1
bin=$2
shift 2
project=examples/play_demo.sb3
jit_log=0
for arg in "$@"; do
    case "$arg" in
        --jit-log) jit_log=1 ;;
        -*) usage ;;
        *) project=$arg ;;
    esac
done
[ -x "$bin" ] || { echo "smoke: '$bin' が実行できない" >&2; exit 2; }

work=smoke-tmp
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

exe_suffix=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe_suffix=.exe ;; esac

# 静的リンクでは ORC が dlsym で libcall を解決できないため、未解決シンボルが
# interp フォールバックの裏に隠れないよう stderr を検査する。
check_jit_errors() {
    if grep -q "JIT session error" "$1"; then
        echo "==== JIT のシンボル解決に失敗 ====" >&2
        cat "$1" >&2
        return 1
    fi
}

case "$cmd" in
run)
    echo "--- run ---"
    "$bin" run "$project" --frames 30
    if [ "$jit_log" = 1 ]; then
        echo "--- JIT でコンパイルされたスクリプト数 ---"
        XYO_JIT_VERBOSE=1 "$bin" run "$project" --frames 10 2>&1 \
            | grep -c "compiled" || true
    fi
    echo "--- AOT 書き出しと実行 ---"
    app="$work/app$exe_suffix"
    "$bin" aot "$project" -o "$app"
    "$app" run --frames 30 2>"$work/jit-err.txt"
    check_jit_errors "$work/jit-err.txt"
    ;;
play)
    echo "--- play（SDL dummy ドライバ） ---"
    export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    XYO_EXIT_FRAMES=10 XYO_SHOT="$work/shot.ppm" "$bin" play "$project"
    test -s "$work/shot.ppm"
    ls -l "$work/shot.ppm"
    ;;
*)
    usage
    ;;
esac

echo "smoke($cmd): OK"
