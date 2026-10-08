"""The N64 menu's assets from the Xbox game's ui.map (port/n64/src/n64_menu.c).

The menu itself is the N64's own (split screen multiplayer only); from
Halo it takes:

- the scene behind Halo's main menu: the space sky (sky\\sky_ui) and the
  ring (scenery\\halo\\halo) as models (levels/ui.mdl, models.py), the ring's
  placement and the menu's camera points (ui/scene.bin);
- its fonts (fonts/<name>.font: an I8 atlas and the glyphs' places);
- pictures: the Halo logo, the multiplayer maps' and game types' pictures
  (ui/<name>.sprite).

    python3 -m tools.n64.ui <ui.map> --filesystem <file tree>
"""

import argparse
import math
import struct
import subprocess
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter

from .bitmaps import bitmap_images, decode
from .level import MKSPRITE, MATERIAL_TWO_SIDED
from .models import ModelConverter
from .repack import Repacker
from .tagdefs import TagDefinitions
from .xboxmap import XboxMap

FONTS = {"ui\\large_ui": "large", "ui\\small_ui": "small", "ui\\interstate": "interstate"}
SKY = "sky\\sky_ui\\sky_ui"
RING = "scenery\\halo\\halo"
# the sky is drawn as a sphere of this radius around the eye (it is
# ~27,000 units across; the menu's scene is ~40)
SKY_RADIUS = 30.0
# a model batch drawn first, without depth (models.py, n64_render.c)
MATERIAL_SKY = 64
# the sky's backdrop baked at this many texels a side (in 64-texel windows,
# models.py's atlas_windows), and about how many stars it keeps
SKY_ATLAS = 256
SKY_STARS = 1800
PICTURES = {
    "ui\\shell\\main_menu\\halo_logo": ("logo", 256, "RGBA16"),
    "ui\\shell\\bitmaps\\mp_map_grafix": ("map", 128, "RGBA16"),
    "ui\\shell\\bitmaps\\game_type_grafix": ("gametype", 128, "RGBA16"),
}


def rotation(yaw, pitch, roll):
    """Halo's matrix4x3_rotation_from_angles: forward, left, up"""
    cr, sr, cp, sp, cy, sy = math.cos(roll), math.sin(roll), math.cos(pitch), math.sin(pitch), math.cos(yaw), math.sin(yaw)
    forward = (cy * cp, sy * cr - cy * sp * sr, cy * sp * cr + sy * sr)
    left = (-(sy * cp), sy * sp * sr + cy * cr, cy * sr - sy * sp * cr)
    up = (-sp, -(cp * sr), cp * cr)
    return forward, left, up


def export_font(xmap, defs, tag, out):
    """the font's glyphs (Halo's own 8-bit coverage) in one I8 atlas, and
    their places: 'H64F', ascent, descent, glyph count, then per glyph its
    character, atlas x and y, width, height, origin x and y, advance"""
    f = {fl.name: o for o, fl in defs.fields("Font")}
    cf = {fl.name: o for o, fl in defs.fields("FontCharacter")}
    csize = defs.size("FontCharacter")
    a = tag.address
    ascent, descent = struct.unpack("<hh", xmap.read(a + f["ascending height"], 4))
    count, pointer = struct.unpack("<iI", xmap.read(a + f["characters"], 8))
    psize, _, _, ppointer, _ = struct.unpack("<iIIII", xmap.read(a + f["pixels"], 20))
    pixels = xmap.read(ppointer, psize)
    glyphs = []
    for i in range(count):
        raw = xmap.read(pointer + i * csize, csize)
        code, advance, width, height, ox, oy = struct.unpack_from("<hhhhhh", raw, 0)
        offset = struct.unpack_from("<I", raw, cf["pixels offset"])[0]
        if code < 32 or code > 126:
            continue
        width, height = max(width, 0), max(height, 0)
        data = pixels[offset:offset + width * height] if width and height else b""
        glyphs.append((code, advance, width, height, ox, oy, data))
    # shelves 128 texels wide
    atlas_width = 128
    x = y = shelf = 0
    places = []
    for code, advance, width, height, ox, oy, data in glyphs:
        if x + width + 1 > atlas_width:
            x = 0
            y += shelf + 1
            shelf = 0
        places.append((x, y))
        x += width + 1
        shelf = max(shelf, height)
    atlas_height = 1
    while atlas_height < y + shelf + 1:
        atlas_height *= 2
    atlas = Image.new("L", (atlas_width, atlas_height), 0)
    for (code, advance, width, height, ox, oy, data), (px, py) in zip(glyphs, places):
        if width and height and len(data) == width * height:
            atlas.paste(Image.frombytes("L", (width, height), data), (px, py))
    out.mkdir(parents=True, exist_ok=True)
    name = FONTS[tag.name]
    png = out / f"{name}.png"
    atlas.save(png)
    subprocess.run([MKSPRITE, "--format", "I8", "--compress", "1", "-o", str(out), str(png)], check=True,
                   capture_output=True)
    png.unlink()
    table = b"H64F" + struct.pack(">hhH", ascent, descent, len(glyphs))
    for (code, advance, width, height, ox, oy, data), (px, py) in zip(glyphs, places):
        table += struct.pack(">HHHBBbbB", code, px, py, width, height, ox, oy, max(0, min(255, advance)))
        table += b"\0"
    (out / f"{name}.glyphs").write_bytes(table)
    return len(glyphs)


def export_pictures(xmap, defs, out):
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for tag in xmap.tags:
        if tag.group_name != "bitm" or tag.name not in PICTURES:
            continue
        name, width, fmt = PICTURES[tag.name]
        for i, image in enumerate(bitmap_images(xmap, tag, defs)):
            img = decode(xmap, image, max(image.width, image.height))
            height = max(1, round(img.height * width / img.width))
            img = img.resize((width, height), Image.LANCZOS)
            png = out / (f"{name}.png" if name == "logo" else f"{name}{i}.png")
            img.save(png)
            subprocess.run([MKSPRITE, "--format", fmt, "--compress", "1", "-o", str(out), str(png)], check=True,
                           capture_output=True)
            png.unlink()
            written.append(png.stem)
    return written


def export_scene(xmap, defs, out):
    """ui/scene.bin: 'H64S', the sky's and the ring's model tag indices
    (u16 each), the ring's node matrix (scale, forward, left, up, position:
    Halo's real_matrix4x3), the camera points' count, then each one's
    name[32], position, forward, up and vertical field of view"""
    scenario = xmap.scenario
    f = {fl.name: o for o, fl in defs.fields("Scenario")}
    a = scenario.address
    sc, sp = struct.unpack("<iI", xmap.read(a + f["scenery"], 8))
    sf = {fl.name: o for o, fl in defs.fields("ScenarioScenery")}
    ring = (1.0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    if sc > 0:
        raw = xmap.read(sp, defs.size("ScenarioScenery"))
        position = struct.unpack_from("<3f", raw, sf["position"])
        yaw, pitch, roll = struct.unpack_from("<3f", raw, sf["rotation"])
        forward, left, up = rotation(yaw, pitch, roll)
        ring = (1.0,) + forward + left + up + position
    cc, cp = struct.unpack("<iI", xmap.read(a + f["cutscene camera points"], 8))
    cf = {fl.name: o for o, fl in defs.fields("ScenarioCutsceneCameraPoint")}
    csize = defs.size("ScenarioCutsceneCameraPoint")
    sky = [t.index for t in xmap.tags if t.group_name == "mode" and t.name == SKY]
    ring_model = [t.index for t in xmap.tags if t.group_name == "mode" and t.name == RING]
    out_data = b"H64S" + struct.pack(">HH", sky[0] if sky else 0xFFFF, ring_model[0] if ring_model else 0xFFFF)
    out_data += struct.pack(">13f", *ring) + struct.pack(">I", max(cc, 0))
    for i in range(max(cc, 0)):
        raw = xmap.read(cp + i * csize, csize)
        name = raw[cf["name"]:cf["name"] + 32].split(b"\0")[0]
        position = struct.unpack_from("<3f", raw, cf["position"])
        yaw, pitch, roll = struct.unpack_from("<3f", raw, cf["orientation"])
        fov = struct.unpack_from("<f", raw, cf["field of view"])[0]
        forward, left, up = rotation(yaw, pitch, roll)
        out_data += name + b"\0" * (32 - len(name)) + struct.pack(">10f", *position, *forward, *up, fov)
    out.mkdir(parents=True, exist_ok=True)
    (out / "scene.bin").write_bytes(out_data)
    return cc


def chicago_maps(xmap, defs, shader_tag):
    """a chicago shader's maps: (bitmap tag, colour function, u scale, v
    scale)"""
    f = {fl.name: o for o, fl in defs.fields("ShaderTransparentChicago")}
    mf = {fl.name: o for o, fl in defs.fields("ShaderTransparentChicagoMap")}
    size = defs.size("ShaderTransparentChicagoMap")
    count, pointer = struct.unpack("<iI", xmap.read(shader_tag.address + f["maps"], 8))
    maps = []
    for i in range(max(count, 0)):
        raw = xmap.read(pointer + i * size, size)
        datum = struct.unpack_from("<I", raw, mf["map"] + 12)[0]
        function = struct.unpack_from("<H", raw, mf["color function"])[0]
        u, v = struct.unpack_from("<2f", raw, mf["map u scale"])[0], struct.unpack_from("<f", raw, mf["map v scale"])[0]
        maps.append((xmap.tag(datum), function, u or 1.0, v or 1.0))
    return maps


def tiled(xmap, defs, tag, u_scale, v_scale, size, pool=False):
    """a map tiled u_scale by v_scale times over size x size texels: averaged,
    or with pool the brightest of the texels each one covers"""
    img = decode(xmap, bitmap_images(xmap, tag, defs)[0], 4096).convert("RGB")
    tiles_u, tiles_v = max(1, round(u_scale)), max(1, round(v_scale))
    sheet = Image.new("RGB", (img.width * tiles_u, img.height * tiles_v))
    for y in range(tiles_v):
        for x in range(tiles_u):
            sheet.paste(img, (x * img.width, y * img.height))
    if not pool:
        return sheet.resize((size, size), Image.BICUBIC)
    step_x, step_y = sheet.width / size, sheet.height / size
    result = Image.new("RGB", (size, size))
    for dy in range(max(1, int(step_y))):
        for dx in range(max(1, int(step_x))):
            sample = sheet.transform((size, size), Image.AFFINE, (step_x, 0, dx, 0, step_y, dy), Image.NEAREST)
            result = ImageChops.lighter(result, sample)
    return result


def space_backdrop(xmap, defs, shader_tag, size=SKY_ATLAS, stars=SKY_STARS):
    """the menu sky's space shader (a chicago shader: star mask times stars,
    plus space, added to a black framebuffer) baked into one image. The
    stars are tiled far finer than any N64 texture shows: the brightest in
    each place are kept, spaced out, about as many as the screen shows"""
    maps = chicago_maps(xmap, defs, shader_tag)
    # (the Xbox's order: current map, then the next by the colour function)
    layers = [tiled(xmap, defs, tag, u, v, size, pool=(max(u, v) * 64 > size)) for tag, _, u, v in maps]
    result = layers[0]
    for (tag, function, u, v), layer in zip(maps, layers[1:]):
        if function == 2:  # multiply
            result = ImageChops.multiply(result, layer)
        elif function == 4:  # add
            result = ImageChops.add(result, layer)
        else:
            result = layer
    # the stars alone (what the backdrop adds to the darkest map), thinned
    # to the local maxima above the level that keeps about 'stars' of them
    base = min(layers, key=lambda layer: sum(layer.convert("L").getextrema()))
    glow = ImageChops.subtract(result, base).convert("L")
    peaks = glow.filter(ImageFilter.MaxFilter(5))
    values = list(glow.getdata())
    is_peak = [g == p and g > 0 for g, p in zip(values, peaks.getdata())]
    ranked = sorted((g for g, peak in zip(values, is_peak) if peak), reverse=True)
    level = ranked[min(len(ranked) - 1, stars)] if ranked else 255
    kept = Image.new("L", glow.size)
    kept.putdata([g if peak and g > level else 0 for g, peak in zip(values, is_peak)])
    return ImageChops.add(base, Image.merge("RGB", (kept, kept, kept)))


class UIModels(ModelConverter):
    """the menu's scene: the sky, baked small, and the ring"""

    def shader_texture(self, shader_datum):
        # the sky's star field is its backdrop: drawn whole, not cut out, in
        # windows of a bigger image (texture_windows)
        tag = self.map.tag(shader_datum)
        if tag is not None and tag.name.endswith("\\space"):
            image = space_backdrop(self.map, self.defs, tag)
            whole = self.textures.write_texture(image, tag.name)
            return whole, 0, ("backdrop", image, whole, tag.name)
        return super().shader_texture(shader_datum)

    def texture_windows(self, bitmap, vertices, triangles):
        if bitmap is not None and bitmap[0] == "backdrop":
            _, image, whole, name = bitmap
            return self.atlas_windows(image, None, whole, name, vertices, triangles)
        return super().texture_windows(bitmap, vertices, triangles)

    def object_models(self):
        found = []
        self.budgets = {}
        self.first_person = set()
        self.hands = set()
        self.units = set()
        for tag in self.map.tags:
            if tag.group_name == "mode" and tag.name in (SKY, RING):
                found.append(tag)
                self.budgets[tag.index] = 100000 if tag.name == SKY else 1500
                if tag.name == SKY:
                    # (for texture_windows: the backdrop's windows)
                    self.first_person.add(tag.index)
        # the sky first: drawn first (n64_menu.c)
        found.sort(key=lambda t: t.name != SKY)
        return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", type=Path)
    parser.add_argument("--filesystem", type=Path, required=True)
    args = parser.parse_args()
    xmap = XboxMap(args.map)
    defs = TagDefinitions()
    fonts = [export_font(xmap, defs, t, args.filesystem / "fonts") for t in xmap.tags
             if t.group_name == "font" and t.name in FONTS]
    pictures = export_pictures(xmap, defs, args.filesystem / "ui")
    cameras = export_scene(xmap, defs, args.filesystem / "ui")
    models = UIModels(xmap, Repacker(xmap, None).defs, "ui", args.filesystem)
    sky = [t for t in xmap.tags if t.group_name == "mode" and t.name == SKY]
    if sky:
        # the sky's vertices at SKY_RADIUS, its batches drawn first and
        # without depth
        models.position_scales = {sky[0].index: SKY_RADIUS / 27000.0}
        models.extra_flags = {sky[0].index: MATERIAL_SKY | MATERIAL_TWO_SIDED}
        models.no_far = {t.index for t in xmap.tags if t.group_name == "mode"}
    stats = models.write(args.filesystem / "levels" / "ui.mdl")
    from .level import write_list
    write_list(args.filesystem / "tex" / "ui.list", sorted(models.textures.used_textures))
    print(f"ui: {len(fonts)} fonts, {len(pictures)} pictures, {cameras} camera points, models {stats}")


if __name__ == "__main__":
    main()
