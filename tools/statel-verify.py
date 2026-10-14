#!/usr/bin/env python3
"""Checks the placed objects' placement (rvk/statels.cpp) against the game: a playfield's statel pack (ao-assets
statels) and frame dumps taken in that playfield (Ctrl+Shift+F12: logs/rvk-frame-*.txt).

Each "T" line of a dump is a caster the game drew that frame: its world translation, world box, vertex and index
counts. A piece of ours with the same counts at the same spot (within 6 cm) must have the same box (within 0.15) -
the box turns with the piece, so it tests the rotation; each of the six orders of the three rotations is tried
(rvk uses qZ * qX * qY: zxy). "M" lines (remembered casters) are the caster cache's and say nothing of ours.

    tools/statel-verify.py <client>/randy-vk/statels/790.bin logs/rvk-frame-*.txt
"""

import math
import re
import struct
import sys

import numpy as np


def load(path):
    d = open(path, "rb").read()
    magic, version, playfield, nm, npc, ns, nt, nv, ni, ntx = struct.unpack_from("<8sII7I", d, 0)
    if magic != b"AOSTATEL" or version != 1:
        sys.exit(f"{path}: not a statel pack (version 1)")
    at = 44 + nt * 16
    meshes = [struct.unpack_from("<III6f", d, at + i * 36) for i in range(nm)]
    at += nm * 36
    pieces = [struct.unpack_from("<16fIIIIif6f", d, at + i * 112) for i in range(npc)]
    at += npc * 112
    statels = [struct.unpack_from("<3f3hHfII", d, at + i * 32) for i in range(ns)]
    return playfield, meshes, pieces, statels


def qmul(a, b):
    return (a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2])


def statel_matrix(s, order):
    x, y, z, yaw, pitch, roll, _, scale = s[:8]
    h = math.pi / 360.0
    q = {"y": (0, math.sin(yaw * h), 0, math.cos(yaw * h)), "x": (math.sin(pitch * h), 0, 0, math.cos(pitch * h)),
         "z": (0, 0, math.sin(roll * h), math.cos(roll * h))}
    a, b, c = order
    X, Y, Z, W = qmul(q[a], qmul(q[b], q[c]))
    m = np.array([[1 - 2 * (Y * Y + Z * Z), 2 * (X * Y + Z * W), 2 * (X * Z - Y * W), 0],
                  [2 * (X * Y - Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z + X * W), 0],
                  [2 * (X * Z + Y * W), 2 * (Y * Z - X * W), 1 - 2 * (X * X + Y * Y), 0], [0, 0, 0, 1]])
    m[:3, :3] *= scale
    m[3, :3] = (x, y, z)
    return m


def world_box(lo, hi, m):
    c = (np.array(lo) + np.array(hi)) / 2
    e = (np.array(hi) - np.array(lo)) / 2
    wc = c @ m[:3, :3] + m[3, :3]
    we = e @ np.abs(m[:3, :3])
    return wc - we, wc + we


LINE = re.compile(r"^T \w+: at \(([-\d.]+) ([-\d.]+) ([-\d.]+)\) box \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)\.\."
                  r"\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) v (\d+) i (\d+)")


def drawn(files):
    out = []
    for f in files:
        for line in open(f, errors="replace"):
            m = LINE.match(line)
            if m:
                v = [float(t) for t in m.groups()]
                out.append((np.array(v[0:3]), int(v[9]), int(v[10]), np.array(v[3:6]), np.array(v[6:9])))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    playfield, meshes, pieces, statels = load(sys.argv[1])
    lines = drawn(sys.argv[2:])
    print(f"playfield {playfield}: {len(statels)} placed; {len(lines)} drawn casters in {len(sys.argv) - 2} dumps")
    for order in ("zxy", "yxz", "xyz", "zyx", "yzx", "xzy"):
        ours = {}
        for s in statels:
            mesh = meshes[s[6]]
            m = statel_matrix(s, order)
            for k in range(mesh[2]):
                p = pieces[mesh[1] + k]
                w = np.array(p[:16]).reshape(4, 4) @ m
                lo, hi = world_box(p[22:25], p[25:28], w)
                ours.setdefault((p[17], p[19]), []).append((w[3, :3], lo, hi))
        same = other = 0
        for at, v, i, lo, hi in lines:
            near = [o for o in ours.get((v, i), []) if np.abs(o[0] - at).max() < 0.06]
            if not near:
                continue
            if any(np.abs(o[1] - lo).max() < 0.15 and np.abs(o[2] - hi).max() < 0.15 for o in near):
                same += 1
            else:
                other += 1
        print(f"  {order}{' (rvk)' if order == 'zxy' else '      '}: {same} drawn pieces as ours, {other} otherwise")


if __name__ == "__main__":
    main()
