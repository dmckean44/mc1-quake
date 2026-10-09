#!/usr/bin/env python3
"""Build reduced-polygon versions of the MDL models in a Quake pak file.

MDL skins are mapped per vertex, with back-facing triangles that touch a seam
vertex shifted by half the skin width. Each corner is therefore given one of
two skin coordinates (front or back) for the simplifier, and a collapse is
only accepted if the result can still be expressed that way.

By default the simplified models replace the originals. With --suffix _lod
they are stored as foo_lod.mdl instead, and the renderer switches to them for
models that are small on screen (see the r_lodsize cvar).
"""

import argparse
import fnmatch
import struct
import sys

import numpy as np

from meshsimp import Simplifier, read_pak, write_pak

HEADER = struct.Struct("<4si3f3ff3f8if")


class Mdl:
    def __init__(self, data):
        fields = HEADER.unpack_from(data)
        if fields[0] != b"IDPO" or fields[1] != 6:
            raise ValueError("not an MDL file")
        self.header = list(fields)
        self.scale = np.array(fields[2:5])
        self.origin = np.array(fields[5:8])
        num_skins, self.skinwidth, self.skinheight, num_verts, num_tris, num_frames = fields[12:18]
        pos = HEADER.size

        skin_size = self.skinwidth * self.skinheight
        skins_start = pos
        for _ in range(num_skins):
            kind = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            if kind == 0:
                pos += skin_size
            else:
                count = struct.unpack_from("<i", data, pos)[0]
                pos += 4 + 4 * count + count * skin_size
        self.skins = data[skins_start:pos]

        st = np.frombuffer(data, "<i4", num_verts * 3, pos).reshape(-1, 3)
        pos += num_verts * 12
        self.onseam = st[:, 0] != 0
        self.vert_st = st[:, 1:].astype(np.int64)
        tris = np.frombuffer(data, "<i4", num_tris * 4, pos).reshape(-1, 4)
        pos += num_tris * 16
        self.facesfront = tris[:, 0].astype(np.int64)
        self.tri_xyz = tris[:, 1:].astype(np.int64)

        # Frames are kept as a list of ("single", name+bbox bytes) or
        # ("group", group header bytes, [frame bytes...]) with vertex arrays.
        self.frames = []
        frame_size = 24 + num_verts * 4
        all_verts = []
        for _ in range(num_frames):
            kind = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            if kind == 0:
                head = data[pos:pos + 24]
                verts = np.frombuffer(data, np.uint8, num_verts * 4, pos + 24).reshape(-1, 4)
                pos += frame_size
                self.frames.append(("single", head, [verts]))
                all_verts.append(verts)
            else:
                count = struct.unpack_from("<i", data, pos)[0]
                head = data[pos:pos + 12 + 4 * count]
                pos += 12 + 4 * count
                subframes = []
                for _ in range(count):
                    subframes.append((data[pos:pos + 24],
                                      np.frombuffer(data, np.uint8, num_verts * 4, pos + 24).reshape(-1, 4)))
                    all_verts.append(subframes[-1][1])
                    pos += frame_size
                self.frames.append(("group", head, subframes))
        self.all_verts = np.stack(all_verts)

        # Corner skin coordinates for the simplifier: 2*v is the front one,
        # 2*v+1 the back one (s shifted by half the skin width on seams).
        back = self.vert_st + np.array([self.skinwidth // 2, 0])
        self.st = np.empty((2 * num_verts, 2), np.int64)
        self.st[0::2] = self.vert_st
        self.st[1::2] = np.where(self.onseam[:, None], back, self.vert_st)
        shifted = self.onseam[self.tri_xyz] & (self.facesfront[:, None] == 0)
        self.tri_st = 2 * self.tri_xyz + shifted

    def positions(self):
        return self.all_verts[:, :, :3].astype(np.float64) * self.scale + self.origin

    def corner_st(self, v, t):
        return 2 * v + int(self.onseam[v] and not self.facesfront[t])

    def serialize(self, tri_xyz, keep_tris):
        used = np.unique(tri_xyz[keep_tris])
        remap = np.full(len(self.vert_st), -1)
        remap[used] = np.arange(len(used))
        tris = remap[tri_xyz[keep_tris]]
        header = list(self.header)
        header[15] = len(used)
        header[16] = len(tris)
        out = bytearray(HEADER.pack(*header))
        out += self.skins
        st = np.concatenate([self.onseam[used, None].astype(np.int64) * 32, self.vert_st[used]], axis=1)
        out += st.astype("<i4").tobytes()
        tri_rows = np.concatenate([self.facesfront[keep_tris, None], tris], axis=1)
        out += tri_rows.astype("<i4").tobytes()
        for kind, head, payload in self.frames:
            if kind == "single":
                out += struct.pack("<i", 0) + head + payload[0][used].tobytes()
            else:
                out += struct.pack("<i", 1) + head
                for sub_head, verts in payload:
                    out += sub_head + verts[used].tobytes()
        return bytes(out)


class MdlSimplifier(Simplifier):
    def can_collapse(self, u, v):
        remap = super().can_collapse(u, v)
        if remap is None:
            return None
        for t, st in remap.items():
            if st != self.mesh.corner_st(v, t):
                return None
        return remap


def target_for(name, num_tris, args):
    for rule in args.rule:
        pattern, ratio = rule.split("=")
        if fnmatch.fnmatch(name, pattern):
            return max(args.min_tris, int(num_tris * float(ratio)))
    return max(args.min_tris, int(num_tris * args.ratio))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paks", nargs="+", help="source pak files, later ones override earlier ones")
    parser.add_argument("-o", "--output", required=True, help="output pak file")
    parser.add_argument("-r", "--ratio", type=float, default=0.5, help="fraction of triangles to keep")
    parser.add_argument("--rule", action="append", default=[],
                        help="per-model ratio, e.g. 'progs/v_*=0.75' (first match wins)")
    parser.add_argument("--min-tris", type=int, default=48, help="leave models this small alone")
    parser.add_argument("--min-normal-dot", type=float, default=0.2,
                        help="reject collapses that turn a triangle further than this in any frame")
    parser.add_argument("--seam-weight", type=float, default=2.0,
                        help="cost multiplier for collapsing along texture seams")
    parser.add_argument("--uv-tolerance", type=float, default=2.0,
                        help="max change in skin texels covered around a removed vertex")
    parser.add_argument("--suffix", default="", help="append this to the output model names")
    parser.add_argument("--only", default="*.mdl", help="glob of models to process")
    args = parser.parse_args()

    files = {}
    for pak in args.paks:
        files.update(read_pak(pak))

    out = {}
    total_before = total_after = 0
    for name in sorted(files):
        if not name.endswith(".mdl") or not fnmatch.fnmatch(name, args.only):
            continue
        mdl = Mdl(files[name])
        before = len(mdl.tri_xyz)
        target = target_for(name, before, args)
        if before <= args.min_tris or target >= before:
            continue
        simplifier = MdlSimplifier(mdl, args.min_normal_dot, args.seam_weight, args.uv_tolerance)
        simplifier.run(target)
        after = int(simplifier.alive.sum())
        total_before += before
        total_after += after
        out[name[:-4] + args.suffix + ".mdl"] = mdl.serialize(simplifier.tri_xyz, simplifier.alive)
        print(f"{name}: {before} -> {after} triangles", file=sys.stderr)

    write_pak(args.output, out)
    print(f"{len(out)} models, {total_before} -> {total_after} triangles", file=sys.stderr)


if __name__ == "__main__":
    main()
