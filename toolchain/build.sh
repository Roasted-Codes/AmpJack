#!/bin/bash
# Builds the Xbox 360 plugins (xbox360/out/) in the toolchain container, building the
# container first if it isn't there yet. Run from anywhere:
#   bash toolchain/build.sh
# The build switches (xbox360/build.sh, docs/DEVELOPMENT.md) are passed through, e.g.
#   XW_DEV=1 bash toolchain/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=xw-toolchain
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
    echo "building the toolchain image (once; about an hour)"
    docker build -t $IMAGE "$ROOT/toolchain"
fi
docker run --rm -e XW_STAGED -e XA_SLOT -e XW_SLOT -e XA_HOOKS -e XW_DEV -e XW_TEAM -v "$ROOT":/src -w /src/xbox360 $IMAGE bash build.sh
