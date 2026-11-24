#!/bin/bash
# Shows how far the toolchain image build is (the LLVM stage), as a progress bar.
#   bash toolchain/progress.sh          once
#   bash toolchain/progress.sh watch    refresh every 10 s until it's done
# Looks into Docker's build container with a throwaway read-only alpine container.
probe() {
    docker run --rm --privileged --pid=host alpine sh -c '
        for p in /proc/[0-9]*; do
            [ "$(cat $p/comm 2>/dev/null)" = ninja ] && { P=${p#/proc/}; break; }
        done
        [ -z "$P" ] && { echo none; exit; }
        # the [done/total] counter ninja writes to the build log
        tr "\r" "\n" < /proc/$P/root/opt/openxechain/src/build.log \
            | grep -o "^\[[0-9]*/[0-9]*\]" | tail -1 | tr -d "[]" | tr "/" " "
    ' 2>/dev/null
}
show() {
    read -r done total <<<"$(probe)"
    if [ "$done" = none ] || [ -z "$total" ]; then
        if docker image inspect xw-toolchain >/dev/null 2>&1; then
            echo "toolchain: built"
            return 1
        fi
        echo "toolchain: LLVM isn't compiling right now (another stage, or no build running)"
        return 0
    fi
    local pct=$((done * 100 / total)) n
    n=$((pct / 2))
    printf "LLVM  [%-50s] %3d%%  (%d of %d steps)\n" "$(printf '#%.0s' $(seq 1 $n))" $pct $done $total
    echo "      after LLVM: newlib, compiler-rt, SynthXEX (about 10 more minutes)"
}
if [ "${1:-}" = watch ]; then
    while show; do sleep 10; done
else
    show
fi
