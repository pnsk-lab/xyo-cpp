#!/usr/bin/env bash
# Build the optional Linux Node N-API bridge to the Binaryen C API.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-"$root/build-wasm-hotspot-parity/binaryen-host.node"}"
node_binary="$(readlink -f "$(command -v node)")"
node_root="$(dirname "$(dirname "$node_binary")")"
include_dir="${NODE_INCLUDE_DIR:-"$node_root/include/node"}"

if [[ "$(uname -s)" != Linux ]]; then
  echo "Binaryen N-API adapter currently supports Linux only" >&2
  exit 1
fi
if [[ ! -f "$include_dir/node_api.h" || ! -f /usr/include/binaryen-c.h ]]; then
  echo "Node development headers or binaryen-c.h are missing" >&2
  exit 1
fi
mkdir -p "$(dirname "$output")"
"${CXX:-c++}" -std=c++20 -O2 -fPIC -shared \
  -I"$include_dir" \
  -DXYO_HAVE_BINARYEN_C=1 -DXYO_BINARYEN_DLOPEN=1 \
  "$root/web/binaryen-node/bridge.cpp" \
  "$root/src/wasm/binaryen_c_api.cpp" \
  -ldl -o "$output"
echo "$output"
