"""Mesh simplification shared by the model tools: half-edge collapses ranked
by quadric error summed over all animation frames.

A mesh provides positions() -> (frames, verts, 3), tri_xyz and tri_st
(vertex and skin coordinate indices per triangle corner) and st (skin
coordinates). Collapses keep surviving vertices exactly as they are, so the
compressed per-frame vertex data can be copied over unchanged.
"""

import heapq
import struct

import numpy as np


def plane_quadrics(pos, tri):
    """Per-frame, per-vertex error quadrics from area-weighted triangle planes."""
    num_frames, num_verts = pos.shape[:2]
    p0, p1, p2 = pos[:, tri[:, 0]], pos[:, tri[:, 1]], pos[:, tri[:, 2]]
    n = np.cross(p1 - p0, p2 - p0)
    area = np.linalg.norm(n, axis=2, keepdims=True)
    n = n / np.maximum(area, 1e-12)
    plane = np.concatenate([n, -np.sum(n * p0, axis=2, keepdims=True)], axis=2)
    k = plane[:, :, :, None] * plane[:, :, None, :] * area[:, :, :, None]
    q = np.zeros((num_frames, num_verts, 4, 4))
    for corner in range(3):
        np.add.at(q, (slice(None), tri[:, corner]), k)
    return q


class Simplifier:
    def __init__(self, mesh, min_normal_dot, seam_weight, uv_tolerance):
        self.mesh = mesh
        self.seam_weight = seam_weight
        self.uv_tolerance = uv_tolerance
        self.pos = mesh.positions()
        self.pos_h = np.concatenate([self.pos, np.ones(self.pos.shape[:2] + (1,))], axis=2)
        self.quadric = plane_quadrics(self.pos, mesh.tri_xyz)
        self.min_normal_dot = min_normal_dot
        self.tri_xyz = mesh.tri_xyz.copy()
        self.tri_st = mesh.tri_st.copy()
        self.alive = np.ones(len(self.tri_xyz), bool)
        num_verts = self.pos.shape[1]
        self.vert_tris = [set() for _ in range(num_verts)]
        for t, (a, b, c) in enumerate(self.tri_xyz):
            for v in (a, b, c):
                self.vert_tris[v].add(t)
        self.locked = np.zeros(num_verts, bool)
        self.seam = np.array([len(self.wedges(v)) > 1 for v in range(num_verts)])
        self._lock_borders()

    def st_at(self, t, v):
        return int(self.tri_st[t][list(self.tri_xyz[t]).index(v)])

    def st_value(self, t, v):
        return tuple(self.mesh.st[self.st_at(t, v)])

    def wedges(self, v):
        groups = {}
        for t in self.vert_tris[v]:
            groups.setdefault(self.st_value(t, v), set()).add(t)
        return groups

    def _lock_borders(self):
        edge_count = {}
        for a, b, c in self.tri_xyz:
            for e in ((a, b), (b, c), (c, a)):
                key = (min(e), max(e))
                edge_count[key] = edge_count.get(key, 0) + 1
        for (a, b), n in edge_count.items():
            if n != 2:
                self.locked[a] = self.locked[b] = True

    def neighbours(self, v):
        return {int(w) for t in self.vert_tris[v] for w in self.tri_xyz[t] if w != v}

    def cost(self, u, v):
        p = self.pos_h[:, v]
        c = float(np.einsum("fi,fij,fj->", p, self.quadric[:, u], p))
        return c * self.seam_weight if self.seam[u] else c

    def can_collapse(self, u, v):
        if self.locked[u]:
            return None
        shared = self.vert_tris[u] & self.vert_tris[v]
        if not shared:
            return None
        if self.neighbours(u) & self.neighbours(v) != {
                int(w) for t in shared for w in self.tri_xyz[t] if w != u and w != v}:
            return None
        new_st = {}
        wedges = self.wedges(u)
        for wedge, tris in wedges.items():
            sides = tris & shared
            if not sides or len({self.st_value(t, v) for t in sides}) != 1:
                return None
            new_st[wedge] = self.st_at(next(iter(sides)), v)
        moved = [t for t in self.vert_tris[u] if t not in shared]
        if not moved:
            return None
        tri = self.tri_xyz[moved]
        new_tri = np.where(tri == u, v, tri)
        if not self._orientation_kept(tri, new_tri):
            return None
        remap = {t: new_st[self.st_value(t, u)] for t in moved}
        for wedge, tris in wedges.items():
            if not self._skin_area_kept(tris, remap, u):
                return None
        return remap

    def _uv_area(self, t, replace_u=None):
        st = [self.mesh.st[i].astype(np.float64) for i in self.tri_st[t]]
        if replace_u is not None:
            st[list(self.tri_xyz[t]).index(replace_u[0])] = self.mesh.st[replace_u[1]]
        (a, b, c) = st
        return 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]))

    def _skin_area_kept(self, tris, remap, u):
        before = after = 0.0
        for t in tris:
            area = self._uv_area(t)
            before += area
            if t in remap:
                new_area = self._uv_area(t, (u, remap[t]))
                if area * new_area < 0 and abs(area) > 0.5:
                    return False
                after += new_area
        return abs(after - before) <= self.uv_tolerance

    def _orientation_kept(self, before, after):
        def normals(tri):
            p0, p1, p2 = (self.pos[:, tri[:, i]] for i in range(3))
            return np.cross(p1 - p0, p2 - p0)
        a, b = normals(before), normals(after)
        la, lb = np.linalg.norm(a, axis=2), np.linalg.norm(b, axis=2)
        if np.any(lb < 1e-9):
            return False
        dot = np.sum(a * b, axis=2) / np.maximum(la * lb, 1e-12)
        return bool(np.all((la < 1e-9) | (dot >= self.min_normal_dot)))

    def collapse(self, u, v, moved):
        for t in self.vert_tris[u] & self.vert_tris[v]:
            self.alive[t] = False
            for w in self.tri_xyz[t]:
                if w != u:
                    self.vert_tris[w].discard(t)
        for t, st in moved.items():
            i = list(self.tri_xyz[t]).index(u)
            self.tri_xyz[t][i] = v
            self.tri_st[t][i] = st
            self.vert_tris[v].add(t)
        self.vert_tris[u] = set()
        self.quadric[:, v] += self.quadric[:, u]

    def run(self, target_tris):
        heap = []
        version = np.zeros(self.pos.shape[1], np.int64)

        def push(u):
            for v in self.neighbours(u):
                if not self.locked[u]:
                    heapq.heappush(heap, (self.cost(u, v), u, v, version[u], version[v]))

        for u in range(self.pos.shape[1]):
            push(u)
        while heap and self.alive.sum() > target_tris:
            c, u, v, vu, vv = heapq.heappop(heap)
            if vu != version[u] or vv != version[v] or not self.vert_tris[u]:
                continue
            ok = self.can_collapse(u, v)
            if ok is None:
                continue
            affected = self.neighbours(u) | {v}
            self.collapse(u, v, ok)
            for w in affected:
                version[w] += 1
            for w in affected:
                push(w)
                for x in self.neighbours(w):
                    if x in affected:
                        continue
                    heapq.heappush(heap, (self.cost(x, w), x, w, version[x], version[w]))


def read_pak(path):
    data = open(path, "rb").read()
    ident, ofs, size = struct.unpack_from("<4sii", data)
    if ident != b"PACK":
        raise ValueError(path + " is not a pak file")
    files = {}
    for i in range(size // 64):
        name, fofs, flen = struct.unpack_from("<56sii", data, ofs + i * 64)
        files[name.split(b"\0")[0].decode()] = data[fofs:fofs + flen]
    return files


def write_pak(path, files):
    body = bytearray()
    directory = bytearray()
    for name, data in files.items():
        directory += struct.pack("<56sii", name.encode(), 12 + len(body), len(data))
        body += data
    with open(path, "wb") as f:
        f.write(struct.pack("<4sii", b"PACK", 12 + len(body), len(directory)))
        f.write(body)
        f.write(directory)
