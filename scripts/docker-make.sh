#!/usr/bin/env bash
# Runs `make <args>` inside the gpu-sim container (Verilator 5.020, same
# packages as CI). Build products go to a named Docker volume, not the repo.
#   scripts/docker-make.sh all
#   scripts/docker-make.sh test-rasterizer SEED=3
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
host_root="$root"
command -v cygpath >/dev/null 2>&1 && host_root="$(cygpath -w "$root")"

docker image inspect gpu-sim >/dev/null 2>&1 || docker build -t gpu-sim "$host_root/docker"
MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$host_root":/work -v gpu-sim-build:/build -w /work \
    gpu-sim make BUILD_DIR=/build "$@"
