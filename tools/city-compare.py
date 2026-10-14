#!/usr/bin/env python3
"""Compares two runs of tools/city-bench.sh: the pictures (pixels that differ, the largest difference) and the city
bench's render-thread sections (the mean of the timing runs in each directory), and the frame dumps' draw counts.

    tools/city-compare.py BASELINE_DIR NEW_DIR [--tolerance 8]

Pictures must be identical while the draw order is unchanged (P1, P2); with instancing (P3) a few pixels may differ
where equal depths resolve the other way - a count over the tolerance is printed, look at them (*-diff.png).
Needs Pillow and numpy.
"""
import argparse
import pathlib
import re

import numpy as np
from PIL import Image

PICTURES = ("city.bmp", "city-time.bmp", "shadow-hdr.bmp", "cache.bmp", "statel.bmp")


def sections(directory):
    """{section: ms} from the "city bench: cpu ms" lines, averaged over the timing runs; plus the caller's frame."""
    sums, runs = {}, 0
    for f in sorted(directory.glob("city-time*.txt")):
        text = f.read_text(errors="replace")
        m = re.search(r"city bench: cpu ms \(frame recording\):(.*?)\(avg of", text)
        if not m:
            continue
        runs += 1
        for part in m.group(1).split("|"):
            name, _, value = part.strip().rpartition(" ")
            if name:
                sums[name] = sums.get(name, 0.0) + float(value)
        g = re.search(r"city bench: gpu ms:.*total ([\d.]+)", text)
        if g:
            sums["(gpu total)"] = sums.get("(gpu total)", 0.0) + float(g.group(1))
        c = re.search(r"calling thread: frame ([\d.]+) ms", text)
        if c:
            sums["(calling thread frame)"] = sums.get("(calling thread frame)", 0.0) + float(c.group(1))
    return {k: v / runs for k, v in sums.items()} if runs else {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", type=pathlib.Path)
    ap.add_argument("new", type=pathlib.Path)
    ap.add_argument("--tolerance", type=int, default=8)
    args = ap.parse_args()

    print("pictures:")
    for name in PICTURES:
        a, b = args.base / name, args.new / name
        if not a.exists() or not b.exists():
            print(f"  {name}: missing")
            continue
        x = np.asarray(Image.open(a).convert("RGB")).astype(int)
        y = np.asarray(Image.open(b).convert("RGB")).astype(int)
        if x.shape != y.shape:
            print(f"  {name}: sizes differ {x.shape} {y.shape}")
            continue
        d = np.abs(x - y).max(axis=2)
        if not d.any():
            print(f"  {name}: identical")
            continue
        print(f"  {name}: {int((d > 0).sum())} pixels differ, {int((d > args.tolerance).sum())} by more than "
              f"{args.tolerance}, at most {int(d.max())}")
        Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).save(args.new / (name[:-4] + "-diff.png"))

    for name in ("city-dump.txt",):
        a, b = args.base / name, args.new / name
        if a.exists() and b.exists():
            count = lambda p: sum(1 for line in p.read_text(errors="replace").splitlines() if line.startswith("D "))
            print(f"frame dump: {count(a)} -> {count(b)} D lines")

    sa, sb = sections(args.base), sections(args.new)
    if sa and sb:
        print("render thread sections (ms a frame, mean of the timing runs):")
        width = max(len(k) for k in sa | sb)
        for k in list(sa) + [k for k in sb if k not in sa]:
            va, vb = sa.get(k), sb.get(k)
            fa = f"{va:7.2f}" if va is not None else "      -"
            fb = f"{vb:7.2f}" if vb is not None else "      -"
            delta = f"{vb - va:+7.2f}" if va is not None and vb is not None else ""
            print(f"  {k:<{width}} {fa} {fb} {delta}")


if __name__ == "__main__":
    main()
