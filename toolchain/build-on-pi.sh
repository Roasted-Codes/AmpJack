#!/usr/bin/env bash
# Builds OpenXeChain natively on a Raspberry Pi 5 (4 cores, 8 GB): about 3 h, most of it LLVM.
# Expects the OpenXeChain buildscript checkout (with submodules) in $OXC_ROOT/src and
# installs into $OXC_ROOT/sysroot. Default OXC_ROOT=/opt/openxechain (must be writable;
# on an overlayroot Pi, point it at the SD card and remount that rw first).
#
# Upstream build-toolchain.sh passes no memory limits to LLVM, so we make a patched copy:
#   - LLVM_PARALLEL_LINK_JOBS=1  (each clang/lld link can take several GB)
#   - LLVM_USE_LINKER=lld        (much lighter than GNU ld for the host build)
#   - PARALLEL=4                 (compile jobs; override with PARALLEL=N)
#   - keeps the build log on success (upstream deletes it)
#   - Newlib CPP = "clang -E" instead of clang-cpp: upstream only writes clang.cfg/clang++.cfg,
#     so clang-cpp runs without --sysroot and pulls in the host's /usr/include (fails on
#     Debian multiarch: 'bits/libc-header-start.h' not found)
#   - Newlib CFLAGS=-O2 (no -g): the fork's clang corrupts its heap when emitting DWARF into
#     COFF and segfaults in MCContext::reset (e.g. on w_gamma.c)
# SKIP_LLVM=1 reuses an already-installed cross compiler in $PREFIX (skips the ~3 h LLVM stage).
set -euo pipefail

ROOT="${OXC_ROOT:-/opt/openxechain}"
SRC="${ROOT}/src"
export PREFIX="${PREFIX:-${ROOT}/sysroot}"
export PARALLEL="${PARALLEL:-4}"
LOG_KEEP="${ROOT}/build.log"

cd "${SRC}"
sed -e 's|-DLLVM_INSTALL_TOOLCHAIN_ONLY=true \\|-DLLVM_INSTALL_TOOLCHAIN_ONLY=true -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_USE_LINKER=lld \\|' \
    -e 's|^rm -f build.log$|cp build.log "'"${LOG_KEEP}"'"|' \
    -e 's|CPP="${PREFIX}/bin/clang-cpp"|CPP="${PREFIX}/bin/clang -E -nostdlib -I${PWD}/xecorelibtmp/include"|' \
    -e 's|^    --enable-newlib-iconv >>|    CFLAGS="-O2" --enable-newlib-iconv >>|' \
    build-toolchain.sh > build-local.sh
grep -q LLVM_PARALLEL_LINK_JOBS build-local.sh && grep -q 'clang -E -nostdlib' build-local.sh \
    && grep -q 'CFLAGS="-O2" --enable-newlib-iconv' build-local.sh \
    || { echo "patch failed: upstream script changed"; exit 1; }

if [[ "${SKIP_LLVM:-0}" == 1 ]]; then
    [[ -x "${PREFIX}/bin/clang" && -x "${PREFIX}/bin/lld-link" ]] || { echo "SKIP_LLVM=1 but no cross compiler in ${PREFIX}"; exit 1; }
    sed -i '/^# Configure it first$/,/^rm -rf \* # Clear the build directory, ready for xecorelib$/c\rm -rf * # SKIP_LLVM: reuse installed cross compiler' build-local.sh
    grep -q 'SKIP_LLVM: reuse' build-local.sh || { echo "SKIP_LLVM patch failed"; exit 1; }
fi

mkdir -p "${PREFIX}"
# Toolchain must not see host include/lib paths
env -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LIBRARY_PATH bash build-local.sh
