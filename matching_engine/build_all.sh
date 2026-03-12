#!/usr/bin/env bash
# build_all.sh — Build native engine server + WASM engine in one step.
#
# Usage (from repo root or matching_engine/):
#   bash matching_engine/build_all.sh [--native-only] [--wasm-only]
#
# Outputs:
#   matching_engine/build/engine_server   (native HTTP server)
#   matching_engine/web/engine.js         (WASM JS glue)
#   matching_engine/web/engine.wasm       (WASM binary)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

BUILD_NATIVE=true
BUILD_WASM=true

for arg in "$@"; do
  case $arg in
    --native-only) BUILD_WASM=false ;;
    --wasm-only)   BUILD_NATIVE=false ;;
  esac
done

# ── Native server ──────────────────────────────────────────────────────────────

if $BUILD_NATIVE; then
  echo "==> Building native engine server..."
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -Wno-dev --log-level=WARNING
  cmake --build "$BUILD_DIR" --parallel
  echo "    Output: $BUILD_DIR/engine_server"
  echo ""
fi

# ── WASM ───────────────────────────────────────────────────────────────────────

if $BUILD_WASM; then
  echo "==> Building WASM engine..."
  if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found. Install Emscripten: https://emscripten.org/docs/getting_started/downloads.html"
    echo "  Or build native only: bash build_all.sh --native-only"
    exit 1
  fi
  bash "$SCRIPT_DIR/build_wasm.sh"
  echo ""
fi

echo "==> Done."
if $BUILD_NATIVE; then echo "    Native server: $BUILD_DIR/engine_server"; fi
if $BUILD_WASM;   then echo "    WASM:          $SCRIPT_DIR/web/engine.{js,wasm}"; fi
