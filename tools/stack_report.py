#!/usr/bin/env python3
"""Worst-case stack estimate for a ccs32clara build, from GCC's call graphs.

Build with the call graphs, then run from the repository root:
    make clean all EXTRAFLAGS="-fstack-usage -fcallgraph-info=su"
    python3 tools/stack_report.py [obj] [stm32_ccs]

Each object's .ci file gives every function's own frame and its direct calls. Calls through a pointer can't be seen
there, so INDIRECT below says what they reach. The report gives the deepest chain from main and from each interrupt
handler, adds them up as the interrupts can nest (an exception frame of 32 bytes each), and compares that with the RAM
left after data and bss. It also gives the depth of the EXI encoder and decoder entry points on their own.

It is an estimate: functions without a call graph (libopencm3, libgcc) count as 0 bytes, and indirect calls not
listed in INDIRECT are reported and not followed.
"""

import glob
import os
import re
import subprocess
import sys

PREFIX = os.environ.get("PREFIX", "arm-none-eabi")
RAM = 64 * 1024
EXCEPTION_FRAME = 32

# Calls through pointers: (caller, callees), as regular expressions on the demangled names.
INDIRECT = [
    (r"^Stm32Scheduler::Run", [r"Ms30Task", r"Ms100Task"]),
    (r"^(pev_runFsm|pevStateMachine_Mainfunction)", [r"stateFunction"]),
    (r"^(print|prints|printi)\(", [r"::PutChar\(char\)"]),
    (r"^putchar", [r"^Terminal::PutChar\(char\)"]),
    (r"^CanHardware::HandleRx", [r"^(CanMap|CanSdo)::HandleRx"]),
    (r"^CanHardware::ClearUserMessages", [r"^(CanMap|CanSdo)::HandleClear"]),
    (r"^CanHardware::RegisterUserMessage", [r"^Stm32Can::ConfigureFilters"]),
    (r"^(CanHardware::Send|CanMap::Send)", [r"^Stm32Can::Send\("]),
    (r"^CanMap::IterateCanMap", [r"PrintCanMap"]),
    (r"^Param::Change", [r"^Stm32Can::SetBaudrate"]),
    (r"^Terminal::Run", [r"\(Terminal\*, char\*\)", r"::PutChar\(char\)"]),
]

# Levels that can be active at once, lowest priority first (the NVIC priorities set in hwinit.cpp and stm32_can.cpp).
LEVELS = [
    ("main loop", [r"^main$"]),
    ("CAN receive, 0xF0", [r"^usb_lp_can_rx0_isr$", r"^can_rx1_isr$", r"^usb_hp_can_tx_isr$"]),
    ("scheduler (TIM4), 0xE0", [r"^tim4_isr$"]),
    ("SPI DMA, 0xD0", [r"^dma1_channel2_isr$"]),
]

EXI_ENTRIES = [r"^decode_dinExiDocument", r"^encode_dinExiDocument", r"^decode_iso1ExiDocument",
               r"^encode_iso1ExiDocument", r"^decode_appHandExiDocument"]


def demangle(names):
    out = subprocess.run([PREFIX + "-c++filt"], input="\n".join(names), capture_output=True, text=True,
                         check=True).stdout.splitlines()
    return dict(zip(names, out))


def load(obj_dir):
    frame, calls = {}, {}
    node = re.compile(r'node:\s*\{\s*title:\s*"([^"]+)"\s*label:\s*"([^"]*)"')
    edge = re.compile(r'edge:\s*\{\s*sourcename:\s*"([^"]+)"\s*targetname:\s*"([^"]+)"')
    for path in glob.glob(os.path.join(obj_dir, "*.ci")):
        text = open(path, encoding="latin-1").read()
        for title, label in node.findall(text):
            m = re.search(r"(\d+) bytes", label)
            frame[title] = max(frame.get(title, 0), int(m.group(1)) if m else 0)
        for a, b in edge.findall(text):
            calls.setdefault(a, []).append(b)
    return frame, calls


def main():
    obj_dir = sys.argv[1] if len(sys.argv) > 1 else "obj"
    elf = sys.argv[2] if len(sys.argv) > 2 else "stm32_ccs"
    frame, calls = load(obj_dir)
    if not frame:
        sys.exit("no call graphs: build with EXTRAFLAGS=\"-fstack-usage -fcallgraph-info=su\"")
    # titles are mangled names, with "file:" in front for static functions
    pretty = demangle(sorted({f.split(":")[-1] for f in frame}))
    plain = {f: pretty[f.split(":")[-1]] for f in frame}
    unresolved, cycles = set(), set()

    def targets(fn):
        result = []
        for callee in calls.get(fn, []):
            if callee != "__indirect_call":
                result.append(callee)
                continue
            rules = [t for c, ts in INDIRECT if re.search(c, plain[fn]) for t in ts]
            if not rules:
                unresolved.add(plain[fn])
            result += [f for f in frame if any(re.search(t, plain[f]) for t in rules)]
        return result

    memo = {}

    def deepest(fn, active):
        if fn in memo:
            return memo[fn]
        if fn in active:
            cycles.add(plain[fn])
            return 0, []
        active.add(fn)
        best = (0, [])
        for t in targets(fn):
            d = deepest(t, active)
            if d[0] > best[0]:
                best = d
        active.discard(fn)
        memo[fn] = (frame.get(fn, 0) + best[0], [fn] + best[1])
        return memo[fn]

    def show(path):
        return " > ".join(f"{plain[f]}({frame.get(f, 0)})" for f in path)

    total = 0
    print("Deepest stack per level (each level can interrupt the ones above it):")
    for i, (name, pats) in enumerate(LEVELS):
        roots = [f for f in frame if any(re.search(p, plain[f]) for p in pats)]
        if not roots:
            print(f"  {name}: not in the build")
            continue
        depth, path = max(deepest(r, set()) for r in roots)
        extra = EXCEPTION_FRAME if i else 0
        total += depth + extra
        print(f"  {name}: {depth} bytes" + (f" + {extra} exception frame" if extra else ""))
        print(f"    {show(path)}")

    size = subprocess.run([PREFIX + "-size", elf], capture_output=True, text=True, check=True).stdout
    _, data, bss = (int(x) for x in size.splitlines()[1].split()[:3])
    free = RAM - data - bss
    print(f"All levels at once: {total} bytes; RAM after data and bss: {free}; left: {free - total}")

    print("EXI entry points on their own:")
    for p in EXI_ENTRIES:
        for f in [f for f in frame if re.search(p, plain[f])]:
            depth, path = deepest(f, set())
            print(f"  {plain[f]}: {depth} bytes; deepest: {show(path[:6])}{' > ...' if len(path) > 6 else ''}")
    big = sorted(((n, plain[f]) for f, n in frame.items()), reverse=True)[:10]
    print("Largest frames: " + ", ".join(f"{name} {n}" for n, name in big))
    if cycles:
        print("Recursion (cut, counted once): " + ", ".join(sorted(cycles)))
    if unresolved:
        print("Indirect calls not followed: " + ", ".join(sorted(unresolved)))


if __name__ == "__main__":
    main()
