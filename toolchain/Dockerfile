# OpenXeChain (open-source Xbox 360 toolchain) in a container, so nothing is installed on
# the host. Build once (LLVM: about an hour on 8 cores), then build the plugins with
# toolchain/build.sh from the repository root.
#
#   docker build -t xw-toolchain toolchain
#
# build-on-pi.sh and synthxex_dll_patch.py are from Jacckii/Xbox360-Series-Controllers-Bridge
# (toolchain/), GPL-3.0: the memory-safe wrapper for upstream build-toolchain.sh and the
# system-DLL patch for SynthXEX.
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang lld cmake ninja-build make gcc g++ git ca-certificates python3 python3-yaml \
        bzip2 gzip xz-utils tar unzip zip gawk sed grep findutils file binutils \
    && rm -rf /var/lib/apt/lists/*

# the buildscript and the submodule commits it pins (Oct 2025)
ARG OXC_BUILDSCRIPT=eed1fa65bf9577fd31625764b320a90182ea9ade
ENV OXC_ROOT=/opt/openxechain
# the submodules are listed with SSH URLs: fetch them over HTTPS
RUN git config --global url."https://github.com/".insteadOf "git@github.com:" \
    && git clone https://github.com/OpenXeChain/buildscript.git $OXC_ROOT/src \
    && cd $OXC_ROOT/src && git checkout $OXC_BUILDSCRIPT \
    && (git submodule update --init --recursive --depth 1 \
        || git submodule update --init --recursive)

COPY build-on-pi.sh synthxex_dll_patch.py /opt/openxechain/
ARG PARALLEL=6
RUN cd $OXC_ROOT && PARALLEL=$PARALLEL bash build-on-pi.sh \
    || (tail -80 $OXC_ROOT/src/build.log; exit 1)

# synthxex-patched: SynthXEX with the system-DLL patch (written for v0.0.5; the newer
# pinned version is tried first)
RUN set -e; cp -r $OXC_ROOT/src/synthxex /tmp/sx; cd /tmp/sx/src; \
    if ! python3 /opt/openxechain/synthxex_dll_patch.py; then \
        cd /tmp && rm -rf sx && git clone https://github.com/OpenXeChain/SynthXEX.git sx \
        && cd sx && git checkout v0.0.5 && cd src && python3 /opt/openxechain/synthxex_dll_patch.py; \
    fi; \
    cd /tmp/sx && gcc -O2 -Iinclude -DSYNTHXEX_VERSION='"v0.0.x-dllpatch"' \
        $(find src include -name '*.c') \
        -o $OXC_ROOT/sysroot/bin/synthxex-patched; \
    rm -rf /tmp/sx $OXC_ROOT/src/build

ENV OXC=/opt/openxechain/sysroot
WORKDIR /src
