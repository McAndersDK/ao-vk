#!/usr/bin/env python3
"""Where the game's time goes during its slow frames (tools/profile-hitches.sh): perf samples (`perf script -F
time,tid,ip`, wall-clock times) kept only inside the slow frames randy-vk.log names ("slow frame: N ms at
HH:MM:SS.mmm" - the frame ended then and took N ms), bucketed per thread by module and function. randy31.dll's
functions come from its linker map; other PE modules (the game's DLLs, Wine's) by their nearest export.
Usage: tools/profile-hitches.py samples.txt maps.txt randy-vk.log randy31.map [--top N] [--since HH:MM:SS]
"""
import argparse
import bisect
import collections
import datetime
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from importlib import import_module

demo = import_module("profile-demo")


def map_functions(path):
    """[(rva, name)] from an lld-link /MAP file (Rva+Base column, base 0x10000000)."""
    funcs = []
    for line in open(path, errors="replace"):
        m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8})", line)
        if m:
            funcs.append((int(m.group(2), 16) - 0x10000000, demo.short(m.group(1))))
    return sorted(funcs)


def pe_exports(path):
    """[(rva, name)] of a PE file's named exports ([] if it has none or can't be read)."""
    try:
        data = open(path, "rb").read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        magic = struct.unpack_from("<H", data, pe + 24)[0]
        dirs = pe + 24 + (96 if magic == 0x10B else 112)
        exp_rva, exp_size = struct.unpack_from("<II", data, dirs)
        if not exp_rva:
            return []
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        sec0 = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
        secs = [struct.unpack_from("<IIII", data, sec0 + 40 * i + 8) for i in range(nsec)]   # vsize, va, rawsize, raw

        def off(rva):
            for vsize, va, rawsize, raw in secs:
                if va <= rva < va + max(vsize, rawsize):
                    return rva - va + raw
            raise ValueError(rva)

        e = off(exp_rva)
        nfuncs, nnames, afuncs, anames, aords = struct.unpack_from("<IIIII", data, e + 20)
        out = []
        for i in range(nnames):
            name_rva = struct.unpack_from("<I", data, off(anames) + 4 * i)[0]
            ordinal = struct.unpack_from("<H", data, off(aords) + 2 * i)[0]
            rva = struct.unpack_from("<I", data, off(afuncs) + 4 * ordinal)[0]
            if exp_rva <= rva < exp_rva + exp_size:
                continue                                  # forwarder
            n = off(name_rva)
            name = data[n:data.index(b"\0", n)].decode("ascii", "replace")
            out.append((rva, demo.short(name)))
        return sorted(out)
    except (OSError, ValueError, struct.error):
        return []


def windows(log, since):
    """[(start, end)] wall-clock seconds of the slow frames in randy-vk.log (today's date; after `since`)."""
    today = datetime.date.today()
    out = []
    for line in open(log, errors="replace"):
        m = re.search(r"slow frame: (\d+) ms at (\d\d):(\d\d):(\d\d)\.(\d\d\d)", line)
        if not m:
            continue
        ms, hh, mm, ss, mss = (int(x) for x in m.groups())
        if since and f"{hh:02}:{mm:02}:{ss:02}" < since:
            continue
        end = datetime.datetime(today.year, today.month, today.day, hh, mm, ss, mss * 1000).timestamp()
        out.append((end - ms / 1000.0, end, ms, f"{hh:02}:{mm:02}:{ss:02}.{mss:03}"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("maps")
    ap.add_argument("log")
    ap.add_argument("randymap")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--since", default="")
    ap.add_argument("--min-ms", type=int, default=50)
    args = ap.parse_args()

    regs = demo.regions(args.maps)
    paths = {}
    for line in open(args.maps):
        parts = line.split(None, 5)
        if len(parts) == 6:
            paths.setdefault(pathlib.Path(parts[5].strip()).name, parts[5].strip())
    starts = [r[0] for r in regs]
    symbols = {}                                          # module -> ([rva...], [name...])

    def where(ip):
        i = bisect.bisect_right(starts, ip) - 1
        if i < 0 or ip >= regs[i][1]:
            return "(anonymous)", "?"
        base, _, name = regs[i]
        if name not in symbols:
            funcs = map_functions(args.randymap) if name.lower() == "randy31.dll" else \
                pe_exports(paths.get(name, "")) if re.search(r"\.(dll|exe|drv)$", name, re.I) else []
            symbols[name] = ([f[0] for f in funcs], [f[1] for f in funcs])
        rvas, names = symbols[name]
        j = bisect.bisect_right(rvas, ip - base) - 1
        return name, (names[j] if j >= 0 else f"+{ip - base:x}")

    wins = [w for w in windows(args.log, args.since) if w[2] >= args.min_ms]
    wstarts = [w[0] for w in wins]
    per_thread = collections.defaultdict(list)
    total_in = 0
    for line in open(args.samples):
        parts = line.replace(":", " ").split()
        if len(parts) < 3:
            continue
        try:
            tid, t, ip = int(parts[0]), float(parts[1]), int(parts[2], 16)
        except ValueError:
            continue
        i = bisect.bisect_right(wstarts, t) - 1
        if i >= 0 and t <= wins[i][1]:
            per_thread[tid].append(ip)
            total_in += 1
    print(f"{len(wins)} slow frames of {args.min_ms} ms or more; {total_in} samples inside them")
    for w in wins[:40]:
        print(f"  {w[3]}  {w[2]} ms")
    for tid, ips in sorted(per_thread.items(), key=lambda t: -len(t[1]))[:3]:
        n = len(ips)
        by_module, by_func = collections.Counter(), collections.Counter()
        for ip in ips:
            mod, fn = where(ip)
            by_module[mod] += 1
            by_func[f"{mod}!{fn}"] += 1
        print(f"thread {tid}: {n} samples")
        print("  by module: " + ", ".join(f"{m} {c * 100 / n:.1f}%" for m, c in by_module.most_common(10)))
        for f, c in by_func.most_common(args.top):
            print(f"  {c * 100 / n:5.1f}%  {f}")


if __name__ == "__main__":
    main()
