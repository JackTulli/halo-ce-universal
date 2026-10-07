#!/usr/bin/env python3
"""Converting an Xbox multiplayer map for the Nintendo 64 build.

    python3 -m tools.n64.convert build/xbox/disc/maps/chillout.map -o build/n64/maps/chillout.map

The output (N64 map) is one file the game copies from the cartridge into
RAM and relocates (port/n64/src/n64_cache.c):

    header (64 bytes, big-endian)
        'H64M', version, name[32], image size, relocation count,
        tag header offset, BSP header offset, reserved[2]
    image (tag data, big-endian; pointers are image offsets)
    relocations (image offsets of every non-null pointer, u32 each)

Tag data the N64 draws or plays differently (bitmaps, sounds, render
geometry) is left out here and converted separately.
"""

import argparse
import struct
import sys
from pathlib import Path

from .repack import NONE, Policy, Repacker, swap_array
from .xboxmap import XboxMap

MAGIC = b"H64M"
VERSION = 1

# tag groups the N64 game never loads: the Xbox menus and input devices
DROPPED_GROUPS = {b"DeLa", b"Soul", b"vcky", b"mply", b"devc", b"ngpr"}


def fourcc(text):
    return int.from_bytes(text, "big")


DROPPED = {fourcc(g) for g in DROPPED_GROUPS}


def swap_animation(raw, struct_raw, defs):
    """the frame data / default data of an uncompressed animation: per node,
    a rotation (4 shorts), a translation (3 floats) and a scale (float),
    each either per frame (the node's flag set) or in the default data
    (model_animations.c animation_get_node_orientations)"""
    return raw


class N64Policy(Policy):
    def __init__(self, xmap, defs):
        self.map = xmap
        self.defs = defs
        offs = {f.name: o for o, f in defs.fields("ModelAnimationsAnimation")}
        self.anim = offs

    def keep_tag(self, tag):
        return tag.group not in DROPPED

    def data(self, tag, path, parent, raw):
        address, struct_name, struct_raw = parent
        if path.endswith("animations[].frame data") or path.endswith("animations[].default data"):
            return self.animation_data(struct_raw, raw, path.endswith("default data"))
        if path.endswith("animations[].frame info"):
            return swap_array(raw, 4)
        if path.endswith("UnicodeStringList.strings[].string") or path.endswith("HUDMessageText.text data"):
            return swap_array(raw, 2)
        if path.endswith("StringList.strings[].string") or path.endswith("Scenario.script string data"):
            return raw
        if path.endswith("Meter.encoded stencil") or path.endswith("permutations[].mouth data"):
            return raw
        if path.endswith("ScenarioStructureBSP.cluster data"):
            return swap_array(raw, 4)
        if path.endswith("ScenarioStructureBSP.sound pas data"):
            return raw
        return None

    def animation_data(self, struct_raw, raw, default):
        o = self.anim
        frame_count, frame_size = struct.unpack_from("<hh", struct_raw, o["frame count"])
        node_count = struct.unpack_from("<h", struct_raw, o["node count"])[0]
        flags = struct.unpack_from("<H", struct_raw, o["flags"])[0]
        if flags & 1:
            raise ValueError("compressed animations are not converted yet")
        rot = struct.unpack_from("<2I", struct_raw, o["node rotation flag data"])
        trans = struct.unpack_from("<2I", struct_raw, o["node transform flag data"])
        scale = struct.unpack_from("<2I", struct_raw, o["node scale flag data"])

        def flag(bits, n):
            return (bits[n >> 5] >> (n & 31)) & 1

        layout = []  # element sizes of one frame (or of the default data)
        for n in range(node_count):
            want = 0 if default else 1
            if flag(rot, n) == want:
                layout += [2, 2, 2, 2]
            if flag(trans, n) == want:
                layout += [4, 4, 4]
            if flag(scale, n) == want:
                layout += [4]
        unit = sum(layout)
        out = bytearray(raw)
        if unit == 0:
            return bytes(out)
        frames = 1 if default else frame_count
        if unit * frames != len(raw):
            raise ValueError(f"animation data is {len(raw)} bytes, layout {unit}x{frames}")
        pos = 0
        for _ in range(frames):
            for size in layout:
                out[pos:pos + size] = raw[pos:pos + size][::-1]
                pos += size
        return bytes(out)


def convert(path, output):
    xmap = XboxMap(path)
    bsp_tag, bsp_base, bsp_size = xmap.load_bsp(0)
    repacker = Repacker(xmap, None)
    repacker.policy = N64Policy(xmap, repacker.defs)
    image = repacker.image
    image.alloc(0)

    tags = list(xmap.tags)
    roots = repacker.repack(tags)

    # the tag header and instance array (cache_files.c)
    instances = image.alloc(32 * len(tags))
    for t in tags:
        o = instances + 32 * t.index
        kept = t.index in roots
        group = t.group if kept else NONE
        image.put(o, struct.pack(">IIII", group, t.parents[0], t.parents[1], t.datum & 0xFFFFFFFF))
        image.put_pointer(o + 16, repacker.name_offset(t.name))
        # the BSP's instance has no address until the game loads the BSP
        # (cache_files.c scenario_structure_bsp_load asserts it)
        image.put_pointer(o + 20, None if t.index == bsp_tag.index else roots.get(t.index))
        image.put(o + 24, b"\0" * 8)
    header = image.alloc(36)
    image.put_pointer(header, instances)
    image.put(header + 4, struct.pack(">iIiIIII", xmap.scenario_datum, xmap.tags_checksum, len(tags), 0, 0, 0, 0))
    image.put(header + 32, b"tags")

    # the structure BSP's header (cache_file_structure_bsp_header) and the
    # scenario's reference to it
    bsp_header = image.alloc(24)
    image.put_pointer(bsp_header, roots.get(bsp_tag.index))
    image.put(bsp_header + 4, struct.pack(">IIII", 0, 0, 0, 0) + b"sbsp")
    scenario_root = roots[xmap.scenario.index]
    count, refs = struct.unpack_from(">iI", image.data, scenario_root + 0x5A4)
    if count > 0:
        image.put_pointer(refs + 8, bsp_header)

    relocs = sorted(set(image.relocs))
    out = bytearray()
    name = xmap.name.encode()[:31]
    out += MAGIC + struct.pack(">I", VERSION) + name + b"\0" * (32 - len(name))
    out += struct.pack(">IIII", len(image.data), len(relocs), header, bsp_header)
    out += b"\0" * (64 - len(out))
    out += image.data
    out += b"\0" * ((-len(out)) % 8)
    out += struct.pack(f">{len(relocs)}I", *relocs)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    Path(output).write_bytes(out)
    return xmap, repacker, len(out), relocs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--stats", action="store_true")
    args = parser.parse_args()
    xmap, repacker, size, relocs = convert(args.map, args.output)
    print(f"{xmap.name}: {size / 1e6:.2f} MB, {len(relocs)} relocations")
    if args.stats:
        for g, s in sorted(repacker.stats.items(), key=lambda x: -x[1])[:25]:
            print(f"  {g} {s / 1024:8.1f} KB")


if __name__ == "__main__":
    main()
