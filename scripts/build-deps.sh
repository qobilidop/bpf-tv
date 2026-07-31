#!/usr/bin/env bash
# Build the two heavy dependencies out-of-tree: LLVM and official alive2
# (both pinned submodules). Idempotent; safe to re-run after a failure.
#
# Layout (all under $BUILD_ROOT, default <repo>/build):
#   $BUILD_ROOT/llvm     LLVM build tree (also provides Target/*/​*Gen*.inc)
#   $BUILD_ROOT/alive2   alive2 build tree (static libs + alive-tv etc.)
#
# third_party/alive2-arm-tv is reference-only and is never built here.
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

# parallelism: bounded by memory as well as cores -- LLVM's largest
# translation units need ~3 GB each, and an over-parallel build inside
# a memory-capped VM gets OOM-killed (observed: docker VM at 8 GB)
case "$(uname -s)" in
  Darwin)
    NCPU=$(sysctl -n hw.ncpu)
    MEM_GB=$(($(sysctl -n hw.memsize) / 1073741824))
    ;;
  *)
    NCPU=$(nproc)
    MEM_GB=$(($(awk '/MemTotal/{print $2}' /proc/meminfo) / 1048576))
    ;;
esac
MEM_JOBS=$((MEM_GB / 3)); [ "$MEM_JOBS" -lt 1 ] && MEM_JOBS=1
DEFAULT_JOBS=$((NCPU < MEM_JOBS ? NCPU : MEM_JOBS))
JOBS="${JOBS:-$DEFAULT_JOBS}"

# ccache, when present, makes pin-bump rebuilds cheap
LAUNCHER_FLAGS=()
if command -v ccache >/dev/null 2>&1; then
  LAUNCHER_FLAGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache
                  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Z3: the devcontainer installs a pinned source build at /opt/z3; a native
# macOS setup can use homebrew. Override with Z3_PREFIX.
PREFIX_PATH="${Z3_PREFIX:-/opt/z3}"
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
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  "${LAUNCHER_FLAGS[@]}"
ninja -C "$LLVM_BUILD" -j "$JOBS"

cmake -S "$ALIVE2_SRC" -B "$ALIVE2_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_LLVM_UTILS=ON \
  -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm" \
  -DCMAKE_PREFIX_PATH="$PREFIX_PATH" \
  -DEXTERNAL_PROJECTS=bpf-tv \
  -DEXTERNAL_BPF_TV_SOURCE_DIR="$ROOT" \
  "${LAUNCHER_FLAGS[@]}"
ninja -C "$ALIVE2_BUILD" -j "$JOBS"

echo "built: $LLVM_BUILD, $ALIVE2_BUILD"
echo "bpf-tv binary: $ALIVE2_BUILD/bpf-tv/bpf-tv"
