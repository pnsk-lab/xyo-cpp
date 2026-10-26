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

# AOT 実行ファイルを動かす。失敗したら stderr を出し、同じオブジェクトを
# --aot で読む経路も試して「実行ファイル側かオブジェクト側か」を切り分ける。
run_aot() {
    app=$1
    if "$app" run --frames 30 2>"$work/aot-err.txt"; then
        check_jit_errors "$work/aot-err.txt"
        return 0
    fi
    echo "==== AOT 実行ファイルが失敗 ====" >&2
    cat "$work/aot-err.txt" >&2 || true
    echo "--- 同じオブジェクトを --aot で読む（切り分け） ---" >&2
    "$bin" aot "$project" --obj "$work/project.o" >/dev/null 2>&1 || true
    if [ -f "$work/project.o" ]; then
        "$bin" run "$project" --aot "$work/project.o" --frames 30 >&2 || true
    fi
    return 1
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
    run_aot "$app"
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
