#!/usr/bin/env python3
"""Where rvk_demo's threads spend their time (tools/profile-demo.sh): perf samples (`perf script -F tid,ip`) of its
busiest threads bucketed by module (from /proc/PID/maps) and, inside rvk_demo.exe, by function (the linker's map,
build/linux-release/rvk_demo.map; names shortened to Class::Method).

Usage: tools/profile-demo.py samples.txt maps.txt [--threads N] [--top N]
"""
import argparse
import bisect
import collections
import pathlib
import re
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent


def short(name):
    """?Method@Class@ns@@... -> Class::Method (operators and templates left as they are)."""
    m = re.match(r"\?(\w+)@(\w+)@(?:\w+@)*@", name)
    if m:
        return f"{m.group(2)}::{m.group(1)}"
    m = re.match(r"\?(\w+)@@", name)
    return m.group(1) if m else name


def demo_functions(build):
    funcs = []
    for line in open(ROOT / "build" / build / "rvk_demo.map", errors="replace"):
        m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8,16})", line)
        if m:
            funcs.append((int(m.group(2), 16), short(m.group(1))))
    return sorted(funcs)


def regions(maps_path):
    """[(start, end, name)] of every file mapping, merged per file; a PE image (Wine maps only its header from the
    file, its sections are anonymous memory after it) to its SizeOfImage."""
    spans, paths = {}, {}
    for line in open(maps_path):
        parts = line.split(None, 5)
        if len(parts) < 6:
            continue
        a, b = (int(x, 16) for x in parts[0].split("-"))
        path = parts[5].strip()
        name = pathlib.Path(path).name
        lo, hi = spans.get(name, (a, b))
        spans[name] = (min(lo, a), max(hi, b))
        paths[name] = path
    for name, (lo, hi) in spans.items():
        if re.search(r"\.(dll|exe|drv)$", name, re.I):
            try:
                data = open(paths[name], "rb").read(4096)
                pe = struct.unpack_from("<I", data, 0x3C)[0]
                spans[name] = (lo, max(hi, lo + struct.unpack_from("<I", data, pe + 24 + 56)[0]))
            except (OSError, struct.error):
                pass
    return sorted((lo, hi, n) for n, (lo, hi) in spans.items())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("maps")
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--build", default="linux-release")
    ap.add_argument("--lines", type=int, default=0, help="the N hottest source lines (a build with debug info)")
    args = ap.parse_args()
    samples = collections.defaultdict(list)
    for line in open(args.samples):
        parts = line.split()
        if len(parts) >= 2:
            samples[int(parts[0])].append(int(parts[1], 16))
    regs = regions(args.maps)
    starts = [r[0] for r in regs]
    funcs = demo_functions(args.build)
    fstarts = [f[0] for f in funcs]
    exe = next((r for r in regs if r[2].lower() == "rvk_demo.exe"), None)
    for rank, (tid, ips) in enumerate(sorted(samples.items(), key=lambda t: -len(t[1]))[:args.threads]):
        by_module, by_func, by_ip = collections.Counter(), collections.Counter(), collections.Counter()
        for ip in ips:
            if exe and exe[0] <= ip < exe[1]:
                by_module["rvk_demo.exe"] += 1
                by_ip[ip - exe[0] + 0x400000] += 1
                i = bisect.bisect_right(fstarts, ip) - 1
                by_func[funcs[i][1] if i >= 0 else "?"] += 1
                continue
            i = bisect.bisect_right(starts, ip) - 1
            by_module[regs[i][2] if i >= 0 and ip < regs[i][1] else "(anonymous)"] += 1
        n = len(ips)
        print(f"thread {tid}: {n} samples")
        print("  by module: " + ", ".join(f"{m} {c * 100 / n:.1f}%" for m, c in by_module.most_common(8)))
        for f, c in by_func.most_common(args.top):
            print(f"  {c * 100 / n:5.1f}%  {f}")
        if args.lines and rank == 0 and by_ip:
            # Addresses to file:line (inlined code: the innermost frame), summed by line.
            exe_path = ROOT / "build" / args.build / "rvk_demo.exe"
            addrs = list(by_ip)
            out = subprocess.run(["llvm-symbolizer", "--obj=" + str(exe_path), "--no-inlines", "--relative-address"] +
                                 [hex(a - 0x400000) for a in addrs], capture_output=True, text=True).stdout
            blocks = [b.strip().splitlines() for b in out.strip().split("\n\n")]
            by_line = collections.Counter()
            for a, b in zip(addrs, blocks):
                loc = b[1] if len(b) > 1 else "?"
                loc = re.sub(r".*/randy-vk/", "", loc)
                loc = re.sub(r":\d+$", "", loc)      # (the column)
                by_line[(b[0] if b else "?", loc)] += by_ip[a]
            print(f"  hottest lines:")
            for (fn, loc), c in by_line.most_common(args.lines):
                print(f"  {c * 100 / n:5.1f}%  {loc}  ({short(fn)})")


if __name__ == "__main__":
    main()
