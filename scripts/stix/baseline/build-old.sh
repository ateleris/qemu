#!/usr/bin/env bash
# Rebuild the QEMU 5.2 STIX baseline (branch stix-emul-qemu-v5.2.0, tip
# 93a9824b2d) in an era-correct Ubuntu 20.04 container.
#
# Prerequisites:
#   - throwaway worktree:  git worktree add /tmp/qemu-stix-baseline 93a9824b2d
#   - docker available
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SRC=/tmp/qemu-stix-baseline
BIN="$SRC/build/sparc-softmmu/qemu-system-sparc"

docker build -t qemu52-baseline "$REPO_ROOT/scripts/stix/baseline/"

# Init submodules on the HOST: the worktree's .git file points at the main
# repo's gitdir by absolute path, which does not exist inside the container.
git -C "$SRC" submodule update --init meson ui/keycodemapdb \
    tests/fp/berkeley-softfloat-3 tests/fp/berkeley-testfloat-3

docker run --rm -v "$SRC:/src" -w /src qemu52-baseline bash -c '
  set -e
  mkdir -p build && cd build
  ../configure --target-list=sparc-softmmu --disable-slirp --disable-capstone \
               --disable-docs --disable-fdt --disable-guest-agent
  make -j"$(nproc)" qemu-system-sparc
'

echo "=== binary: $BIN"
"$BIN" --version
