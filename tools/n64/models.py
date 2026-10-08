"""Converting a map's object models (mode tags) for the N64 renderer.

Every model an object of the map uses is written at one level of detail
("low", or the nearest that exists) as tiny3d batches, like the level's
(level.py), but skinned: each vertex belongs to one node (the heavier of its
two), is stored in that node's space (the node's inverse default transform,
which the cache file keeps with each node, applied ahead of time) and is
loaded under that node's matrix. A batch is a list of segments -- (node,
vertices) -- loaded into the RSP's 70-vertex cache, then triangles that may
use vertices of several nodes, which stretches the skin between them.

There is no lighting on the RSP for models (the first milestone): each
vertex carries a sky light from above, baked against its default-pose
normal, which stays right while a model turns about its up axis.

The output file (big-endian):

    header        'H64O', version, counts, offsets
    textures[]    name[24] (rom:/tex/<map>/<name>.sprite), width, height
    models[]      tag index, node count, region count, first region, first permutation...
    regions[]     permutation count, first permutation
    permutations[] geometry (index into geometries, -1 for none)
    geometries[]  first batch, batch count
    batches[]     texture, flags, segment count, triangle count, first segment, index offset
    segments[]    node, cache slot, vertex count, vertex pair offset
    vertices      T3DVertPacked, positions in node space * MODEL_SCALE
    indices       three u8 per triangle (cache slots)
"""

import math
import struct
from pathlib import Path

from .decimate import decimate
from .level import LevelBuilder, MATERIAL_TRANSPARENT, MATERIAL_TWO_SIDED, pack_normal, unpack_normal

MODEL_SCALE = 1024.0
MAXIMUM_CACHE = 70
VERSION = 1
LOD_PREFERENCE = ("low", "super low", "medium", "high", "super high")
OBJECT_GROUPS = ("bipd", "vehi", "weap", "eqip", "garb", "proj", "scen", "mach", "ctrl", "lifi", "plac", "ssce")
SKY_LIGHT = (0.3, 0.2, 0.93)
# triangles a model may keep (tools/n64/decimate.py): the first-person
# models are drawn in every view
FIRST_PERSON_BUDGET = 500
MODEL_BUDGET = 800


def matrix4x3(values):
    """(scale, forward, left, up, position) from 13 floats"""
    return values[0], values[1:4], values[4:7], values[7:10], values[10:13]


def transform_point(m, p):
    scale, f, l, u, t = m
    x, y, z = (c * scale for c in p)
    return (u[0] * z + l[0] * y + f[0] * x + t[0],
            u[1] * z + l[1] * y + f[1] * x + t[1],
            u[2] * z + l[2] * y + f[2] * x + t[2])


def strip_triangles(indices):
    """a Direct3D triangle strip as triangles, degenerate ones dropped,
    each wound as the strip's first (clockwise front faces)"""
    out = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c or 0xFFFF in (a, b, c):
            continue
        out.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return out


class ModelConverter:
    def __init__(self, xmap, defs, map_name, out_dir):
        self.map = xmap
        self.defs = defs
        # textures go through the level builder's writer, named m00...
        self.textures = LevelBuilder(xmap, defs, map_name, out_dir)
        self.textures.texture_prefix = "m"
        # the shader's doubling in the texture too (one RDP cycle); a
        # player's tint is the only second cycle
        self.textures.texture_brightness = 2.0
        self.models = []

    def f(self, name):
        return {fl.name: o for o, fl in self.defs.fields(name)}

    def block(self, address):
        return struct.unpack("<iI", self.map.read(address, 8))

    def object_models(self):
        """the model tags the map's objects use"""
        of = self.f("Object")
        found = []
        for tag in self.map.tags:
            if tag.group_name not in OBJECT_GROUPS:
                continue
            datum = struct.unpack("<I", self.map.read(tag.address + of["model"] + 12, 4))[0]
            model = self.map.tag(datum)
            if model is not None and model.group_name == "mode" and model not in found:
                found.append(model)
        self.budgets = {m.index: MODEL_BUDGET for m in found}
        # the first-person views: each weapon's model, and the hands
        wf = self.f("Weapon")
        for tag in self.map.tags:
            if tag.group_name == "weap":
                datum = struct.unpack("<I", self.map.read(tag.address + wf["first person model"] + 12, 4))[0]
                model = self.map.tag(datum)
                if model is not None and model.group_name == "mode" and model not in found:
                    found.append(model)
                if model is not None:
                    self.budgets[model.index] = FIRST_PERSON_BUDGET
        gf = self.f("Globals")
        for tag in self.map.tags:
            if tag.group_name == "matg":
                count, pointer = self.block(tag.address + gf["first person interface"])
                if count > 0:
                    datum = struct.unpack("<I", self.map.read(pointer + 12, 4))[0]
                    model = self.map.tag(datum)
                    if model is not None and model.group_name == "mode" and model not in found:
                        found.append(model)
                    if model is not None:
                        self.budgets[model.index] = FIRST_PERSON_BUDGET
        return found

    def shader_texture(self, shader_datum):
        """(texture, material flags) of a model's shader (tools/n64/level.py's
        MATERIAL_* bits): two-sided shaders are drawn without culling"""
        tag = self.map.tag(shader_datum)
        if tag is None:
            return -1, 0
        if tag.group_name == "soso":
            so = self.f("ShaderModel")
            base = struct.unpack("<I", self.map.read(tag.address + so["base map"] + 12, 4))[0]
            model_flags = struct.unpack("<H", self.map.read(tag.address + so["shader model flags"], 2))[0]
            return self.textures.texture(base), MATERIAL_TWO_SIDED if model_flags & 2 else 0
        if tag.group_name == "senv":
            so = self.f("ShaderEnvironment")
            base = struct.unpack("<I", self.map.read(tag.address + so["base map"] + 12, 4))[0]
            return self.textures.texture(base), 0
        flags = MATERIAL_TRANSPARENT
        # the transparent shaders' own flags: "two sided" is bit 2 (bit 1
        # for meters)
        for struct_name, field, bit in (("ShaderTransparentChicago", "shader transparent chicago flags", 4),
                                        ("ShaderTransparentChicagoExtended", "shader transparent chicago extended flags", 4),
                                        ("ShaderTransparentGeneric", "shader transparent generic flags", 4),
                                        ("ShaderTransparentGlass", "shader transparent glass flags", 4),
                                        ("ShaderTransparentMeter", "shader transparent meter flags", 2)):
            if tag.group_name == {"ShaderTransparentChicago": "schi", "ShaderTransparentChicagoExtended": "scex",
                                  "ShaderTransparentGeneric": "sotr", "ShaderTransparentGlass": "sgla",
                                  "ShaderTransparentMeter": "smet"}[struct_name]:
                fields = self.f(struct_name)
                if field in fields and self.map.read(tag.address + fields[field], 1)[0] & bit:
                    flags |= MATERIAL_TWO_SIDED
        return self.textures.first_map(tag), flags

    def part_triangle_count(self, part_address):
        pf = self.f("ModelGeometryPart")
        raw = self.map.read(part_address, self.defs.size("ModelGeometryPart"))
        triangle_count = struct.unpack_from("<I", raw, pf["triangle count"])[0]
        triangle_pointer = struct.unpack_from("<I", raw, pf["triangle offset"])[0]
        if not triangle_pointer:
            return 0
        indices = struct.unpack(f"<{triangle_count + 2}H", self.map.read(triangle_pointer, 2 * (triangle_count + 2)))
        return len(strip_triangles(indices))

    def convert_part(self, part_address, nodes_inverse, u_scale, v_scale, shaders, batches, vertex_data, index_data,
                     keep=1.0):
        pf = self.f("ModelGeometryPart")
        raw = self.map.read(part_address, self.defs.size("ModelGeometryPart"))
        shader_index = struct.unpack_from("<h", raw, pf["shader index"])[0]
        triangle_count = struct.unpack_from("<I", raw, pf["triangle count"])[0]
        triangle_pointer = struct.unpack_from("<I", raw, pf["triangle offset"])[0]
        vertex_count = struct.unpack_from("<I", raw, pf["vertex count"])[0]
        vertex_entry = struct.unpack_from("<I", raw, pf["vertex offset"])[0]
        if not vertex_count or not triangle_pointer or not vertex_entry:
            return
        vertex_pointer = struct.unpack("<I", self.map.read(vertex_entry + 4, 4))[0]
        data = self.map.read(vertex_pointer, vertex_count * 32)
        indices = struct.unpack(f"<{triangle_count + 2}H", self.map.read(triangle_pointer, 2 * (triangle_count + 2)))
        texture, flags = shaders[shader_index] if 0 <= shader_index < len(shaders) else (-1, 8)

        vertices = []
        for v in range(vertex_count):
            px, py, pz, normal, binormal, tangent, tu, tv, n0, n1, w0 = struct.unpack_from("<3fIIIhhbbH", data, v * 32)
            node0 = n0 // 3 if n0 >= 0 else 0
            node1 = n1 // 3 if n1 >= 0 else node0
            weight = w0 / 65535.0
            node = node0 if weight >= 0.5 or n1 < 0 else node1
            if node >= len(nodes_inverse):
                node = 0
            local = transform_point(nodes_inverse[node], (px, py, pz))
            n = unpack_normal(normal)
            light = max(0.0, n[0] * SKY_LIGHT[0] + n[1] * SKY_LIGHT[1] + n[2] * SKY_LIGHT[2])
            shade = min(1.0, 0.5 + 0.5 * light)
            vertices.append((node, local, (tu / 32767.0 * u_scale, tv / 32767.0 * v_scale), shade, n))

        # tiny3d winds front faces counter-clockwise
        triangles = [(a, c, b) for a, b, c in strip_triangles(indices)
                     if a < vertex_count and b < vertex_count and c < vertex_count]
        if keep < 1.0:
            triangles = decimate(vertices, triangles, max(8, int(len(triangles) * keep)))
        # triangles of the same nodes together: fewer segments per batch
        triangles.sort(key=lambda t: tuple(sorted({vertices[i][0] for i in t})))

        tw, th = (self.textures.textures[texture].width, self.textures.textures[texture].height) if texture >= 0 else (32, 32)
        while triangles:
            # a batch: triangles while the per-node groups (padded to pairs)
            # fit the vertex cache
            groups = {}
            taken = []
            rest = []
            for tri in triangles:
                if rest:
                    rest.append(tri)
                    continue
                trial = {k: set(v) for k, v in groups.items()}
                for i in tri:
                    trial.setdefault(vertices[i][0], set()).add(i)
                slots = sum((len(s) + 1) // 2 * 2 for s in trial.values())
                if slots > MAXIMUM_CACHE and taken:
                    rest.append(tri)
                    continue
                groups = trial
                taken.append(tri)
            triangles = rest
            u0 = math.floor(min(vertices[i][2][0] for s in groups.values() for i in s))
            v0 = math.floor(min(vertices[i][2][1] for s in groups.values() for i in s))
            slot_of = {}
            segments = []
            slot = 0
            for node in sorted(groups):
                members = sorted(groups[node])
                if len(members) % 2:
                    members.append(members[-1])
                pair_offset = len(vertex_data) // 32
                for i, a in enumerate(members):
                    slot_of.setdefault(a, slot + i)
                for a, b in zip(members[0::2], members[1::2]):
                    vertex_data += self.pack_pair(vertices[a], vertices[b], u0, v0, tw, th)
                segments.append((node, slot, len(members), pair_offset))
                slot += len(members)
            index_offset = len(index_data)
            for tri in taken:
                index_data += bytes(slot_of[i] for i in tri)
            batches.append((texture, flags, segments, len(taken), index_offset))

    @staticmethod
    def pack_pair(a, b, u0, v0, tw, th):
        def pos(v):
            return [max(-32768, min(32767, int(round(c * MODEL_SCALE)))) for c in v[1]]

        def st(v):
            return [max(-32768, min(32767, int(round((v[2][0] - u0) * tw * 32)))),
                    max(-32768, min(32767, int(round((v[2][1] - v0) * th * 32))))]

        def rgba(v):
            c = int(round(v[3] * 255))
            return (c << 24) | (c << 16) | (c << 8) | 255
        return struct.pack(">3hH3hHII2h2h", *pos(a), pack_normal(a[4]), *pos(b), pack_normal(b[4]),
                           rgba(a), rgba(b), *st(a), *st(b))

    def write(self, path):
        mf, nf, rf, pf, gf = (self.f(s) for s in ("Model", "ModelNode", "ModelRegion", "ModelRegionPermutation", "ModelGeometry"))
        nsize, rsize, psize, gsize = (self.defs.size(s) for s in ("ModelNode", "ModelRegion", "ModelRegionPermutation", "ModelGeometry"))
        lod_fields = {"super low": "super low", "low": "low", "medium": "medium", "high": "high", "super high": "super high"}
        model_records, region_records, permutation_records, geometry_records = [], [], [], []
        batches, vertex_data, index_data = [], bytearray(), bytearray()
        for tag in self.object_models():
            a = tag.address
            u_scale, v_scale = struct.unpack("<2f", self.map.read(a + mf["base map u scale"], 8))
            u_scale = u_scale or 1.0
            v_scale = v_scale or 1.0
            nc, np_ = self.block(a + mf["nodes"])
            nodes_inverse = []
            for i in range(nc):
                raw = self.map.read(np_ + i * nsize, nsize)
                scale = struct.unpack_from("<f", raw, nf["scale"])[0]
                rot = struct.unpack_from("<9f", raw, nf["rotation"])
                trans = struct.unpack_from("<3f", raw, nf["translation"])
                nodes_inverse.append(matrix4x3((scale,) + rot + trans))
            sc, sp = self.block(a + mf["shaders"])
            shaders = []
            for i in range(sc):
                datum = struct.unpack("<I", self.map.read(sp + i * 32 + 12, 4))[0]
                shaders.append(self.shader_texture(datum))
            gc, gp = self.block(a + mf["geometries"])
            geometry_map = {}

            budget = self.budgets.get(tag.index, MODEL_BUDGET)

            def geometry(gi):
                if gi in geometry_map:
                    return geometry_map[gi]
                first = len(batches)
                pc, pp = self.block(gp + gi * gsize + gf["parts"])
                part_size = self.defs.size("ModelGeometryPart")
                total = sum(self.part_triangle_count(pp + p * part_size) for p in range(pc))
                keep = min(1.0, budget / total) if total else 1.0
                for p in range(pc):
                    self.convert_part(pp + p * part_size, nodes_inverse, u_scale, v_scale,
                                      shaders, batches, vertex_data, index_data, keep)
                geometry_map[gi] = len(geometry_records)
                geometry_records.append((first, len(batches) - first))
                return geometry_map[gi]

            rc, rp = self.block(a + mf["regions"])
            first_region = len(region_records)
            for r in range(rc):
                pc, pp = self.block(rp + r * rsize + rf["permutations"])
                first_permutation = len(permutation_records)
                for p in range(max(pc, 0)):
                    raw = self.map.read(pp + p * psize, psize)
                    choice = -1
                    for lod in LOD_PREFERENCE:
                        gi = struct.unpack_from("<h", raw, pf[lod_fields[lod]])[0]
                        if 0 <= gi < gc:
                            choice = geometry(gi)
                            break
                    permutation_records.append(choice)
                region_records.append((max(pc, 0), first_permutation))
            model_records.append((tag.index, nc, rc, first_region))

        textures = self.textures.textures
        segments = []
        batch_records = []
        for texture, flags, segs, tri_count, index_offset in batches:
            batch_records.append((texture, flags, len(segs), tri_count, len(segments), index_offset))
            segments.extend(segs)

        sizes = [len(textures) * 28, len(model_records) * 8, len(region_records) * 4, len(permutation_records) * 2,
                 len(geometry_records) * 4, len(batch_records) * 12, len(segments) * 8]
        offset = 64
        offsets = []
        for size in sizes:
            offsets.append(offset)
            offset = (offset + size + 3) & ~3
        offset = (offset + 7) & ~7
        vertices_offset = offset
        indices_offset = vertices_offset + len(vertex_data)

        out = bytearray()
        out += b"H64O" + struct.pack(">I HHHHHHH xx", VERSION, len(textures), len(model_records), len(region_records),
                                     len(permutation_records), len(geometry_records), len(batch_records), len(segments))
        out += struct.pack(">7I II", *offsets, vertices_offset, indices_offset)
        out += b"\0" * (64 - len(out))

        def pad_to(o):
            out.extend(b"\0" * (o - len(out)))

        pad_to(offsets[0])
        for t in textures:
            name = t.name.encode()
            out += name + b"\0" * (24 - len(name)) + struct.pack(">HH", t.width, t.height)
        pad_to(offsets[1])
        for tag_index, nc, rc, first_region in model_records:
            out += struct.pack(">HBBHH", tag_index, min(nc, 255), rc, first_region, 0)
        pad_to(offsets[2])
        for count, first in region_records:
            out += struct.pack(">HH", count, first)
        pad_to(offsets[3])
        for g in permutation_records:
            out += struct.pack(">h", g)
        pad_to(offsets[4])
        for first, count in geometry_records:
            out += struct.pack(">HH", first, count)
        pad_to(offsets[5])
        for texture, flags, seg_count, tri_count, first_segment, index_offset in batch_records:
            out += struct.pack(">hHBBHI", texture, flags, seg_count, tri_count, first_segment, index_offset)
        pad_to(offsets[6])
        for node, slot, count, pair_offset in segments:
            out += struct.pack(">BBBxI", node, slot, count, pair_offset)
        pad_to(vertices_offset)
        out += vertex_data
        out += index_data
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(out)
        return {"models": len(model_records), "geometries": len(geometry_records), "batches": len(batch_records),
                "segments": len(segments), "vertices": len(vertex_data) // 16, "triangles": len(index_data) // 3,
                "textures": len(textures), "bytes": len(out)}
