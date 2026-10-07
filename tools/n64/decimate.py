"""Simplifying a skinned mesh for the N64 (quadric error edge collapse).

Halo's first-person models are 1,000 to 2,300 triangles each, made for the
Xbox; the N64 draws them in every view. This collapses edges by the error
they add (Garland and Heckbert's quadrics), with constraints that keep the
result drawable as it was:

- vertices are welded by position within their node first, so a texture
  seam (two vertices, one position) moves as one and never opens a crack;
- an edge collapses only between welded vertices of the same node (the
  skinning stays as it was);
- a vertex moves onto its neighbour's position (half-edge collapse), and
  each corner of a remaining triangle takes, from the vertices at that
  position, the one whose texture coordinate is nearest its own;
- a collapse that would turn a triangle over is refused.

decimate(vertices, triangles, target) takes vertices as (node, position,
uv, ...) tuples and triangles as vertex index triples, and returns the
simplified triangles (still indices into the same vertices).
"""

import heapq
import math


def _plane_quadric(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length < 1e-12:
        return None
    area = length * 0.5
    nx, ny, nz = nx / length, ny / length, nz / length
    d = -(nx * a[0] + ny * a[1] + nz * a[2])
    # the symmetric 4x4 as its 10 distinct terms, weighted by area
    return [area * v for v in (nx * nx, nx * ny, nx * nz, nx * d, ny * ny, ny * nz, ny * d, nz * nz, nz * d, d * d)]


def _error(q, p):
    x, y, z = p
    return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x
            + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y
            + q[7] * z * z + 2 * q[8] * z + q[9])


def _normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    return (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)


def decimate(vertices, triangles, target):
    if len(triangles) <= target:
        return list(triangles)
    # weld by node and position
    group_of = []
    groups = {}
    members = []
    positions = []
    nodes = []
    for v in vertices:
        key = (v[0], round(v[1][0], 5), round(v[1][1], 5), round(v[1][2], 5))
        g = groups.get(key)
        if g is None:
            g = len(members)
            groups[key] = g
            members.append([])
            positions.append(v[1])
            nodes.append(v[0])
        group_of.append(g)
        members[g].append(len(group_of) - 1)

    tris = []  # [corner vertices, corner groups, alive]
    by_group = [set() for _ in members]
    quadric = [[0.0] * 10 for _ in members]
    for a, b, c in triangles:
        ga, gb, gc = group_of[a], group_of[b], group_of[c]
        if ga == gb or gb == gc or ga == gc:
            continue
        t = len(tris)
        tris.append([[a, b, c], [ga, gb, gc], True])
        q = _plane_quadric(positions[ga], positions[gb], positions[gc])
        for g in (ga, gb, gc):
            by_group[g].add(t)
            if q:
                quadric[g] = [x + y for x, y in zip(quadric[g], q)]
    alive_count = len(tris)
    alive_group = [True] * len(members)
    version = [0] * len(members)
    heap = []

    def push_edges(g):
        neighbours = set()
        for t in by_group[g]:
            for h in tris[t][1]:
                if h != g and nodes[h] == nodes[g]:
                    neighbours.add(h)
        for h in neighbours:
            q = [x + y for x, y in zip(quadric[g], quadric[h])]
            heapq.heappush(heap, (_error(q, positions[h]), g, h, version[g], version[h]))
            heapq.heappush(heap, (_error(q, positions[g]), h, g, version[h], version[g]))

    for g in range(len(members)):
        push_edges(g)

    def nearest_member(target_group, uv):
        best, best_d = None, None
        for v in members[target_group]:
            d = (vertices[v][2][0] - uv[0]) ** 2 + (vertices[v][2][1] - uv[1]) ** 2
            if best is None or d < best_d:
                best, best_d = v, d
        return best

    while alive_count > target and heap:
        cost, g, h, vg, vh = heapq.heappop(heap)
        if not alive_group[g] or not alive_group[h] or version[g] != vg or version[h] != vh:
            continue
        # refuse a collapse that turns a remaining triangle over
        flips = False
        for t in by_group[g]:
            corners = tris[t][1]
            if h in corners:
                continue
            before = _normal(*(positions[c] for c in corners))
            after = _normal(*(positions[h] if c == g else positions[c] for c in corners))
            if before[0] * after[0] + before[1] * after[1] + before[2] * after[2] <= 0:
                flips = True
                break
        if flips:
            continue
        for t in list(by_group[g]):
            tri = tris[t]
            if h in tri[1]:
                tri[2] = False
                alive_count -= 1
                for c in tri[1]:
                    by_group[c].discard(t)
                continue
            for i in range(3):
                if tri[1][i] == g:
                    tri[1][i] = h
                    tri[0][i] = nearest_member(h, vertices[tri[0][i]][2])
            by_group[h].add(t)
        by_group[g] = set()
        alive_group[g] = False
        quadric[h] = [x + y for x, y in zip(quadric[h], quadric[g])]
        version[h] += 1
        push_edges(h)

    return [tuple(t[0]) for t in tris if t[2]]
