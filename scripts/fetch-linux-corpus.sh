#!/usr/bin/env bash
# Fetch the kernel selftests corpus for M4 measurement.
#
# The linux tree is deliberately NOT a submodule (the one exception to
# the pin-as-submodule policy): even a blob-filtered clone carries
# gigabytes of history. Instead this script pins an exact commit and
# performs a depth-1 sparse checkout (~200 MB). See DECISIONS.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/linux"
LINUX_SHA="${LINUX_SHA:-8ba098e6b6ff0db8edf28528d1552be261af30d4}" # master 2026-07-31

if [ -d "$DEST/.git" ]; then
  echo "already present: $DEST"
  exit 0
fi

git clone --filter=blob:none --sparse --depth 1 \
  https://github.com/torvalds/linux.git "$DEST"
git -C "$DEST" sparse-checkout set \
  tools/testing/selftests/bpf tools/lib/bpf tools/include include/uapi
git -C "$DEST" log -1 --format='linux corpus at %H'
