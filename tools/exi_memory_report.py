#!/usr/bin/env python3
"""Memory report for a ccs32clara build: flash and RAM, the largest RAM symbols, and the EXI documents.

Run after a build, from the repository root:
    python3 tools/exi_memory_report.py [stm32_ccs] [extra compiler flags, e.g. -DUSE_ISO1]

The sizes of the EXI document types are found by compiling a small probe with the same headers and reading the size
of an array per type, so they are right for the build's configuration (pass -DUSE_ISO1 etc. as for the build).
"""

import os
import subprocess
import sys
import tempfile

PREFIX = os.environ.get("PREFIX", "arm-none-eabi")
ELF = sys.argv[1] if len(sys.argv) > 1 else "stm32_ccs"
FLAGS = sys.argv[2:]

PROBE = r"""
#include "projectExiConnector.h"
char size_appHandEXIDocument[sizeof(struct appHandEXIDocument)];
char size_dinEXIDocument[sizeof(struct dinEXIDocument)];
char size_dinBodyType[sizeof(struct dinBodyType)];
char size_dinMessageHeaderType[sizeof(struct dinMessageHeaderType)];
char size_dinChargeParameterDiscoveryResType[sizeof(struct dinChargeParameterDiscoveryResType)];
char size_dinServiceDiscoveryResType[sizeof(struct dinServiceDiscoveryResType)];
char size_dinCurrentDemandResType[sizeof(struct dinCurrentDemandResType)];
char size_dinCertificateInstallationResType[sizeof(struct dinCertificateInstallationResType)];
char size_dinCertificateUpdateResType[sizeof(struct dinCertificateUpdateResType)];
char size_dinPaymentDetailsReqType[sizeof(struct dinPaymentDetailsReqType)];
#ifdef USE_ISO1
char size_iso1EXIDocument[sizeof(struct iso1EXIDocument)];
char size_iso1BodyType[sizeof(struct iso1BodyType)];
char size_iso1MessageHeaderType[sizeof(struct iso1MessageHeaderType)];
char size_iso1ServiceDiscoveryResType[sizeof(struct iso1ServiceDiscoveryResType)];
char size_iso1ChargeParameterDiscoveryResType[sizeof(struct iso1ChargeParameterDiscoveryResType)];
char size_iso1SAScheduleListType[sizeof(struct iso1SAScheduleListType)];
char size_iso1SalesTariffType[sizeof(struct iso1SalesTariffType)];
char size_iso1CurrentDemandResType[sizeof(struct iso1CurrentDemandResType)];
char size_iso1ChargeParameterDiscoveryReqType[sizeof(struct iso1ChargeParameterDiscoveryReqType)];
#endif
#if defined(USE_ISO1) || defined(USE_SAME_DOC_FOR_ENCODER_AND_DECODER)
char size_ExiDocUnion_type[sizeof(union ExiDocUnion_type)];
#endif
"""


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def symbol_sizes(path):
    sizes = {}
    for line in run([PREFIX + "-nm", "-S", "-C", path]).splitlines():
        parts = line.split()
        if len(parts) >= 4:
            sizes[" ".join(parts[3:])] = (int(parts[1], 16), parts[2])
    return sizes


def main():
    text, data, bss = (int(x) for x in run([PREFIX + "-size", ELF]).splitlines()[1].split()[:3])
    print(f"text {text}, data {data}, bss {bss}; RAM left for the stack: {64 * 1024 - data - bss} of 65536")

    syms = symbol_sizes(ELF)
    ram = sorted(((n, s) for s, (n, t) in syms.items() if t in "bBdD"), reverse=True)[:15]
    print("Largest RAM symbols:")
    for n, s in ram:
        print(f"  {n:6d}  {s}")
    docs = ["aphsDoc", "dinDocEnc", "dinDocDec", "ExiDocEnc", "ExiDocDec", "ExiDoc"]
    print("EXI documents in this build: " + ", ".join(f"{d} {syms[d][0]}" for d in docs if d in syms))

    with tempfile.TemporaryDirectory() as tmp:
        src, obj = os.path.join(tmp, "probe.c"), os.path.join(tmp, "probe.o")
        open(src, "w").write(PROBE)
        run([PREFIX + "-gcc", "-c", "-Os", "-mcpu=cortex-m3", "-mthumb", "-fno-common", "-Iexi", "-Iinclude",
             "-Iccs", "-DSTM32F1"] + FLAGS + ["-o", obj, src])
        probe = symbol_sizes(obj)
    print("EXI types (sizeof):")
    for s, (n, _) in sorted(probe.items()):
        print(f"  {n:6d}  {s[len('size_'):]}")


if __name__ == "__main__":
    main()
