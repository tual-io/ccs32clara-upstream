#!/bin/sh
# Builds the default (DIN only) configuration and the ISO 15118-2 ones, and reports flash, RAM, the EXI documents and
# a stack estimate for each (see doc/iso15118-2.md). Needs arm-none-eabi-gcc and python3; run from the repository
# root after "make get-deps". Leaves the last configuration built.
set -e
LOG="${TMPDIR:-/tmp}/ccs32clara_build.log"
ANALYSIS="-fstack-usage -fcallgraph-info=su"
for cfg in "" "SHARED_EXI_DOC=1" "ISO1=1" "ISO1=1 SHARED_EXI_DOC=1"; do
    flags=""
    case "$cfg" in *ISO1=1*) flags="$flags -DUSE_ISO1";; esac
    case "$cfg" in *SHARED_EXI_DOC=1*) flags="$flags -DUSE_SAME_DOC_FOR_ENCODER_AND_DECODER";; esac
    echo "=================== make ${cfg:-(default)}"
    make clean all $cfg EXTRAFLAGS="$ANALYSIS" > "$LOG" 2>&1 || { tail -40 "$LOG"; exit 1; }
    grep -i "warning" "$LOG" || true
    python3 tools/exi_memory_report.py stm32_ccs $flags
    python3 tools/stack_report.py obj stm32_ccs
done
