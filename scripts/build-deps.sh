#!/usr/bin/env bash
# Build the two heavy dependencies out-of-tree: LLVM (from the pinned
# submodule) and alive2 (arm-tv branch, pinned submodule).
# Idempotent; safe to re-run after a failure.
#
# Layout (all under $BUILD_ROOT, default <repo>/build):
#   $BUILD_ROOT/llvm     LLVM build tree (also provides Target/*/​*Gen*.inc)
#   $BUILD_ROOT/alive2   alive2 build tree (static libs + alive-tv etc.)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_SRC="$ROOT/third_party/llvm-project"
ALIVE2_SRC="$ROOT/third_party/alive2"
BUILD_ROOT="${BUILD_ROOT:-$ROOT/build}"
LLVM_BUILD="$BUILD_ROOT/llvm"
ALIVE2_BUILD="$BUILD_ROOT/alive2"

if [ ! -f "$LLVM_SRC/llvm/CMakeLists.txt" ] || [ ! -f "$ALIVE2_SRC/CMakeLists.txt" ]; then
  echo "error: submodules not initialized; run:" >&2
  echo "  git submodule update --init --depth 1" >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin) JOBS="${JOBS:-$(sysctl -n hw.ncpu)}" ;;
  *)      JOBS="${JOBS:-$(nproc)}" ;;
esac

# ANTLR (needed by alive2's aslp bridge, build-time only): a complete jar and
# a version-matched C++ runtime. The devcontainer installs both under
# /opt/antlr; on macOS use homebrew. Override with ANTLR4_JAR / ANTLR4_PREFIX.
if [ -z "${ANTLR4_JAR:-}" ]; then
  for cand in /opt/antlr/antlr-complete.jar \
              "$(brew --prefix antlr 2>/dev/null || true)"/antlr-*-complete.jar \
              "$(brew --prefix antlr 2>/dev/null || true)"/libexec/antlr-*-complete.jar; do
    if [ -f "$cand" ]; then ANTLR4_JAR="$cand"; break; fi
  done
fi
[ -n "${ANTLR4_JAR:-}" ] || { echo "error: ANTLR jar not found; set ANTLR4_JAR" >&2; exit 1; }

PREFIX_PATH="${ANTLR4_PREFIX:-/opt/antlr}"
if command -v brew >/dev/null 2>&1; then
  PREFIX_PATH="$PREFIX_PATH;$(brew --prefix)"
fi

cmake -S "$LLVM_SRC/llvm" -B "$LLVM_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_EH=ON \
  -DLLVM_TARGETS_TO_BUILD="AArch64;RISCV;BPF" \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF
ninja -C "$LLVM_BUILD" -j "$JOBS"

cmake -S "$ALIVE2_SRC" -B "$ALIVE2_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_LLVM_UTILS=ON \
  -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm" \
  -DCMAKE_PREFIX_PATH="$PREFIX_PATH" \
  -DANTLR4_JAR_LOCATION="$ANTLR4_JAR"
ninja -C "$ALIVE2_BUILD" -j "$JOBS"

echo "deps built: $LLVM_BUILD, $ALIVE2_BUILD"
