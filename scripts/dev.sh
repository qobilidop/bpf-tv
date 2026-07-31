#!/usr/bin/env bash
# Run a command inside the bpf-tv devcontainer (the configuration of
# record). Builds the image from .devcontainer/Dockerfile if needed.
#
#   ./scripts/dev.sh ./scripts/build-deps.sh
#   ./scripts/dev.sh ninja -C build-linux/alive2 bpf-tv
#   ./scripts/dev.sh                              # interactive shell
#
# Uses plain docker (same path CI uses); VS Code users can instead
# "Reopen in Container" for the full devcontainer experience.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${BPF_TV_DEV_IMAGE:-bpf-tv-dev}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "building devcontainer image '$IMAGE'..." >&2
  docker build -t "$IMAGE" "$ROOT/.devcontainer"
fi

if [ $# -eq 0 ]; then
  set -- bash
fi

exec docker run --rm -it \
  -v "$ROOT:/work" \
  -w /work \
  -e BUILD_ROOT=/work/build-linux \
  "$IMAGE" "$@"
