#!/usr/bin/env bash
# build_wasm.sh — Compile MatchingEngine to WebAssembly via Emscripten
#
# Prerequisites:
#   emcc (Emscripten) on PATH — install via: https://emscripten.org/docs/getting_started/downloads.html
#
# Run from the project root:
#   bash matching_engine/build_wasm.sh
#
# Outputs:
#   docs/engine.js   (Emscripten JS glue)
#   docs/engine.wasm (compiled C++ engine)
#
# Both files must be committed to the repository for GitHub Pages to serve them.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WEB_DIR="$PROJECT_ROOT/docs"
SRC_DIR="$SCRIPT_DIR/src"

echo "Building WASM engine..."
echo "  Source: $SRC_DIR"
echo "  Output: $WEB_DIR/engine.{js,wasm}"

emcc \
  "$SRC_DIR/order_book.cpp" \
  "$SRC_DIR/matching_engine.cpp" \
  "$SRC_DIR/wasm_bridge.cpp" \
  -I "$SRC_DIR" \
  -std=c++20 \
  -O2 \
  -s EXPORTED_FUNCTIONS='["_wasm_submit_limit","_wasm_submit_market","_wasm_cancel","_wasm_tick","_wasm_run_benchmark","_wasm_reset"]' \
  -s EXPORTED_RUNTIME_METHODS='["UTF8ToString"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web \
  -s MODULARIZE=0 \
  -o "$WEB_DIR/engine.js"

echo "Done."
echo "  $WEB_DIR/engine.js"
echo "  $WEB_DIR/engine.wasm"
