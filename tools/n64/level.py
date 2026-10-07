"""Converting a structure BSP's render geometry for the N64 renderer.

The level is drawn as tiny3d vertex batches (port/n64/src/n64_render.c):
each batch is up to 70 vertices (the RSP's vertex cache) and the triangles
that use them, with one material. Batches are grouped by BSP cluster, so a
view can skip clusters outside its frustum.

There are no lightmaps (the first milestone leaves them out): each vertex
is lit by its material's own lighting summary from the lightmapper (ambient
colour and two distant lights), against the vertex normal.

The output file (big-endian):

    header         'H64L', version, counts and offsets (struct below)
    textures[]     name[24] (rom:/tex/<map>/<name>.sprite), width, height
    materials[]    texture index, flags
    clusters[]     bounds min[3], max[3] (floats, world units), first batch, batch count
    batches[]      material, vertex count, triangle count, vertex pair offset, index offset,
                   bounds min[3], max[3] (s16, N64 units)
    vertices       tiny3d T3DVertPacked, two vertices per 32 bytes
    indices        three u8 per triangle (vertex numbers within the batch)

Positions are stored in fixed point, world units * POSITION_SCALE.
"""

import math
import os
import struct
import subprocess
from pathlib import Path

from PIL import Image

from .bitmaps import bitmap_images, decode

POSITION_SCALE = 64.0
TEXTURE_FORMAT = os.environ.get("N64_TEXTURE_FORMAT", "CI4")
MAXIMUM_BATCH_VERTICES = 70
MAXIMUM_UV_EXTENT = 512.0  # texels; s10.5 holds +-1024, keep well inside it
VERSION = 2

MATERIAL_ALPHA_TEST = 1
MATERIAL_TWO_SIDED = 2
MATERIAL_TRANSPARENT = 4
MATERIAL_UNTEXTURED = 8

MKSPRITE = str(Path(os.environ.get("N64_INST", Path.home() / "n64/toolchain")) / "bin/mksprite")


def unpack_normal(value):
    """Halo's compressed normal: 11, 11 and 10 signed bits"""
    def signed(v, bits):
        return v - (1 << bits) if v & (1 << (bits - 1)) else v
    x = signed(value & 0x7FF, 11) / 1023.0
    y = signed((value >> 11) & 0x7FF, 11) / 1023.0
    z = signed((value >> 22) & 0x3FF, 10) / 511.0
    return x, y, z


def pack_normal(n):
    """tiny3d's 5.6.5 normal"""
    x, y, z = n
    length = math.sqrt(x * x + y * y + z * z) or 1.0
    x, y, z = x / length, y / length, z / length
    xi = max(-16, min(15, int(round(x * 15.5))))
    yi = max(-32, min(31, int(round(y * 31.5))))
    zi = max(-16, min(15, int(round(z * 15.5))))
    return ((xi & 31) << 11) | ((yi & 63) << 5) | (zi & 31)


class Texture:
    def __init__(self, name, width, height):
        self.name, self.width, self.height = name, width, height


class LevelBuilder:
    def __init__(self, xmap, defs, map_name, out_dir):
        self.map = xmap
        self.defs = defs
        self.map_name = map_name
        self.out_dir = Path(out_dir)
        self.textures = []
        self.texture_by_bitmap = {}
        self.materials = []
        self.material_by_shader = {}
        self.texture_prefix = ""
        # Halo's environment shader doubles texture x light; the N64 draws
        # the level in one RDP cycle, so the doubling is in the texture
        self.texture_brightness = 2.0

    def fields(self, struct_name):
        return {f.name: o for o, f in self.defs.fields(struct_name)}

    # ---------- textures

    def texture(self, bitmap_datum):
        if bitmap_datum in self.texture_by_bitmap:
            return self.texture_by_bitmap[bitmap_datum]
        tag = self.map.tag(bitmap_datum)
        index = -1
        if tag is not None:
            images = bitmap_images(self.map, tag, self.defs)
            if images:
                try:
                    img = decode(self.map, images[0], 64)
                except ValueError:
                    img = None
                if img is not None:
                    index = self.write_texture(img, tag.name)
        self.texture_by_bitmap[bitmap_datum] = index
        return index

    def write_texture(self, img, tag_name):
        # at most 64 texels a side and 4 KB of TMEM with mipmaps: CI4 64x64
        # is 2 KB, its mipmaps about 0.7 KB more
        w, h = img.size
        limit = 32.0 if TEXTURE_FORMAT == "RGBA16" else 64.0
        scale = min(1.0, limit / max(w, h))
        w2 = max(4, 1 << int(math.log2(max(1, int(w * scale)))))
        h2 = max(4, 1 << int(math.log2(max(1, int(h * scale)))))
        img = img.resize((w2, h2), Image.LANCZOS)
        if self.texture_brightness != 1.0:
            k = self.texture_brightness
            r, g, b, a = img.split()
            r, g, b = (ch.point(lambda v: min(255, int(v * k))) for ch in (r, g, b))
            img = Image.merge("RGBA", (r, g, b, a))
        index = len(self.textures)
        name = f"{self.texture_prefix}{index:02d}"
        tex_dir = self.out_dir / "tex" / self.map_name
        tex_dir.mkdir(parents=True, exist_ok=True)
        png = tex_dir / f"{name}.png"
        img.save(png)
        # a colour-indexed texture and its mipmaps must fit the lower 2 KB
        # of TMEM (its palette takes the upper half)
        bits = 16 if TEXTURE_FORMAT == "RGBA16" else 4
        # no mipmaps yet: the renderer's doubling combiner needs both RDP
        # cycles, and interpolated mipmapping would need one of them
        mipmaps = False
        subprocess.run([MKSPRITE, "--format", TEXTURE_FORMAT, "--mipmap", "BOX" if mipmaps else "NONE",
                        "--compress", "1", "-o", str(tex_dir), str(png)], check=True, capture_output=True)
        png.unlink()
        self.textures.append(Texture(name, w2, h2))
        return index

    # ---------- shaders

    def material(self, shader_datum):
        if shader_datum in self.material_by_shader:
            return self.material_by_shader[shader_datum]
        tag = self.map.tag(shader_datum)
        texture, flags = -1, 0
        if tag is not None:
            group = tag.group_name
            if group == "senv":
                f = self.fields("ShaderEnvironment")
                base = struct.unpack("<I", self.map.read(tag.address + f["base map"] + 12, 4))[0]
                env_flags = struct.unpack("<H", self.map.read(tag.address + f["shader environment flags"], 2))[0]
                if env_flags & 1:
                    flags |= MATERIAL_ALPHA_TEST
                texture = self.texture(base)
            else:
                # transparent shaders: their first map, blended
                flags |= MATERIAL_TRANSPARENT
                texture = self.first_map(tag)
        if texture < 0:
            flags |= MATERIAL_UNTEXTURED
        index = len(self.materials)
        self.materials.append((texture, flags))
        self.material_by_shader[shader_datum] = index
        return index

    def first_map(self, tag):
        """the first bitmap any dependency of the shader names"""
        found = []

        def on_dependency(t, address, f, path, group, datum):
            if group == 0x6269746D and datum != 0xFFFFFFFF and not found:  # 'bitm'
                found.append(datum)

        from .walk import Walker
        Walker(self.map, self.defs).walk(tag, on_dependency=on_dependency)
        return self.texture(found[0]) if found else -1

    # ---------- geometry

    def build(self, bsp_tag):
        x = self.map
        bo = self.fields("ScenarioStructureBSP")
        lo = self.fields("ScenarioStructureBSPLightmap")
        mo = self.fields("ScenarioStructureBSPMaterial")
        co = self.fields("ScenarioStructureBSPCluster")
        base = bsp_tag.address
        surfaces_count, surfaces_ptr = struct.unpack("<iI", x.read(base + bo["surfaces"], 8))
        surfaces = [struct.unpack("<hhh", x.read(surfaces_ptr + i * 6, 6)) for i in range(surfaces_count)]

        # every surface (triangle) with its world vertices, uvs, colours and material
        surface_info = [None] * surfaces_count
        lightmap_count, lightmap_ptr = struct.unpack("<iI", x.read(base + bo["lightmaps"], 8))
        lsize, msize = self.defs.size("ScenarioStructureBSPLightmap"), self.defs.size("ScenarioStructureBSPMaterial")
        for li in range(lightmap_count):
            la = lightmap_ptr + li * lsize
            mc, mp = struct.unpack("<iI", x.read(la + lo["materials"], 8))
            for mi in range(mc):
                ma = mp + mi * msize
                raw = x.read(ma, msize)
                shader_datum = struct.unpack_from("<I", raw, mo["shader"] + 12)[0]
                first, count = struct.unpack_from("<ii", raw, mo["surfaces"])
                rv_count = struct.unpack_from("<I", raw, mo["rendered vertices count"])[0]
                ambient = struct.unpack_from("<3f", raw, mo["ambient color"])
                light_count = struct.unpack_from("<h", raw, mo["distant light count"])[0]
                lights = [(struct.unpack_from("<3f", raw, mo["distant light 0 color"]),
                           struct.unpack_from("<3f", raw, mo["distant light 0 direction"])),
                          (struct.unpack_from("<3f", raw, mo["distant light 1 color"]),
                           struct.unpack_from("<3f", raw, mo["distant light 1 direction"]))][:max(0, light_count)]
                cv_size, _, _, cv_ptr, _ = struct.unpack_from("<iIIII", raw, mo["compressed vertices"])
                if count <= 0 or rv_count == 0 or not cv_ptr:
                    continue
                material = self.material(shader_datum)
                verts = []
                data = x.read(cv_ptr, rv_count * 32)
                for v in range(rv_count):
                    px, py, pz, normal, binormal, tangent, u, vv = struct.unpack_from("<3fIII2f", data, v * 32)
                    n = unpack_normal(normal)
                    color = list(ambient)
                    for (lc, ld) in lights:
                        d = -(n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2])
                        if d > 0:
                            color = [c + l * d for c, l in zip(color, lc)]
                    verts.append(((px, py, pz), n, (u, vv), tuple(min(1.0, max(0.0, c)) for c in color)))
                for s in range(first, first + count):
                    a, b, c = surfaces[s]
                    # Halo's front faces wind clockwise (Direct3D), tiny3d's
                    # counter-clockwise
                    surface_info[s] = (material, verts[a], verts[c], verts[b])

        # clusters: their surfaces (surface indices), batched per material
        cluster_count, cluster_ptr = struct.unpack("<iI", x.read(base + bo["clusters"], 8))
        csize = self.defs.size("ScenarioStructureBSPCluster")
        clusters = []
        used = set()
        for ci in range(cluster_count):
            ca = cluster_ptr + ci * csize
            ic, ip = struct.unpack("<iI", x.read(ca + co["surface indices"], 8))
            indices = list(struct.unpack(f"<{ic}i", x.read(ip, 4 * ic))) if ic > 0 else []
            clusters.append([i for i in indices if 0 <= i < surfaces_count and surface_info[i]])
            used.update(clusters[-1])
        # surfaces no cluster lists (rare): a last cluster of their own
        leftover = [i for i in range(surfaces_count) if surface_info[i] and i not in used]
        if leftover:
            clusters.append(leftover)
        return clusters, surface_info

    def batches(self, triangles):
        """splits (material, v0, v1, v2) triangles of one material into
        batches of at most 70 vertices with a bounded uv extent"""
        out = []
        current_vertices, current_map, current_tris = [], {}, []
        uv_min = [math.inf, math.inf]
        uv_max = [-math.inf, -math.inf]
        material = triangles[0][0] if triangles else 0
        texture, _ = self.materials[material]
        size = (self.textures[texture].width, self.textures[texture].height) if texture >= 0 else (32, 32)

        def flush():
            if current_tris:
                out.append((material, list(current_vertices), list(current_tris)))

        for tri in triangles:
            new = [v for v in tri[1:] if id(v) not in current_map]
            n_min = list(uv_min)
            n_max = list(uv_max)
            for v in tri[1:]:
                for k in range(2):
                    n_min[k] = min(n_min[k], v[2][k])
                    n_max[k] = max(n_max[k], v[2][k])
            extent = max((n_max[0] - n_min[0]) * size[0], (n_max[1] - n_min[1]) * size[1])
            if len(current_vertices) + len(new) > MAXIMUM_BATCH_VERTICES or extent > MAXIMUM_UV_EXTENT:
                flush()
                current_vertices, current_map, current_tris = [], {}, []
                n_min = [min(v[2][k] for v in tri[1:]) for k in range(2)]
                n_max = [max(v[2][k] for v in tri[1:]) for k in range(2)]
            uv_min, uv_max = n_min, n_max
            idx = []
            for v in tri[1:]:
                key = id(v)
                if key not in current_map:
                    current_map[key] = len(current_vertices)
                    current_vertices.append(v)
                idx.append(current_map[key])
            current_tris.append(tuple(idx))
        flush()
        return out

    def write(self, path, bsp_tag):
        clusters, surface_info = self.build(bsp_tag)
        out_batches = []
        out_clusters = []
        for indices in clusters:
            first = len(out_batches)
            by_material = {}
            for i in indices:
                info = surface_info[i]
                by_material.setdefault(info[0], []).append(info)
            lo = [math.inf] * 3
            hi = [-math.inf] * 3
            for material in sorted(by_material):
                for b in self.batches(by_material[material]):
                    out_batches.append(b)
                    for v in b[1]:
                        for k in range(3):
                            lo[k] = min(lo[k], v[0][k])
                            hi[k] = max(hi[k], v[0][k])
            if lo[0] == math.inf:
                lo = hi = [0.0, 0.0, 0.0]
            out_clusters.append((lo, hi, first, len(out_batches) - first))

        vertex_data = bytearray()
        index_data = bytearray()
        batch_records = []
        for material, verts, tris in out_batches:
            texture, _ = self.materials[material]
            tw, th = (self.textures[texture].width, self.textures[texture].height) if texture >= 0 else (32, 32)
            u0 = math.floor(min(v[2][0] for v in verts))
            v0 = math.floor(min(v[2][1] for v in verts))
            if len(verts) % 2:
                verts = verts + [verts[-1]]
            pair_offset = len(vertex_data) // 32
            for a, b in zip(verts[0::2], verts[1::2]):
                def pos(v):
                    return [max(-32768, min(32767, int(round(c * POSITION_SCALE)))) for c in v[0]]

                def st(v):
                    return [max(-32768, min(32767, int(round((v[2][0] - u0) * tw * 32)))),
                            max(-32768, min(32767, int(round((v[2][1] - v0) * th * 32))))]

                def rgba(v):
                    r, g, b_, = (int(round(c * 255)) for c in v[3])
                    return (r << 24) | (g << 16) | (b_ << 8) | 255
                vertex_data += struct.pack(">3hH3hHII2h2h", *pos(a), pack_normal(a[1]), *pos(b), pack_normal(b[1]),
                                           rgba(a), rgba(b), *st(a), *st(b))
            index_offset = len(index_data)
            for t in tris:
                index_data += bytes(t)
            lo = [max(-32768, min(32767, int(math.floor(min(v[0][k] for v in verts) * POSITION_SCALE)))) for k in range(3)]
            hi = [max(-32768, min(32767, int(math.ceil(max(v[0][k] for v in verts) * POSITION_SCALE)))) for k in range(3)]
            batch_records.append((material, len(verts), len(tris), pair_offset, index_offset, lo, hi))

        header_size = 64
        textures_size = len(self.textures) * 28
        materials_size = len(self.materials) * 4
        clusters_size = len(out_clusters) * 28
        batches_size = len(batch_records) * 24
        offset = header_size
        textures_offset = offset; offset += textures_size
        materials_offset = offset; offset += materials_size
        clusters_offset = offset; offset += clusters_size
        batches_offset = offset; offset += batches_size
        offset = (offset + 7) & ~7
        vertices_offset = offset; offset += len(vertex_data)
        indices_offset = offset; offset += len(index_data)

        out = bytearray()
        out += b"H64L" + struct.pack(">I f HHHH II II II II", VERSION, 1.0 / POSITION_SCALE,
                                     len(self.textures), len(self.materials), len(out_clusters), len(batch_records),
                                     textures_offset, materials_offset, clusters_offset, batches_offset,
                                     vertices_offset, len(vertex_data), indices_offset, len(index_data))
        out += b"\0" * (header_size - len(out))
        for t in self.textures:
            name = t.name.encode()
            out += name + b"\0" * (24 - len(name)) + struct.pack(">HH", t.width, t.height)
        for texture, flags in self.materials:
            out += struct.pack(">hH", texture, flags)
        for lo, hi, first, count in out_clusters:
            out += struct.pack(">3f3fHH", *lo, *hi, first, count)
        for material, nv, nt, pair_offset, index_offset, lo, hi in batch_records:
            out += struct.pack(">HBBII3h3h", material, nv, nt, pair_offset, index_offset, *lo, *hi)
        out += b"\0" * (vertices_offset - len(out))
        out += vertex_data
        out += index_data
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(out)
        return {
            "textures": len(self.textures), "materials": len(self.materials), "clusters": len(out_clusters),
            "batches": len(batch_records), "vertices": len(vertex_data) // 16,
            "triangles": len(index_data) // 3, "bytes": len(out),
        }
