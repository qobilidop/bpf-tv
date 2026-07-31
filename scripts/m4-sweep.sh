#!/usr/bin/env bash
# M4 sweep: kernel selftests through clang -target bpf and bpf-tv.
# Usage:
#   ./scripts/m4-sweep.sh native      # macOS host (header shim)
#   ./scripts/m4-sweep.sh container   # inside the devcontainer (real
#                                     # Linux headers; the clean run)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-native}"
L="$ROOT/third_party/linux"

if [ "$MODE" = container ]; then
  BUILD="$ROOT/build-linux"
  OUT="$BUILD/eval-m4"
  SHIM_FLAGS=()
else
  BUILD="$ROOT/build"
  OUT="$BUILD/eval-m4"
  R="$("$BUILD/llvm/bin/clang" -print-resource-dir)"
  SHIM_FLAGS=(--cflag=-nostdinc "--cflag=-I$R/include"
              "--cflag=-I$BUILD/m4-shim")
fi

python3 "$ROOT/scripts/corpus_run.py" \
  --corpus "$L/tools/testing/selftests/bpf/progs" \
  --clang "$BUILD/llvm/bin/clang" \
  --bpf-tv "$BUILD/alive2/bpf-tv/bpf-tv" \
  "${SHIM_FLAGS[@]}" \
  --cflag=-I"$ROOT/third_party/vendored" \
  --cflag=-I"$L/tools/lib" \
  --cflag=-I"$L/tools/include/uapi" \
  --cflag=-I"$L/include/uapi" \
  --cflag=-I"$L/tools/testing/selftests/bpf" \
  --cflag=-D__TARGET_ARCH_x86 \
  --cflag=-Wno-incompatible-function-pointer-types \
  --cflag=-Wno-implicit-function-declaration \
  --out "$OUT"
