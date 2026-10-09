#!/usr/bin/env python3
"""Render an MDL model from two pak files side by side to compare simplification."""

import argparse
import struct
import zlib

import numpy as np

from mdllod import Mdl
from meshsimp import read_pak


def render(mdl, frame, skin, size, yaw):
    pos = mdl.positions()[frame]
    c, s = np.cos(yaw), np.sin(yaw)
    x = pos[:, 0] * c - pos[:, 1] * s
    depth = pos[:, 0] * s + pos[:, 1] * c
    z = pos[:, 2]
    extent = max(np.ptp(x), np.ptp(z)) * 1.1
    sx = (x - (x.max() + x.min()) / 2) / extent * size + size / 2
    sy = size / 2 - (z - (z.max() + z.min()) / 2) / extent * size
    image = np.zeros((size, size), np.uint8)
    zbuf = np.full((size, size), np.inf)
    st = mdl.st.astype(np.float64)
    for corners, sts in zip(mdl.tri_xyz, mdl.tri_st):
        px, py = sx[corners], sy[corners]
        x0, x1 = int(max(px.min(), 0)), int(min(px.max() + 1, size))
        y0, y1 = int(max(py.min(), 0)), int(min(py.max() + 1, size))
        det = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2])
        if x0 >= x1 or y0 >= y1 or abs(det) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        w0 = ((py[1] - py[2]) * (gx - px[2]) + (px[2] - px[1]) * (gy - py[2])) / det
        w1 = ((py[2] - py[0]) * (gx - px[2]) + (px[0] - px[2]) * (gy - py[2])) / det
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        d = w0 * depth[corners[0]] + w1 * depth[corners[1]] + w2 * depth[corners[2]]
        u = w0 * st[sts[0], 0] + w1 * st[sts[1], 0] + w2 * st[sts[2], 0]
        v = w0 * st[sts[0], 1] + w1 * st[sts[1], 1] + w2 * st[sts[2], 1]
        sub = zbuf[y0:y1, x0:x1]
        hit = inside & (d < sub)
        sub[hit] = d[hit]
        u = np.clip(u.astype(int), 0, skin.shape[1] - 1)
        v = np.clip(v.astype(int), 0, skin.shape[0] - 1)
        image[y0:y1, x0:x1][hit] = skin[v[hit], u[hit]]
    return image


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paks", nargs="+", help="original pak, then one or more simplified paks")
    parser.add_argument("--model", required=True)
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--size", type=int, default=200)
    parser.add_argument("--suffix", action="append", default=[],
                        help="model name suffix to use in each simplified pak, e.g. _lod")
    args = parser.parse_args()

    base = read_pak(args.paks[0])
    palette = np.frombuffer(base["gfx/palette.lmp"], np.uint8).reshape(256, 3)
    models = [Mdl(base[args.model])]
    for i, pak in enumerate(args.paks[1:]):
        suffix = args.suffix[i] if i < len(args.suffix) else ""
        models.append(Mdl(read_pak(pak)[args.model[:-4] + suffix + ".mdl"]))
    first = models[0]
    skin = np.frombuffer(first.skins[4:4 + first.skinwidth * first.skinheight], np.uint8)
    skin = skin.reshape(first.skinheight, first.skinwidth)
    tiles = [render(m, args.frame, skin, args.size, 0.6) for m in models]
    write_png(args.output, palette[np.concatenate(tiles, axis=1)])


if __name__ == "__main__":
    main()
