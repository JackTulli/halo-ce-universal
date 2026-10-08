"""Converting a map's object models (mode tags) for the N64 renderer.

Every model an object of the map uses is written at one level of detail
("low", or the nearest that exists) as tiny3d batches, like the level's
(level.py), but skinned: each vertex belongs to one node (the heavier of its
two), is stored in that node's space (the node's inverse default transform,
which the cache file keeps with each node, applied ahead of time) and is
loaded under that node's matrix. A batch is a list of segments -- (node,
vertices) -- loaded into the RSP's 70-vertex cache, then triangles that may
use vertices of several nodes, which stretches the skin between them.

A first-person model's texture is usually one atlas for the whole weapon
(512x512), far more than the 64x64 a texture can be in TMEM. Its triangles
are grouped into windows of the atlas (at DETAIL_ATLAS texels a side) that
fit 64x64, each window its own texture with its own palette; triangles too
big for any window keep the whole atlas at 64x64.

There is no lighting on the RSP for models (the first milestone): each
vertex carries a sky light from above, baked against its default-pose
normal, which stays right while a model turns about its up axis.

The output file (big-endian):

    header        'H64O', version, counts, offsets (the tables: everything up to
                  the first model's data, which the N64 keeps in memory)
    textures[]    name[24] (rom:/tex/<map>/<name>.sprite), width, height
    models[]      tag index, node count, region count, first region, first
                  batch, batch count, data offset, vertex bytes, index bytes
    regions[]     permutation count, first permutation
    permutations[] geometry (index into geometries, -1 for none)
    geometries[]  first batch, batch count
    batches[]     texture, flags, segment count, triangle count, first segment, index offset
    segments[]    node, cache slot, vertex count, vertex pair offset
    model data    per model: its vertices (T3DVertPacked, positions in node
                  space * MODEL_SCALE), then its indices (three u8 per
                  triangle: cache slots); a batch's segments and indices are
                  offsets into its model's data. The N64 reads a model's data
                  from the cartridge when it is first drawn.
"""

import math
import struct
from pathlib import Path

from PIL import Image

from .bitmaps import bitmap_images, decode
from .decimate import decimate
from .level import (LevelBuilder, MATERIAL_CHANGE_COLOR, MATERIAL_GLOW, MATERIAL_TRANSPARENT, MATERIAL_TWO_SIDED,
                    pack_normal, unpack_normal)

MODEL_SCALE = 1024.0
MAXIMUM_CACHE = 70
VERSION = 2
LOD_PREFERENCE = ("low", "super low", "medium", "high", "super high")
OBJECT_GROUPS = ("bipd", "vehi", "weap", "eqip", "garb", "proj", "scen", "mach", "ctrl", "lifi", "plac", "ssce")
SKY_LIGHT = (0.3, 0.2, 0.93)
# triangles a model may keep (tools/n64/decimate.py): the first-person
# models are drawn in every view
FIRST_PERSON_BUDGET = 500
MODEL_BUDGET = 800
# first-person textures: the atlas at this size, cut into windows of at most
# WINDOW texels a side (CI4, 2 KB of TMEM); a window keeps a texel of
# margin for the filtering
DETAIL_ATLAS = 256
WINDOW = 64


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
        # Halo doubles a model's colour, its lighting being kept at half;
        # the sky light baked here is whole, so the texture is not doubled
        self.textures.texture_brightness = 1.0
        # (base map, multipurpose map) -> texture with a split palette
        self.masked = {}
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
        """(texture, material flags, base map's bitmap or None) of a model's shader (tools/n64/level.py's
        MATERIAL_* bits): two-sided shaders are drawn without culling"""
        tag = self.map.tag(shader_datum)
        if tag is None:
            return -1, 0, None
        if tag.group_name == "soso":
            so = self.f("ShaderModel")
            base = struct.unpack("<I", self.map.read(tag.address + so["base map"] + 12, 4))[0]
            model_flags = struct.unpack("<H", self.map.read(tag.address + so["shader model flags"], 2))[0]
            flags = MATERIAL_TWO_SIDED if model_flags & 2 else 0
            # a player's colour where the multipurpose map's blue says (the
            # Xbox's channel order)
            change_source = struct.unpack("<H", self.map.read(tag.address + so["change color source"], 2))[0]
            multipurpose = struct.unpack("<I", self.map.read(tag.address + so["multipurpose map"] + 12, 4))[0]
            if change_source and self.map.tag(multipurpose) is not None:
                texture = self.masked_texture(base, multipurpose)
                if texture >= 0:
                    return texture, flags | MATERIAL_CHANGE_COLOR, (base, multipurpose)
            return self.textures.texture(base), flags, (base, None)
        if tag.group_name == "senv":
            so = self.f("ShaderEnvironment")
            base = struct.unpack("<I", self.map.read(tag.address + so["base map"] + 12, 4))[0]
            return self.textures.texture(base), 0, (base, None)
        # transparent shaders (glowing screens, sights, meters): meters and
        # numeric displays need Halo's own drawing and are left out (texture
        # None: the part is not converted);
        # the rest are cut-out glows of their first map
        if tag.group_name == "smet":
            return None, 0, None
        flags = MATERIAL_TRANSPARENT | MATERIAL_GLOW
        # the transparent shaders' own flags: "two sided" is bit 2, "numeric"
        # bit 7
        for struct_name, field, group in (("ShaderTransparentChicago", "shader transparent chicago flags", "schi"),
                                          ("ShaderTransparentChicagoExtended", "shader transparent chicago extended flags", "scex"),
                                          ("ShaderTransparentGeneric", "shader transparent generic flags", "sotr"),
                                          ("ShaderTransparentGlass", "shader transparent glass flags", "sgla")):
            if tag.group_name == group:
                fields = self.f(struct_name)
                value = self.map.read(tag.address + fields[field], 1)[0] if field in fields else 0
                if value & 4:
                    flags |= MATERIAL_TWO_SIDED
                if value & 0x80 and group != "sgla":
                    return None, 0, None
        return self.glow_texture(tag), flags, None

    def glow_texture(self, shader_tag):
        """a transparent shader's first map as a cut-out glow (RGBA16): its
        brighter texels, the rest transparent (the RDP's additive blending
        wraps around)"""
        found = []

        def on_dependency(t, address, f, path, group, datum):
            if group == 0x6269746D and datum != 0xFFFFFFFF and not found:  # 'bitm'
                found.append(datum)

        from .walk import Walker
        Walker(self.map, self.defs).walk(shader_tag, on_dependency=on_dependency)
        if not found:
            return -1
        key = ("glow", found[0])
        if key in self.masked:
            return self.masked[key]
        index = -1
        tag = self.map.tag(found[0])
        images = bitmap_images(self.map, tag, self.defs) if tag is not None else []
        if images:
            try:
                img = decode(self.map, images[0], 32)
            except ValueError:
                img = None
            if img is not None:
                r, g, b, a = img.convert("RGBA").split()
                brightness = Image.merge("RGB", (r, g, b)).convert("L").point(lambda v: 255 if v >= 48 else 0)
                index = self.textures.write_texture(Image.merge("RGBA", (r, g, b, brightness)), tag.name, fmt="RGBA16")
        self.masked[key] = index
        return index

    def change_mask(self, multipurpose, size):
        """the colour-change mask (the multipurpose map's blue) at size"""
        tag = self.map.tag(multipurpose)
        images = bitmap_images(self.map, tag, self.defs) if tag is not None else []
        if not images:
            return None
        try:
            img = decode(self.map, images[0], max(size))
        except ValueError:
            return None
        return img.getchannel("B").resize(size, Image.BILINEAR)

    def masked_texture(self, base, multipurpose):
        """the base map whole at 64x64, its palette split by the colour-change
        mask, or -1"""
        key = (base, multipurpose)
        if key in self.masked:
            return self.masked[key]
        index = -1
        tag = self.map.tag(base)
        images = bitmap_images(self.map, tag, self.defs) if tag is not None else []
        if images:
            try:
                img = decode(self.map, images[0], 64)
            except ValueError:
                img = None
            mask = self.change_mask(multipurpose, img.size) if img is not None else None
            if img is not None:
                index = self.textures.write_texture(img, tag.name, mask=mask)
        self.masked[key] = index
        return index

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
                     keep=1.0, detail=False):
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
        texture, flags, bitmap = shaders[shader_index] if 0 <= shader_index < len(shaders) else (-1, 8, None)
        if texture is None:
            return

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

        groups = self.texture_windows(bitmap, vertices, triangles) if detail and texture >= 0 else None
        if not groups:
            groups = [(texture, None, triangles)]
        for group_texture, st_of, group_triangles in groups:
            self.build_batches(vertices, group_triangles, group_texture, flags, st_of, batches, vertex_data, index_data)

    def texture_windows(self, bitmap, vertices, triangles):
        """a first-person part's triangles grouped by window of its atlas:
        [(texture, uv -> texels, triangles)], or None to keep the atlas
        whole"""
        if bitmap is None:
            return None
        bitmap, multipurpose = bitmap
        tag = self.map.tag(bitmap)
        if tag is None:
            return None
        images = bitmap_images(self.map, tag, self.defs)
        if not images or max(images[0].width, images[0].height) <= WINDOW:
            return None
        try:
            atlas = decode(self.map, images[0], DETAIL_ATLAS)
        except ValueError:
            return None
        scale = min(1.0, DETAIL_ATLAS / max(atlas.size))
        aw, ah = max(1, int(atlas.width * scale)), max(1, int(atlas.height * scale))
        atlas = atlas.resize((aw, ah), Image.LANCZOS)
        mask = self.change_mask(multipurpose, (aw, ah)) if multipurpose is not None else None
        whole = self.masked_texture(bitmap, multipurpose) if multipurpose is not None else self.textures.texture(bitmap)

        def extent(tri):
            us = [vertices[i][2][0] * aw for i in tri]
            vs = [vertices[i][2][1] * ah for i in tri]
            return min(us), min(vs), max(us), max(vs)

        # windows filled first-fit, the triangles taken in Z order of their
        # centres in the atlas
        def z_order(tri):
            x0, y0, x1, y1 = extent(tri)
            x, y = int(max(0, min(aw - 1, (x0 + x1) / 2))), int(max(0, min(ah - 1, (y0 + y1) / 2)))
            key = 0
            for bit in range(10):
                key |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
            return key

        windows = []  # [x0, y0, x1, y1, triangles]
        coarse = []
        span = WINDOW - 2
        for tri in sorted(triangles, key=z_order):
            x0, y0, x1, y1 = extent(tri)
            if x1 - x0 > span or y1 - y0 > span or x0 < 0 or y0 < 0 or x1 > aw or y1 > ah:
                coarse.append(tri)
                continue
            for w in windows:
                nx0, ny0, nx1, ny1 = min(w[0], x0), min(w[1], y0), max(w[2], x1), max(w[3], y1)
                if nx1 - nx0 <= span and ny1 - ny0 <= span:
                    w[0:4] = [nx0, ny0, nx1, ny1]
                    w[4].append(tri)
                    break
            else:
                windows.append([x0, y0, x1, y1, [tri]])

        groups = []
        for x0, y0, x1, y1, tris in windows:
            # a power-of-two window around the triangles, a texel of margin,
            # inside the atlas where it can be
            ww = 1 << max(2, math.ceil(math.log2(max(1.0, x1 - x0 + 2))))
            wh = 1 << max(2, math.ceil(math.log2(max(1.0, y1 - y0 + 2))))
            ox = int(max(0, min(math.floor(x0) - 1, aw - ww)))
            oy = int(max(0, min(math.floor(y0) - 1, ah - wh)))
            crop = atlas.crop((ox, oy, ox + ww, oy + wh))
            texture = self.textures.write_texture(crop, f"{tag.name} window",
                                                  mask=mask.crop((ox, oy, ox + ww, oy + wh)) if mask else None)
            groups.append((texture, lambda uv, ox=ox, oy=oy: (uv[0] * aw - ox, uv[1] * ah - oy), tris))
        if coarse:
            groups.append((whole, None, coarse))
        return groups

    def build_batches(self, vertices, triangles, texture, flags, st_of, batches, vertex_data, index_data):
        """batches of triangles while the per-node groups (padded to pairs)
        fit the vertex cache; st_of maps a texture coordinate to texels of
        the batch's texture (None: the texture's texels from the batch's
        lowest whole coordinate)"""
        # triangles of the same nodes together: fewer segments per batch
        triangles = sorted(triangles, key=lambda t: tuple(sorted({vertices[i][0] for i in t})))
        tw, th = (self.textures.textures[texture].width, self.textures.textures[texture].height) if texture >= 0 else (32, 32)
        while triangles:
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
            batch_st = st_of
            if batch_st is None:
                u0 = math.floor(min(vertices[i][2][0] for t in taken for i in t))
                v0 = math.floor(min(vertices[i][2][1] for t in taken for i in t))
                batch_st = lambda uv, u0=u0, v0=v0: ((uv[0] - u0) * tw, (uv[1] - v0) * th)
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
                    vertex_data += self.pack_pair(vertices[a], vertices[b], batch_st)
                segments.append((node, slot, len(members), pair_offset))
                slot += len(members)
            index_offset = len(index_data)
            for tri in taken:
                index_data += bytes(slot_of[i] for i in tri)
            batches.append((texture, flags, segments, len(taken), index_offset))

    @staticmethod
    def pack_pair(a, b, st_of):
        def pos(v):
            return [max(-32768, min(32767, int(round(c * MODEL_SCALE)))) for c in v[1]]

        def st(v):
            s_, t_ = st_of(v[2])
            return [max(-32768, min(32767, int(round(s_ * 32)))), max(-32768, min(32767, int(round(t_ * 32))))]

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
        batches = []
        model_data = []
        for tag in self.object_models():
            vertex_data, index_data = bytearray(), bytearray()
            first_batch = len(batches)
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
                                      shaders, batches, vertex_data, index_data, keep,
                                      detail=budget == FIRST_PERSON_BUDGET)
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
            model_records.append((tag.index, nc, rc, first_region, first_batch, len(batches) - first_batch))
            model_data.append((vertex_data, index_data))

        textures = self.textures.textures
        segments = []
        batch_records = []
        for texture, flags, segs, tri_count, index_offset in batches:
            batch_records.append((texture, flags, len(segs), tri_count, len(segments), index_offset))
            segments.extend(segs)

        sizes = [len(textures) * 28, len(model_records) * 24, len(region_records) * 4, len(permutation_records) * 2,
                 len(geometry_records) * 4, len(batch_records) * 12, len(segments) * 8]
        offset = 64
        offsets = []
        for size in sizes:
            offsets.append(offset)
            offset = (offset + size + 3) & ~3
        tables_size = (offset + 15) & ~15
        # each model's data, 16-byte aligned (the RSP loads its vertices by DMA)
        data_offsets = []
        offset = tables_size
        for vertex_data, index_data in model_data:
            data_offsets.append(offset)
            offset = (offset + len(vertex_data) + len(index_data) + 15) & ~15

        out = bytearray()
        out += b"H64O" + struct.pack(">I HHHHHHH xx", VERSION, len(textures), len(model_records), len(region_records),
                                     len(permutation_records), len(geometry_records), len(batch_records), len(segments))
        out += struct.pack(">7I II", *offsets, tables_size, 0)
        out += b"\0" * (64 - len(out))

        def pad_to(o):
            out.extend(b"\0" * (o - len(out)))

        pad_to(offsets[0])
        for t in textures:
            name = t.name.encode()
            out += name + b"\0" * (24 - len(name)) + struct.pack(">HH", t.width, t.height)
        pad_to(offsets[1])
        for (tag_index, nc, rc, first_region, first_batch, batch_count), (vertex_data, index_data), data_offset in \
                zip(model_records, model_data, data_offsets):
            out += struct.pack(">HBBHHHHIII", tag_index, min(nc, 255), rc, first_region, 0, first_batch, batch_count,
                               data_offset, len(vertex_data), len(index_data))
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
        for (vertex_data, index_data), data_offset in zip(model_data, data_offsets):
            pad_to(data_offset)
            out += vertex_data
            out += index_data
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(out)
        return {"models": len(model_records), "geometries": len(geometry_records), "batches": len(batch_records),
                "segments": len(segments), "vertices": sum(len(v) for v, i in model_data) // 16,
                "triangles": sum(len(i) for v, i in model_data) // 3, "textures": len(textures), "bytes": len(out),
                "tables": tables_size}
