#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${WEB_BUILD_DIR:-${repo_root}/build-web}"
site_dir="${WEB_SITE_DIR:-${build_dir}/site}"

command -v emcc >/dev/null 2>&1 || {
    echo "emcc was not found. Activate an Emscripten SDK first." >&2
    exit 1
}
command -v emcmake >/dev/null 2>&1 || {
    echo "emcmake was not found. Activate an Emscripten SDK first." >&2
    exit 1
}

emcmake cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSJIT_BUILD_HOST=OFF
emmake cmake --build "${build_dir}" --parallel

cmake -E make_directory "${site_dir}"
cmake -E copy "${repo_root}/web/index.html" "${site_dir}/index.html"
cmake -E copy "${repo_root}/web/app.js" "${site_dir}/app.js"
cmake -E copy "${repo_root}/web/styles.css" "${site_dir}/styles.css"
cmake -E copy "${build_dir}/xyo-web.js" "${site_dir}/xyo-web.js"
cmake -E copy "${build_dir}/xyo-web.wasm" "${site_dir}/xyo-web.wasm"

echo "Web bundle written to ${site_dir}"
