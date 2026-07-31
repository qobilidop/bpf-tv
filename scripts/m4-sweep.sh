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

# REUSE=1 reuses baseline records (same binary + same input) from the
# previous run in $OUT -- makes an incremental re-sweep after a
# bucket-targeted change take minutes instead of re-solving everything
REUSE_FLAGS=()
if [ "${REUSE:-0}" = 1 ] && [ -f "$OUT/corpus-results.jsonl" ]; then
  cp "$OUT/corpus-results.jsonl" "$OUT/corpus-results.baseline.jsonl"
  REUSE_FLAGS=(--reuse "$OUT/corpus-results.baseline.jsonl")
fi

python3 "$ROOT/scripts/corpus_run.py" \
  ${REUSE_FLAGS[@]+"${REUSE_FLAGS[@]}"} \
  --corpus "$L/tools/testing/selftests/bpf/progs" \
  --clang "$BUILD/llvm/bin/clang" \
  --bpf-tv "$BUILD/alive2/bpf-tv/bpf-tv" \
  "${SHIM_FLAGS[@]}" \
  --cflag=-I"$ROOT/third_party/vendored" \
  --cflag=-I"$L/tools/lib" \
  --cflag=-I"$L/tools/include/uapi" \
  --cflag=-I"$L/include/uapi" \
  --cflag=-I"$L/tools/testing/selftests/bpf" \
  --cflag=-I"$L/tools/testing/selftests/bpf/libarena/include" \
  --cflag=-D__TARGET_ARCH_x86 \
  --cflag=-Wno-incompatible-function-pointer-types \
  --cflag=-Wno-implicit-function-declaration \
  --out "$OUT"
