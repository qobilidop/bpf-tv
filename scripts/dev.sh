#!/usr/bin/env bash
# Run a command inside the bpf-tv devcontainer (the configuration of
# record), via the devcontainer CLI (npm i -g @devcontainers/cli).
#
#   ./scripts/dev.sh bash scripts/build-deps.sh
#   ./scripts/dev.sh ninja -C build-linux/alive2 bpf-tv
#   ./scripts/dev.sh                              # interactive shell
#
# `devcontainer up` is idempotent: it builds the image and starts (or
# reuses) the container defined by .devcontainer/, honoring
# devcontainer.json (BUILD_ROOT=build-linux, etc.).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v devcontainer >/dev/null 2>&1; then
  echo "error: devcontainer CLI not found; install with:" >&2
  echo "  npm install -g @devcontainers/cli" >&2
  exit 1
fi

devcontainer up --workspace-folder "$ROOT" >/dev/null

if [ $# -eq 0 ]; then
  set -- bash
fi

exec devcontainer exec --workspace-folder "$ROOT" "$@"
