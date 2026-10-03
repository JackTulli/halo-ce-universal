#!/usr/bin/env python3
"""Learn where the retail Halo maps keep their absolute addresses.

The retail maps (build 01.10.12.2276) are linked to the Xbox memory window:
their tag data holds absolute addresses in the tag cache (0x803A6000) and
each structure BSP absolute addresses where it loads. A build whose window
is elsewhere moves every one of those addresses after reading
(port/linux/src/xbox_map_rebase.c). It finds them by walking the data from
the tag header through the field layouts of each tag group, which this tool
learns from the maps and writes to port/linux/src/xbox_map_layouts.h.

The layouts are learned from a structural recognizer (the oracle): for each
map it classifies every word in the old window by its shape (a tag block is
{count, address, 0}, a tag reference {group, name, 0, index} whose name is
the named tag's, ...), drops shapes that lie inside raw data (tag data
contents, vertex data) and checks a set of invariants. The oracle looks at
values, so it is never used at run time; it tells this tool which fields
hold pointers. The tool then walks every map through the learned layouts
alone and checks that the walk finds exactly the oracle's pointers. Where
they disagree the data decides, and the report says so: a pointer shape in
no array, in a hole nothing points into, is left-over tool memory (an
orphan); a field that holds anything but pointers in other elements is no
pointer field (the oracle's shapes there are look-alikes). The layouts are
also compared with the game's own structures where the source states them
(clang's record layouts of the tag headers; needs NXDK_DIR).

The maps show a pointer field only where one of them holds a pointer. The
fields the game's structures declare and every retail map leaves empty come
from a reviewed table (DECLARED_FIELDS), so that a map of the same build
that fills one has that address moved too; --check fills them in copies of
the maps and proves the walker on those as well.

Usage:
  python3 tools/xbox_map_layouts.py [--maps DIR] [--out FILE] [map ...]
      learn, verify and write the header (from every map only)
  python3 tools/xbox_map_layouts.py --check [--walker FILE] [map ...]
      also build tools/xbox_map_rebase_test.c with the walker and prove it
      on every map against the oracle (MSYS2 clang)
"""

import argparse
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MAPS = os.path.join(ROOT, 'build', 'xbox', 'disc', 'maps')
DEFAULT_OUT = os.path.join(ROOT, 'port', 'linux', 'src', 'xbox_map_layouts.h')
DEFAULT_WALKER = os.path.join(ROOT, 'port', 'linux', 'src', 'xbox_map_rebase.c')
HARNESS = os.path.join(ROOT, 'tools', 'xbox_map_rebase_test.c')

RETAIL_BUILD = '01.10.12.2276'
TAG_CACHE_BASE = 0x803A6000          # physical_memory_map.c
TAG_CACHE_SIZE = 0x01600000
OLD_WINDOW = 0x80000000
WINDOW_SIZE = 0x08000000
NONE32 = 0xFFFFFFFF
TAG_HEADER_SIGNATURE = 0x74616773    # 'tags'
BSP_HEADER_SIGNATURE = 0x73627370    # 'sbsp'
SCENARIO_BSP_REFERENCES = 0x5A4      # offsetof(struct scenario, structure_bsp_references)
D3D_HEADER_SIZE = 12                 # {Common, Data, Lock}
TAG_INSTANCE_SIZE = 0x20
VERTEX_TYPE_SIZES = (56, 32, 20, 8, 68, 32, 24, 36, 20, 16, 16, 8)   # rasterizer_geometry.c

# the oracle's categories of pointer words
FIXED_CATEGORIES = {'tag_header', 'bsp_header', 'instance_name', 'instance_base',
                    'd3d_vb_data', 'd3d_ib_data', 'd3d_bsp_vb_data', 'd3d_bsp_lm_vb_data'}
FIELD_CATEGORIES = {'tag_block', 'tag_data', 'tag_reference', 'tag_reference_stale_index',
                    'd3d_header_pointer', 'triangle_buffer_base_address', 'bsp_reference_base_address'}
POINTER_CATEGORIES = FIXED_CATEGORIES | FIELD_CATEGORIES
STRONG_CATEGORIES = ('tag_reference', 'tag_reference_stale_index', 'd3d_header_pointer', 'instance_name')

# where the pointer is in each kind of field: a tag_block, tag_reference or
# tag_data field starts before its pointer, the rest are the pointer
POINTER_WORD = {'block': 4, 'reference': 4, 'data': 12,
                'vertex_header': 0, 'index_header': 0, 'index_data': 0, 'structure_bsp': 0}


class MapError(Exception):
    pass


def fourcc(value):
    b = value.to_bytes(4, 'big')
    return b.decode('ascii') if all(32 <= c < 127 for c in b) else '%08x' % value


def fourcc_value(text):
    return int.from_bytes(text.encode('ascii'), 'big')


# ---------- reading a map

def read_map(path):
    """the header and the decompressed contents of a cache file, as the game
    caches it (cache_files_decompress_windows.c: the 0x800-byte header stays
    raw, everything after it is one zlib stream whose output continues at
    offset 0x800)"""
    with open(path, 'rb') as f:
        raw = f.read()
    header = raw[:0x800]
    signature, version, file_length, _, tag_data_offset, tag_data_size = struct.unpack_from('<6I', header)
    if signature != 0x68656164 or struct.unpack_from('<I', header, 0x7FC)[0] != 0x666F6F74:
        raise MapError('%s: not a cache file' % path)
    full = header + zlib.decompress(raw[0x800:])
    del raw
    info = {
        'file': os.path.basename(path),
        'name': header[0x20:0x40].split(b'\0')[0].decode('ascii', 'replace'),
        'build': header[0x40:0x60].split(b'\0')[0].decode('ascii', 'replace'),
        'version': version,
        'file_length': file_length,
        'tag_data_offset': tag_data_offset,
        'tag_data_size': tag_data_size,
    }
    if len(full) != file_length:
        raise MapError('%s: decompressed to %#x bytes, the header says %#x' % (path, len(full), file_length))
    if info['build'] != RETAIL_BUILD:
        raise MapError('%s: build %s, not the retail %s' % (path, info['build'], RETAIL_BUILD))
    return info, full


class Region:
    """the tag data or one structure BSP, at the address it was linked to"""

    def __init__(self, name, base, data, kind):
        self.name, self.base, self.kind = name, base, kind
        self.data = bytes(data)
        self.size = len(data)
        self.end = base + self.size
        self.words = array('I', self.data + b'\0' * ((-len(data)) % 4))
        if sys.byteorder != 'little':
            self.words.byteswap()
        self.known = {}          # address -> category of the fixed structures
        self.d3d = {}            # D3D header address -> (kind, Data)
        self.alloc = {}          # start -> (kind, count or size, referenced from)
        self.records = []        # [address, value, category, parameter]
        self.pointers = {}       # address -> (category, value, ambiguous), after analysis

    def contains(self, address, size=1):
        return self.base <= address and address + size <= self.end

    def word(self, address):
        return self.words[(address - self.base) >> 2]

    def cstring(self, address, limit=256):
        o = address - self.base
        e = self.data.find(b'\0', o, o + limit)
        return self.data[o:e].decode('latin-1') if e >= 0 else None


# ---------- the oracle

def oracle(full, info):
    """classify the old-window words of the tag data and of every BSP by
    their shape (the study scanner of the 9x port, M1); returns the regions,
    the tag instances and the BSPs, each region's pointers in .pointers"""
    tdo, tds = info['tag_data_offset'], info['tag_data_size']
    window_lo, window_hi = OLD_WINDOW, OLD_WINDOW + WINDOW_SIZE
    tags = Region('tags', TAG_CACHE_BASE, full[tdo:tdo + tds], 'tags')
    th = struct.unpack_from('<9I', tags.data, 0)
    tag_header = dict(zip(('tag_instances', 'scenario_tag_index', 'checksum', 'tag_count',
                           'vertex_buffer_count', 'vertex_buffers', 'index_buffer_count',
                           'index_buffers', 'signature'), th))
    if tag_header['signature'] != TAG_HEADER_SIGNATURE:
        raise MapError('the tag header has no signature')
    n = tag_header['tag_count']
    ti = tag_header['tag_instances']
    instances = []
    for i in range(n):
        a = ti + i * TAG_INSTANCE_SIZE
        g, p0, p1, idx, name, base, _, _ = struct.unpack_from('<8I', tags.data, a - tags.base)
        instances.append({'group': g, 'parents': (p0, p1), 'index': idx, 'name_ptr': name, 'base': base,
                          'name': tags.cstring(name) if tags.contains(name) else None, 'address': a})
    group_tags = set()
    for t in instances:
        group_tags.add(t['group'])
        group_tags.update(p for p in t['parents'] if p != NONE32)
    name_ptrs = set(t['name_ptr'] for t in instances)

    def tag_by_index(idx):
        k = idx & 0xFFFF
        if idx == NONE32 or k >= n:
            return None
        t = instances[k]
        return t if t['index'] == idx else None

    scenario = tag_by_index(tag_header['scenario_tag_index'])
    bsp_count, bsp_refs, _ = struct.unpack_from('<3I', tags.data, scenario['base'] + SCENARIO_BSP_REFERENCES - tags.base)
    bsps, regions = [], [tags]
    for b in range(bsp_count):
        ra = bsp_refs + b * 0x20
        fo, fs, ba, _, _, _, _, idx = struct.unpack_from('<8I', tags.data, ra - tags.base)
        t = tag_by_index(idx)
        reg = Region('bsp%d' % b, ba, full[fo:fo + fs], 'bsp')
        hb = struct.unpack_from('<6I', reg.data, 0)
        if hb[5] != BSP_HEADER_SIGNATURE:
            raise MapError('BSP %d has no signature' % b)
        bsps.append({'index': b, 'name': t['name'] if t else None, 'file_offset': fo, 'file_size': fs,
                     'base_address': ba, 'reference_address': ra, 'region': reg,
                     'header': dict(zip(('base_address', 'vertex_buffer_count', 'vertex_buffers',
                                         'lightmap_vertex_buffer_count', 'lightmap_vertex_buffers',
                                         'signature'), hb))})
        regions.append(reg)
        tags.known[ra + 8] = 'bsp_reference_base_address'

    # fixed structures
    for off in (0, 0x14, 0x1C):
        tags.known[tags.base + off] = 'tag_header'
    tags.alloc[tags.base] = ('tag_header', 1, None)
    tags.alloc[ti] = ('tag_instances', n, tags.base)
    tags.alloc[min(t['name_ptr'] for t in instances)] = ('tag_names', 1, None)
    for t in instances:
        tags.known[t['address'] + 0x10] = 'instance_name'
        if t['base']:
            tags.known[t['address'] + 0x14] = 'instance_base'
            tags.alloc[t['base']] = ('tag', 1, t['address'] + 0x14)

    def d3d_array(reg, address, count, kind):
        if not count:
            return
        reg.alloc.setdefault(address, (kind + '_headers', count, None))
        for k in range(count):
            h = address + D3D_HEADER_SIZE * k
            data = reg.word(h + 4)
            reg.d3d[h] = (kind, data)
            reg.known[h] = 'd3d_header_junk'        # Common: set by the register code
            reg.known[h + 4] = 'd3d_%s_data' % kind
            reg.known[h + 8] = 'd3d_header_junk'    # Lock: junk in the file
            if data:
                reg.alloc.setdefault(data, (kind + '_data', 1, h + 4))

    d3d_array(tags, tag_header['vertex_buffers'], tag_header['vertex_buffer_count'], 'vb')
    d3d_array(tags, tag_header['index_buffers'], tag_header['index_buffer_count'], 'ib')
    for b in bsps:
        reg, h = b['region'], b['header']
        for off in (0, 8, 0x10):
            reg.known[reg.base + off] = 'bsp_header'
        reg.alloc[reg.base] = ('bsp_header', 1, b['reference_address'] + 8)
        reg.alloc[h['base_address']] = ('tag', 1, reg.base)
        d3d_array(reg, h['vertex_buffers'], h['vertex_buffer_count'], 'bsp_vb')
        d3d_array(reg, h['lightmap_vertex_buffers'], h['lightmap_vertex_buffer_count'], 'bsp_lm_vb')

    def resolve(src, v, size=1):
        """the region a pointer of src may point into"""
        if src.contains(v, size):
            return src
        if tags.contains(v, size):
            return tags
        if src is tags:
            for r in regions[1:]:
                if r.contains(v, size):
                    return r
        return None

    # phase A: one pass over every window word
    for reg in regions:
        rw, nw = reg.words, len(reg.words)
        d3d_any = (reg.d3d, tags.d3d) if reg is not tags else (tags.d3d,)
        for i in range(nw):
            v = rw[i]
            if v < window_lo or v >= window_hi:
                continue
            a = reg.base + 4 * i
            cat, param = reg.known.get(a), None
            if cat is None:
                prev = rw[i - 1] if i >= 1 else None
                nxt = rw[i + 1] if i + 1 < nw else None
                # tag_reference {group_tag, name, name_length, index} (tag_groups.h)
                if prev in group_tags and i + 2 < nw:
                    t = tag_by_index(rw[i + 2])
                    if t is not None and t['name_ptr'] == v and (t['group'] == prev or prev in t['parents']):
                        cat = 'tag_reference'
                    elif v in name_ptrs:
                        cat = 'tag_reference_stale_index'
                # vertex_buffer {short type < 12, pad, count, offset, base_address, hardware_format},
                # triangle_buffer {type, count, base_address = the header's Data, hardware_format}
                # (rasterizer_geometry.h)
                if cat is None and i >= 4:
                    for d in d3d_any:
                        if v in d:
                            if d[v][0] == 'ib':
                                ok = prev == d[v][1]
                            else:
                                ok = rw[i - 4] < 12 and rw[i - 3] < 0x100000
                                param = (rw[i - 4], rw[i - 3], d is reg.d3d)
                            if ok:
                                cat = 'd3d_header_pointer'
                            else:
                                param = None
                            break
                if cat is None and nxt is not None:
                    for d in d3d_any:
                        if nxt in d and d[nxt][1] == v:
                            cat = 'triangle_buffer_base_address'
                            break
                # tag_data {size, flags, file_offset, address, definition} (tag_groups.h)
                if cat is None and i >= 3 and nxt == 0 and 0 < rw[i - 3] < TAG_CACHE_SIZE:
                    if resolve(reg, v, rw[i - 3]) is not None:
                        if 0 < prev < 0x10000:
                            cat, param = 'tag_data_or_block', (rw[i - 3], prev)
                        else:
                            cat, param = 'tag_data', rw[i - 3]
                # tag_block {count, address, definition} (tag_groups.h)
                if cat is None and nxt == 0 and prev is not None and 0 < prev < 0x10000:
                    if resolve(reg, v) is not None:
                        cat, param = 'tag_block', prev
                # 64K elements or more (BSP collision and render data): checked in phase B
                if cat is None and nxt == 0 and prev is not None and 0x10000 <= prev < 0x1000000:
                    if resolve(reg, v, prev) is not None:
                        cat, param = 'large_block_candidate', prev
                if cat is None:
                    cat = 'other'
            if cat not in ('other', 'd3d_header_junk'):
                reg.records.append([a, v, cat, param])
        reg.alloc0 = dict(reg.alloc)

    # phase B: allocations and raw-data extents, then drop the shapes that lie
    # inside raw data (tag data contents, vertex data), until nothing changes
    candidates = ('tag_data_or_block', 'large_block_candidate', 'tag_block_large', 'large_count_shape_rejected')
    for _ in range(16):
        for reg in regions:
            reg.alloc = dict(reg.alloc0)
            reg.data_ext = {}    # tag data contents, also where the start is another allocation's
        for reg in regions:
            strong = sorted(rec[0] for rec in reg.records if rec[2] in STRONG_CATEGORIES)
            for rec in reg.records:
                a, v, cat, param = rec
                if cat == 'tag_block':
                    resolve(reg, v).alloc.setdefault(v, ('block', param, a))
                elif cat == 'tag_data':
                    r = resolve(reg, v)
                    r.alloc.setdefault(v, ('data', param, a))
                    r.data_ext[v] = max(r.data_ext.get(v, 0), param)
                elif cat == 'tag_data_or_block':
                    # {size, flags, small file offset, address, 0} reads as {.., count, address, 0}
                    # too: a "tag_data" whose extent holds a tag reference or D3D header pointer is a block
                    size, cnt = param
                    k = bisect.bisect_left(strong, v)
                    r = resolve(reg, v)
                    if r is reg and k < len(strong) and strong[k] < v + size:
                        r.alloc.setdefault(v, ('block', cnt, a))
                    else:
                        r.alloc.setdefault(v, ('data', size, a))
                        r.data_ext[v] = max(r.data_ext.get(v, 0), size)
        for reg in regions:
            st = sorted(reg.alloc)
            for rec in reg.records:
                if rec[2] in ('large_block_candidate', 'tag_block_large', 'large_count_shape_rejected'):
                    a, v, cat, count = rec
                    r = resolve(reg, v)
                    rst = st if r is reg else sorted(r.alloc)
                    k = bisect.bisect_right(rst, v)
                    nxt_start = rst[k] if k < len(rst) else r.end
                    ok = v + count <= nxt_start or v in r.alloc0
                    rec[2] = 'tag_block_large' if ok else 'large_count_shape_rejected'
                    if ok:
                        r.alloc.setdefault(v, ('block', count, a))
        # vertex data extents: count * size of the type, cut at the next allocation
        for reg in regions:
            reg.vext = {}
        for reg in regions:
            for a, v, cat, param in reg.records:
                if cat != 'd3d_header_pointer' or param is None:
                    continue
                typ, count, own = param
                owner = reg if own else tags
                kind, data = owner.d3d[v]
                if typ < len(VERTEX_TYPE_SIZES) and data:
                    owner.vext[data] = max(owner.vext.get(data, 0), count * VERTEX_TYPE_SIZES[typ])
        for reg in regions:
            st = sorted(set(reg.alloc) | set(reg.vext))
            for data, size in list(reg.vext.items()):
                k = bisect.bisect_right(st, data)
                nxt_start = st[k] if k < len(st) else reg.end
                reg.vext[data] = min(size, nxt_start - data)
                kind, _, ref = reg.alloc.get(data, ('vb_data', 0, None))
                reg.alloc[data] = (kind, reg.vext[data], ref)
        changed = 0
        for reg in regions:
            iv = sorted([(s, s + c) for s, c in reg.data_ext.items()] +
                        [(s, s + c) for s, c in reg.vext.items()])
            lo = [x[0] for x in iv]
            hi_max, m = [], 0
            for x in iv:
                m = max(m, x[1])
                hi_max.append(m)
            for rec in reg.records:
                a, v, cat, param = rec
                if (cat in POINTER_CATEGORIES or cat in candidates) and reg.known.get(a) != cat:
                    k = bisect.bisect_right(lo, a) - 1
                    if k >= 0 and hi_max[k] > a:
                        rec[2] = 'pattern_in_raw_data'
                        changed += 1
        if not changed:
            break
    else:
        raise MapError('the raw-data filter did not settle')
    # a {size, pad, file_offset, address, 0} with a small file offset reads
    # as {count, address, 0} too: the oracle's guess stays in the allocation
    # map, the layouts decide (ambiguous is set)
    for reg in regions:
        reg.pointers = {}
        for a, v, cat, param in reg.records:
            ambiguous = cat == 'tag_data_or_block'
            if ambiguous:
                k = resolve(reg, v).alloc.get(v)
                cat = 'tag_block' if k and k[0] == 'block' else 'tag_data'
            elif cat == 'tag_block_large':
                cat = 'tag_block'
            if cat in POINTER_CATEGORIES:
                reg.pointers[a] = (cat, v, ambiguous)
        del reg.records

    S = {'info': info, 'tags': tags, 'regions': regions, 'bsps': bsps, 'instances': instances,
         'tag_header': tag_header, 'resolve': resolve, 'tag_by_index': tag_by_index,
         'regions_by_name': {r.name: r for r in regions}}
    oracle_invariants(S)
    return S


def oracle_invariants(S):
    """the invariants the study established on every retail map"""
    tags, resolve = S['tags'], S['resolve']
    for reg in S['regions']:
        ext = sorted([(s, s + c) for s, c in reg.data_ext.items()] +
                     [(s, s + c) for s, c in reg.vext.items()])
        lo = [x[0] for x in ext]
        headers = 0
        for a, (cat, v, _) in reg.pointers.items():
            k = bisect.bisect_right(lo, a) - 1
            if k >= 0 and ext[k][0] <= a < ext[k][1]:
                raise MapError('%s: pointer %08x at %08x lies inside raw data' % (reg.name, v, a))
            r = resolve(reg, v)
            if r is None:
                raise MapError('%s: pointer %08x at %08x points nowhere' % (reg.name, v, a))
            if v & 3 and cat not in ('instance_name', 'tag_reference', 'tag_reference_stale_index', 'tag_data'):
                raise MapError('%s: %s %08x at %08x is not 4-aligned' % (reg.name, cat, v, a))
            if cat in ('tag_block', 'tag_data') and v not in r.alloc:
                raise MapError('%s: %s %08x at %08x is not an allocation' % (reg.name, cat, v, a))
            if cat == 'bsp_reference_base_address' and r is tags:
                raise MapError('%s: BSP reference %08x at %08x points into the tag data' % (reg.name, v, a))
            if cat == 'd3d_header_pointer':
                headers += 1
        if headers != len(reg.d3d):
            raise MapError('%s: %d hardware_format pointers for %d D3D headers' % (reg.name, headers, len(reg.d3d)))
    for b in S['bsps']:
        if sum(1 for a, (cat, v, _) in tags.pointers.items()
               if cat == 'bsp_reference_base_address' and v == b['base_address']) != 1:
            raise MapError('BSP %d is not referenced exactly once' % b['index'])


# ---------- the shape of the data: allocations and the pointers they hold

def map_graph(S):
    """what the layouts are learned from: the arrays of a map (tag roots,
    block arrays, and tag data an ambiguous pointer reaches, which may be a
    block array until the layouts decide), the allocation boundaries of each
    region and the oracle's pointer words other than the fixed structures'"""
    nodes = {}       # (region name, start) -> node

    def add(region, start, count, maybe):
        node = nodes.get((region, start))
        if node is None:
            nodes[(region, start)] = {'region': region, 'start': start, 'count': count, 'maybe': maybe}
        elif not maybe:
            node['count'], node['maybe'] = count, False

    for t in S['instances']:
        if t['base']:
            add('tags', t['base'], 1, False)
    for b in S['bsps']:
        add(b['region'].name, b['header']['base_address'], 1, False)
    for reg in S['regions']:
        for a, (cat, v, ambiguous) in reg.pointers.items():
            if cat == 'tag_block' or ambiguous:
                add(S['resolve'](reg, v).name, v, reg.word(a - 4), ambiguous)
    graph = {'nodes': nodes, 'roots': [], 'starts': {}, 'extents': {}, 'bounds': {}, 'pointers': {},
             'blocks': set(), 'kinds': {}}
    for reg in S['regions']:
        # the Data of a vertex buffer with no vertices is no allocation (it
        # may point at whatever follows)
        graph['starts'][reg.name] = sorted(
            s for s, (kind, size, ref) in reg.alloc.items()
            if not (kind.endswith('vb_data') and reg.vext.get(s, 1) == 0) or (reg.name, s) in nodes)
        extents = {}
        for s, (kind, size, ref) in reg.alloc.items():
            if kind.endswith('_headers'):
                extents[s] = size * D3D_HEADER_SIZE
            elif kind == 'tag_instances':
                extents[s] = size * TAG_INSTANCE_SIZE
            elif kind == 'tag_header':
                extents[s] = 0x24
            elif kind == 'bsp_header':
                extents[s] = 0x18
        for s, size in list(reg.data_ext.items()) + list(reg.vext.items()):
            extents[s] = max(extents.get(s, 0), size)
        graph['extents'][reg.name] = extents
        graph['bounds'][reg.name] = (reg.base, reg.end)
        graph['kinds'][reg.name] = {s: kind for s, (kind, size, ref) in reg.alloc.items()}
        graph['blocks'].update((reg.name, s) for s, (kind, size, ref) in reg.alloc.items() if kind == 'block')
        pointers = []
        for a, (cat, v, ambiguous) in sorted(reg.pointers.items()):
            if cat in FIXED_CATEGORIES:
                continue
            target = None
            if ambiguous:
                kind = 'block_or_data'
                target = S['resolve'](reg, v).name
            elif cat == 'tag_block':
                kind = 'block'
                target = S['resolve'](reg, v).name
            elif cat == 'tag_data':
                kind = 'data'
            elif cat in ('tag_reference', 'tag_reference_stale_index'):
                kind = 'reference'
            elif cat == 'd3d_header_pointer':
                hreg = reg if v in reg.d3d else S['tags']
                kind = 'index_header' if hreg.d3d[v][0] == 'ib' else 'vertex_header'
            elif cat == 'triangle_buffer_base_address':
                kind = 'index_data'
            elif cat == 'bsp_reference_base_address':
                kind = 'structure_bsp'
            else:
                raise MapError('unknown category %s' % cat)
            pointers.append((a, kind, target, v))
        graph['pointers'][reg.name] = pointers
    for t in S['instances']:
        if t['base']:
            graph['roots'].append((fourcc(t['group']), ('tags', t['base'])))
    for b in S['bsps']:
        graph['roots'].append(('sbsp', (b['region'].name, b['header']['base_address'])))
    graph['roots_set'] = set(k for group, k in graph['roots'])
    return graph


class Layout:
    def __init__(self, key):
        self.key = key          # (group,) for a root, parent key + (offset,) for a block field
        self.nodes = []         # (map name, node)
        self.size = None
        self.exact = 0          # arrays of two or more elements whose gap fits the size exactly
        self.fields = {}        # offset of the pointer word -> kind
        self.children = {}      # offset of the pointer word -> child key
        self.declared = {}      # offset of the pointer word -> the member declaring it (DECLARED_FIELDS)
        self.origin = None      # of a layout no map has: the structure it is built from, or ELEMENTS_UNKNOWN

    def name(self):
        return key_name(self.key)


def key_name(key):
    """the group tag, then the offset of each tag_block field on the way"""
    return key[0] + ''.join('+%#x' % (o - POINTER_WORD['block']) for o in key[1:])


def element_size(layout, notes):
    """the element size of a layout from the gaps between its arrays and the
    next allocation: count * size + 0-3 bytes of padding. The size most
    arrays of two or more elements fit exactly, unless as many arrays are too
    big for their gaps at that size; otherwise the smallest gap per element.
    An array whose gap is too small for it has another allocation inside
    it (reported)."""
    exact = collections.Counter()
    for m, n in layout.nodes:
        if n['count'] >= 2:
            e = n['gap'] // n['count']
            if n['gap'] - n['count'] * e <= 3:
                exact[e] += 1
    size = max(exact, key=lambda e: (exact[e], e)) if exact else None
    if size is not None and sum(1 for m, n in layout.nodes if n['count'] * size > n['gap']) < exact[size]:
        layout.exact = exact[size]
    else:
        size = min(n['gap'] // n['count'] for m, n in layout.nodes)
    for m, n in layout.nodes:
        if n['count'] * size > n['gap']:
            notes.append('%s: %s at %08x, %d elements of %#x, overlaps the allocation %#x bytes in' % (
                m, layout.name(), n['start'], n['count'], size, n['gap']))
    return size


def learn(graphs):
    """layouts for every tag group root and every block field, from the
    graphs of all maps. Arrays that only pointers outside every array reach
    (left-over tool memory) are no allocations: learning repeats without
    them until nothing changes."""
    removed = {m: frozenset() for m in graphs}
    for _ in range(16):
        result = learn_once(graphs, removed)
        unreached = {m: frozenset(k for k, node in g['nodes'].items()
                                  if node['key'] is None and k in g['blocks'] and not node['maybe'])
                     for m, g in graphs.items()}
        if unreached == removed:
            return result + (removed,)
        removed = unreached
    raise MapError('learning did not settle')


def learn_once(graphs, removed):
    layouts, conflicts, notes, orphans = {}, [], [], []
    for mapname, g in sorted(graphs.items()):
        nodes = g['nodes']
        for node in nodes.values():
            node['edges'], node['key'], node['gap'] = [], None, None
        for region, starts in g['starts'].items():
            active = [s for s in starts if (region, s) not in removed[mapname]]
            end = g['bounds'][region][1]
            for s in starts:
                node = nodes.get((region, s))
                if node is not None:
                    j = bisect.bisect_right(active, s)
                    node['gap'] = (active[j] if j < len(active) else end) - s
            for a, kind, target, v in g['pointers'][region]:
                k = bisect.bisect_right(active, a) - 1
                s = active[k] if k >= 0 else None
                node = nodes.get((region, s))
                if node is not None:
                    node['edges'].append((a - s, kind, target, v))
                    continue
                hole = s + g['extents'][region].get(s, 0) if s is not None else g['bounds'][region][0]
                nxt = active[k + 1] if k + 1 < len(active) else end
                if a < hole:
                    conflicts.append('%s: %s %08x at %08x lies inside the allocation at %08x' % (
                        mapname, kind, v, a, s))
                orphans.append((mapname, region, a, kind, v, hole, nxt))
        for group, k in g['roots']:
            key = (group,)
            layouts.setdefault(key, Layout(key)).nodes.append((mapname, nodes[k]))
            nodes[k]['key'] = key
    queue = collections.deque(sorted(layouts))
    while queue:
        key = queue.popleft()
        L = layouts[key]
        L.size = element_size(L, notes)
        by_offset = collections.defaultdict(list)
        for mapname, node in L.nodes:
            end = node['start'] + node['count'] * L.size
            for rel, kind, target, v in node['edges']:
                index, off = divmod(rel, L.size)
                if index >= node['count']:
                    orphans.append((mapname, node['region'], node['start'] + rel, kind, v, end,
                                    node['start'] + node['gap']))
                    continue
                by_offset[off].append((mapname, node, kind, target, v))
        for off in sorted(by_offset):
            uses = by_offset[off]
            kinds = set(u[2] for u in uses if u[2] != 'block_or_data')
            if len(kinds) > 1:
                conflicts.append('%s+%#x is %s' % (L.name(), off, ' and '.join(sorted(kinds))))
                continue
            if kinds:
                kind = kinds.pop()
            else:
                # only ambiguous words: a block when an array it reaches holds pointers
                kind = 'block' if any(graphs[u[0]]['nodes'][(u[3], u[4])]['edges'] for u in uses) else 'data'
                notes.append('%s+%#x: block or data in every map; %s' % (L.name(), off, kind))
            if kind == 'data':
                for mapname, node, k, target, v in uses:
                    child = graphs[mapname]['nodes'].get((target, v)) if k == 'block_or_data' else None
                    if child and child['edges']:
                        conflicts.append('%s: %s+%#x is data, but the data at %08x holds pointers' % (
                            mapname, L.name(), off, v))
            L.fields[off] = kind
            for mapname, node, k, target, v in uses:
                if kind != 'block':
                    continue
                child = graphs[mapname]['nodes'][(target, v)]
                ckey = key + (off,)
                L.children[off] = ckey
                if child['key'] is not None:
                    if child['key'] != ckey:
                        conflicts.append('%s: the array at %08x is reached as %s and as %s' % (
                            mapname, v, key_name(child['key']), key_name(ckey)))
                    continue
                child['key'] = ckey
                if ckey not in layouts:
                    layouts[ckey] = Layout(ckey)
                    queue.append(ckey)
                layouts[ckey].nodes.append((mapname, child))
    return layouts, conflicts, notes, orphans


# ---------- fields the source declares and every retail map leaves empty

# The oracle sees a pointer field only where some map holds a pointer. The
# game's structures declare more tag blocks, tag references and tag data
# than that: fields every element of every retail map leaves empty. A map
# of the same build that fills one (a custom map) has an address there too,
# so the layouts carry these fields as well, marked 'declared'.
#
# Each entry was derived from the structure the source reads the layout as
# (the tag_get casts, block comments and TAG_BLOCK_GET_ELEMENT uses that
# cross_check follows, over the structures of the headers and of the .c
# files) and kept only where the bytes of every retail element are the null
# form of the kind: a tag block {0, 0, 0}; a tag reference {group tag or
# NONE, no name, no name length, index NONE}; a tag data with no size, no
# address and no definition. Those are the forms every null pointer field
# the maps do use has. study() checks the forms again on every map,
# check_declared() the structures (clang's record layouts), and
# add_declared_fields() that each entry's line still declares the member.
#
#   (layout, offset, kind, the structure of the layout, the member
#    (declaring structure.field), where it is declared under source/
#    [, the elements of a tag block])
#
# The elements of a block are
#   ('bipd+0x14c', 'object_definition_widget'): the learned layout of a
#       block with the same elements, and the structure the source reads
#       them as;
#   ('hs_source_file', 0x34, ((0x20, 'data', 'source'),)): a structure of
#       the source (or a scalar of SCALAR_ELEMENTS), its size and its own
#       pointer fields, for elements no map has;
#   None: the source does not say what the elements are. The layout is then
#       one-byte elements with no fields: the walker moves the block's
#       address, checks that at least count bytes lie inside the data, and
#       moves nothing inside the elements. It cannot fail on a retail map
#       (the field is empty in all of them) and it never touches a word it
#       knows nothing about; pointers such elements might hold stay
#       unmoved.
DECLARED_FIELDS = (
    ('actr', 0x13c, 'reference', 'actor_definition',
     'actor_definition.unused_ranged_weapon_reference', 'ai/actor_definitions.h:268'),
    ('actr', 0x258, 'reference', 'actor_definition',
     'actor_definition.unused_grenade_reference', 'ai/actor_definitions.h:270'),
    ('actr', 0x4b8, 'reference', 'actor_definition',
     'actor_definition.unused_major_upgrade_reference', 'ai/actor_definitions.h:279'),
    ('actv', 0x1c0, 'reference', 'actor_variant_definition',
     'actor_item_properties.equipment_reference', 'ai/actor_definitions.h:381'),
    ('bipd', 0x80, 'reference', 'biped_definition', '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('bipd', 0xa0, 'reference', 'biped_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('bipd', 0x2cc, 'block', 'biped_definition', '_unit_definition.powered_seats', 'units/unit_definitions.h:175',
     ('vehi+0x2cc', 'powered_seat_definition')),
    ('bipd', 0x2e4, 'block', 'biped_definition', '_unit_definition.seats', 'units/unit_definitions.h:177',
     ('vehi+0x2e4', 'unit_seat')),
    ('bitm', 0x30, 'data', 'bitmap_group', 'bitmap_group.pixel_data', 'bitmaps/bitmap_group.h:86'),
    ('coll', 0x188, 'reference', 'collision_model',
     'damage_resistance.shield_damaged_effect', 'objects/damage_resistances.h:78'),
    ('coll', 0x24c, 'block', 'collision_model', 'damage_resistance.modifiers', 'objects/damage_resistances.h:86',
     None),
    ('ctrl', 0x80, 'reference', 'control_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('ctrl', 0x90, 'reference', 'control_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('ctrl', 0xa0, 'reference', 'control_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('ctrl', 0x14c, 'block', 'control_definition', '_object_definition.widgets', 'objects/object_definitions.h:157',
     ('bipd+0x14c', 'object_definition_widget')),
    ('ctrl', 0x170, 'block', 'control_definition',
     '_object_definition.predicted_resources', 'objects/object_definitions.h:160',
     ('bipd+0x170', 'predicted_resource')),
    ('ctrl', 0x1c0, 'reference', 'control_definition',
     '_device_definition.positive_stop_effect', 'devices/device_definitions.h:65'),
    ('ctrl', 0x1d0, 'reference', 'control_definition',
     '_device_definition.negative_stop_effect', 'devices/device_definitions.h:66'),
    ('ctrl', 0x1e0, 'reference', 'control_definition',
     '_device_definition.depowered_effect', 'devices/device_definitions.h:67'),
    ('ctrl', 0x1f0, 'reference', 'control_definition',
     '_device_definition.repowered_effect', 'devices/device_definitions.h:68'),
    ('ctrl', 0x20c, 'reference', 'control_definition',
     '_device_definition.delay_effect', 'devices/device_definitions.h:71'),
    ('ctrl', 0x2e8, 'reference', 'control_definition',
     '_control_definition.on_effect', 'devices/device_controls.h:43'),
    ('ctrl', 0x2f8, 'reference', 'control_definition',
     '_control_definition.off_effect', 'devices/device_controls.h:44'),
    ('ctrl', 0x308, 'reference', 'control_definition',
     '_control_definition.denied_effect', 'devices/device_controls.h:45'),
    ('ctrl+0x164', 0x20, 'block', 'object_change_color_definition',
     'object_change_color_definition.permutations', 'objects/object_definitions.h:131',
     ('bipd+0x164+0x20', 'object_change_color_permutation')),
    ('elec+0xa4', 0x4c, 'reference', 'shader_effect_definition',
     'shader_effect_definition.secondary_map', 'shaders/shader_definitions.h:63'),
    ('eqip', 0x38, 'reference', 'equipment_definition',
     '_object_definition.animation_graph', 'objects/object_definitions.h:144'),
    ('eqip', 0x80, 'reference', 'equipment_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('eqip', 0x90, 'reference', 'equipment_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('eqip', 0xa0, 'reference', 'equipment_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('eqip', 0x140, 'block', 'equipment_definition',
     '_object_definition.attachments', 'objects/object_definitions.h:156',
     ('bipd+0x140', 'object_attachment_definition')),
    ('eqip', 0x14c, 'block', 'equipment_definition', '_object_definition.widgets', 'objects/object_definitions.h:157',
     ('bipd+0x14c', 'object_definition_widget')),
    ('eqip', 0x158, 'block', 'equipment_definition',
     '_object_definition.functions', 'objects/object_definitions.h:158',
     ('bipd+0x158', 'object_function_definition')),
    ('eqip', 0x164, 'block', 'equipment_definition',
     '_object_definition.change_colors', 'objects/object_definitions.h:159',
     ('bipd+0x164', 'object_change_color_definition')),
    ('eqip', 0x248, 'reference', 'equipment_definition',
     '_item_definition.material_effects', 'items/item_definitions.h:42'),
    ('eqip', 0x258, 'reference', 'equipment_definition',
     '_item_definition.collision_sound', 'items/item_definitions.h:43'),
    ('font', 0x3c, 'reference', 'font_header', 'font_header.style_fonts', 'text/font_group.h:46'),
    ('font', 0x4c, 'reference', 'font_header', 'font_header.style_fonts', 'text/font_group.h:46'),
    ('font', 0x5c, 'reference', 'font_header', 'font_header.style_fonts', 'text/font_group.h:46'),
    ('font', 0x6c, 'reference', 'font_header', 'font_header.style_fonts', 'text/font_group.h:46'),
    ('garb', 0x38, 'reference', 'item_definition',
     '_object_definition.animation_graph', 'objects/object_definitions.h:144'),
    ('garb', 0x80, 'reference', 'item_definition', '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('garb', 0x90, 'reference', 'item_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('garb', 0x140, 'block', 'item_definition', '_object_definition.attachments', 'objects/object_definitions.h:156',
     ('bipd+0x140', 'object_attachment_definition')),
    ('garb', 0x14c, 'block', 'item_definition', '_object_definition.widgets', 'objects/object_definitions.h:157',
     ('bipd+0x14c', 'object_definition_widget')),
    ('garb', 0x158, 'block', 'item_definition', '_object_definition.functions', 'objects/object_definitions.h:158',
     ('bipd+0x158', 'object_function_definition')),
    ('garb', 0x164, 'block', 'item_definition',
     '_object_definition.change_colors', 'objects/object_definitions.h:159',
     ('bipd+0x164', 'object_change_color_definition')),
    ('garb', 0x248, 'reference', 'item_definition',
     '_item_definition.material_effects', 'items/item_definitions.h:42'),
    ('garb', 0x2e8, 'reference', 'item_definition',
     '_item_definition.detonating_effect', 'items/item_definitions.h:47'),
    ('garb', 0x2f8, 'reference', 'item_definition',
     '_item_definition.detonation_effect', 'items/item_definitions.h:48'),
    ('grhi', 0x7c, 'block', 'grenade_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('grhi', 0xe4, 'block', 'grenade_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('grhi', 0x168, 'block', 'grenade_hud_interface_definition',
     'grenade_hud_interface_definition.warning_sounds', 'interface/hud_weapon.c:274',
     ('unhi+0x3c0', 'hud_sound_definition')),
    ('hudg', 0x3e0, 'reference', 'hud_globals_definition',
     'hud_globals_definition.checkpoint_sound', 'interface/hud_definitions.h:152'),
    ('lifi', 0x38, 'reference', 'device_definition',
     '_object_definition.animation_graph', 'objects/object_definitions.h:144'),
    ('lifi', 0x80, 'reference', 'device_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('lifi', 0x90, 'reference', 'device_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('lifi', 0xa0, 'reference', 'device_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('lifi', 0x14c, 'block', 'device_definition', '_object_definition.widgets', 'objects/object_definitions.h:157',
     ('bipd+0x14c', 'object_definition_widget')),
    ('lifi', 0x164, 'block', 'device_definition',
     '_object_definition.change_colors', 'objects/object_definitions.h:159',
     ('bipd+0x164', 'object_change_color_definition')),
    ('lifi', 0x1a0, 'reference', 'device_definition',
     '_device_definition.positive_start_effect', 'devices/device_definitions.h:63'),
    ('lifi', 0x1b0, 'reference', 'device_definition',
     '_device_definition.negative_start_effect', 'devices/device_definitions.h:64'),
    ('lifi', 0x1c0, 'reference', 'device_definition',
     '_device_definition.positive_stop_effect', 'devices/device_definitions.h:65'),
    ('lifi', 0x1d0, 'reference', 'device_definition',
     '_device_definition.negative_stop_effect', 'devices/device_definitions.h:66'),
    ('lifi', 0x1e0, 'reference', 'device_definition',
     '_device_definition.depowered_effect', 'devices/device_definitions.h:67'),
    ('lifi', 0x1f0, 'reference', 'device_definition',
     '_device_definition.repowered_effect', 'devices/device_definitions.h:68'),
    ('lifi', 0x20c, 'reference', 'device_definition',
     '_device_definition.delay_effect', 'devices/device_definitions.h:71'),
    ('ligh', 0x7c, 'reference', 'point_light_definition',
     'point_light_gel_parameters.secondary_map', 'rasterizer/xbox/rasterizer_xbox_environment.c:412'),
    ('lsnd', 0x2c, 'reference', 'looping_sound_definition',
     'looping_sound_definition.continuous_damage_effect', 'sound/sound_definitions.h:185'),
    ('mach', 0x80, 'reference', 'machine_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('mach', 0x90, 'reference', 'machine_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('mach', 0xa0, 'reference', 'machine_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('mach', 0x1e0, 'reference', 'machine_definition',
     '_device_definition.depowered_effect', 'devices/device_definitions.h:67'),
    ('mach', 0x1f0, 'reference', 'machine_definition',
     '_device_definition.repowered_effect', 'devices/device_definitions.h:68'),
    ('mach+0x164', 0x20, 'block', 'object_change_color_definition',
     'object_change_color_definition.permutations', 'objects/object_definitions.h:131',
     ('bipd+0x164+0x20', 'object_change_color_permutation')),
    ('matg+0x134', 0x19c, 'reference', 'game_globals_rasterizer_data',
     'game_globals_rasterizer_data.distance_attenuation_2d_for_the_pc', 'game/game_globals.h:140'),
    ('matg+0x194', 0x2e4, 'reference', 'material_definition',
     'breakable_surface.effect', 'effects/material_effect_definitions.h:65'),
    ('mode+0xc4+0x40', 0x4c, 'block', 'model_region_permutation',
     'model_region_permutation.markers', 'models/model_definitions.h:93', None),
    ('mode+0xd0+0x24', 0x20, 'block', 'model_geometry_part',
     'model_geometry_part.uncompressed_vertices', 'models/models.c:192', None),
    ('mode+0xd0+0x24', 0x2c, 'block', 'model_geometry_part',
     'model_geometry_part.compressed_vertices', 'models/models.c:193', None),
    ('mode+0xd0+0x24', 0x38, 'block', 'model_geometry_part', 'model_geometry_part.triangles', 'models/models.c:194',
     None),
    ('pctl+0x5c+0x68', 0xb4, 'block', 'particle_system_type_state',
     'particle_system_type_state.physics_constants', 'effects/particle_system_definitions.h:137', None),
    ('proj', 0x70, 'reference', 'projectile_definition',
     '_object_definition.collision_model', 'objects/object_definitions.h:146'),
    ('proj', 0x80, 'reference', 'projectile_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('proj', 0x90, 'reference', 'projectile_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('proj', 0xa0, 'reference', 'projectile_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('proj+0x164', 0x20, 'block', 'object_change_color_definition',
     'object_change_color_definition.permutations', 'objects/object_definitions.h:131',
     ('bipd+0x164+0x20', 'object_change_color_permutation')),
    ('rain+0x24', 0x1f4, 'reference', 'weather_particle_type_definition',
     'shader_effect_definition.secondary_map', 'shaders/shader_definitions.h:63'),
    ('sbsp', 0x1f0, 'block', 'structure_bsp',
     'structure_bsp.pathfinding_edges', 'structures/structure_bsp_definitions.h:210', None),
    ('sbsp', 0x270, 'block', 'structure_bsp', 'leaf_map.leaves', 'structures/leaf_map.h:27',
     ('map_leaf', 0x18, ((0x0, 'block', 'faces',
       ('map_leaf_face', 0x10, ((0x4, 'block', 'vertices', ('real_point2d', 0x8, ())),))),
      (0xc, 'block', 'portal_designators', ('long', 0x4, ()))))),
    ('sbsp', 0x27c, 'block', 'structure_bsp', 'leaf_map.portals', 'structures/leaf_map.h:28',
     ('leaf_portal', 0x18, ((0xc, 'block', 'vertices', ('real_point3d', 0xc, ())),))),
    ('sbsp+0x104+0x14', 0xd8, 'data', 'structure_material',
     'structure_material.uncompressed_vertex_data', 'structures/structure_bsp_definitions.h:64'),
    ('sbsp+0x134', 0x44, 'block', 'structure_cluster_graph',
     'structure_cluster_graph.surface_indices', 'structures/structures.c:129', None),
    ('scen', 0x80, 'reference', 'object_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('scen', 0x90, 'reference', 'object_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('scen', 0xa0, 'reference', 'object_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('schi', 0x38, 'reference', 'shader_transparent_chicago_definition',
     'shader_transparent_chicago.lens_flare', 'rasterizer/xbox/rasterizer_xbox_transparent_geometry.c:547'),
    ('schi', 0x48, 'block', 'shader_transparent_chicago_definition',
     'shader_transparent_chicago.extra_layers', 'rasterizer/xbox/rasterizer_xbox_transparent_geometry.c:548',
     ('sotr+0x48', 'tag_reference')),
    ('scnr', 0x0, 'reference', 'scenario', 'scenario.ugly_structure_bsp', 'scenario/scenario_definitions.h:246'),
    ('scnr', 0x10, 'reference', 'scenario', 'scenario.unloved_globals', 'scenario/scenario_definitions.h:247'),
    ('scnr', 0x20, 'reference', 'scenario', 'scenario.bad_sky', 'scenario/scenario_definitions.h:248'),
    ('scnr', 0xf8, 'block', 'scenario', 'scenario.functions', 'scenario/scenario_definitions.h:257', None),
    ('scnr', 0x2f4, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x300, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x30c, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x318, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x324, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x330, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x33c, 'block', 'scenario', 'scenario.unused_blocks', 'scenario/scenario_definitions.h:281', None),
    ('scnr', 0x45c, 'block', 'scenario', 'scenario.ai_recording_references', 'scenario/scenario_definitions.h:299',
     ('ai_recording_reference_definition', 0x28, ())),
    ('scnr', 0x4c0, 'block', 'scenario', 'scenario.hs_source_files', 'scenario/scenario_definitions.h:306',
     ('hs_source_file', 0x34, ((0x20, 'data', 'source'),))),
    ('scnr+0x390', 0x5c, 'reference', 'scenario_starting_equipment',
     'scenario_starting_equipment.item_collection', 'scenario/scenario_definitions.h:240'),
    ('scnr+0x390', 0x6c, 'reference', 'scenario_starting_equipment',
     'scenario_starting_equipment.item_collection', 'scenario/scenario_definitions.h:240'),
    ('scnr+0x390', 0x7c, 'reference', 'scenario_starting_equipment',
     'scenario_starting_equipment.item_collection', 'scenario/scenario_definitions.h:240'),
    ('scnr+0x390', 0x8c, 'reference', 'scenario_starting_equipment',
     'scenario_starting_equipment.item_collection', 'scenario/scenario_definitions.h:240'),
    ('scnr+0x42c+0x80', 0xdc, 'block', 'squad_definition',
     'squad_definition.unused_block', 'ai/ai_scenario_definitions.h:309', None),
    ('scnr+0x438', 0x48, 'block', 'ai_command_list_definition',
     'ai_command_list_definition.unused_blocks', 'ai/ai_scenario_definitions.h:434', None),
    ('scnr+0x438', 0x54, 'block', 'ai_command_list_definition',
     'ai_command_list_definition.unused_blocks', 'ai/ai_scenario_definitions.h:434', None),
    ('scnr+0x468', 0x68, 'block', 'scenario_conversation_definition_view',
     'scenario_conversation_definition_view.unused', 'ai/ai_communication.c:795', None),
    ('scnr+0x468+0x5c', 0x5c, 'reference', 'scenario_conversation_line_view',
     'scenario_conversation_line_view.dialogue', 'ai/ai_communication.c:822'),
    ('scnr+0x468+0x5c', 0x6c, 'reference', 'scenario_conversation_line_view',
     'scenario_conversation_line_view.dialogue', 'ai/ai_communication.c:822'),
    ('sgla', 0x64, 'reference', 'shader_transparent_glass_definition',
     'shader_transparent_glass_definition.tint_map', 'rasterizer/xbox/rasterizer_xbox_transparent_geometry.c:571'),
    ('sky ', 0xac, 'block', 'sky', 'sky.render_model_regions', 'render/render_sky.c:41',
     ('sky_render_model_region', 0x24, ())),
    ('snd!+0x98+0x3c', 0x68, 'data', 'sound_permutation',
     'sound_permutation.subtitle_data', 'sound/sound_definitions.h:91'),
    ('ssce', 0x28, 'reference', 'object_definition', '_object_definition.model', 'objects/object_definitions.h:143'),
    ('ssce', 0x38, 'reference', 'object_definition',
     '_object_definition.animation_graph', 'objects/object_definitions.h:144'),
    ('ssce', 0x70, 'reference', 'object_definition',
     '_object_definition.collision_model', 'objects/object_definitions.h:146'),
    ('ssce', 0x80, 'reference', 'object_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('ssce', 0x90, 'reference', 'object_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('ssce', 0xa0, 'reference', 'object_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('ssce', 0x14c, 'block', 'object_definition', '_object_definition.widgets', 'objects/object_definitions.h:157',
     ('bipd+0x14c', 'object_definition_widget')),
    ('ssce', 0x158, 'block', 'object_definition', '_object_definition.functions', 'objects/object_definitions.h:158',
     ('bipd+0x158', 'object_function_definition')),
    ('ssce', 0x164, 'block', 'object_definition',
     '_object_definition.change_colors', 'objects/object_definitions.h:159',
     ('bipd+0x164', 'object_change_color_definition')),
    ('ssce', 0x170, 'block', 'object_definition',
     '_object_definition.predicted_resources', 'objects/object_definitions.h:160',
     ('bipd+0x170', 'predicted_resource')),
    ('udlg', 0x220, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x230, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x240, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x270, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x290, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x2b0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x2c0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x2d0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x440, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x470, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x480, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x490, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x4b0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x510, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x520, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x530, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x540, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x570, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x580, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x5a0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x5c0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x890, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x8d0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0x9c0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0xa00, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0xa10, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0xa80, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('udlg', 0xab0, 'reference', 'dialogue_definition',
     'dialogue_definition.vocalizations', 'units/unit_dialogue.c:157'),
    ('unhi', 0x7c, 'block', 'unit_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi', 0xe4, 'block', 'unit_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi', 0x14c, 'block', 'unit_hud_interface_definition',
     'meter_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:83', None),
    ('unhi', 0x1d4, 'block', 'unit_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi', 0x23c, 'block', 'unit_hud_interface_definition',
     'meter_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:83', None),
    ('unhi', 0x2c4, 'block', 'unit_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi', 0x32c, 'block', 'unit_hud_interface_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi', 0x3a4, 'block', 'unit_hud_interface_definition',
     'auxilary_panel_definition.auxilary_overlays', 'interface/unit_hud_interface_definition.h:133',
     ('auxilary_overlay_definition', 0x84, ((0x24, 'reference', 'interface_bitmap'),
      (0x58, 'block', 'multitexture_overlays', ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition'))))),
    ('unhi+0x3cc', 0x6c, 'block', 'auxilary_meter_definition',
     'static_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:62',
     ('wphi+0x60+0x7c', 'multitexture_overlay_hud_element_definition')),
    ('unhi+0x3cc', 0xd4, 'block', 'auxilary_meter_definition',
     'meter_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:83', None),
    ('vcky+0x30', 0x10, 'reference', 'virtual_keyboard_key',
     'virtual_keyboard_key.unselected_background_bitmap_tag', 'interface/virtual_keyboard.c:215'),
    ('vehi', 0x90, 'reference', 'vehicle_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('vehi', 0x188, 'reference', 'vehicle_definition',
     '_unit_definition.integrated_light_toggle_effect', 'units/unit_definitions.h:130'),
    ('vehi', 0x24c, 'reference', 'vehicle_definition',
     '_unit_definition.spawned_actor_variant', 'units/unit_definitions.h:152'),
    ('vehi', 0x288, 'reference', 'vehicle_definition',
     '_unit_definition.melee_damage', 'units/unit_definitions.h:164'),
    ('vehi', 0x2b4, 'block', 'vehicle_definition',
     '_unit_definition.dialogue_variants', 'units/unit_definitions.h:169', ('bipd+0x2b4', 'unit_dialogue_variant')),
    ('weap', 0x80, 'reference', 'weapon_definition',
     '_object_definition.physics', 'objects/object_definitions.h:147'),
    ('weap', 0x90, 'reference', 'weapon_definition',
     '_object_definition.modifier_shader', 'objects/object_definitions.h:148'),
    ('weap', 0xa0, 'reference', 'weapon_definition',
     '_object_definition.creation_effect', 'objects/object_definitions.h:149'),
    ('weap', 0x384, 'reference', 'weapon_definition',
     '_weapon_definition.detonation_effect', 'items/weapon_definitions.h:184'),
    ('weap+0x164', 0x20, 'block', 'object_change_color_definition',
     'object_change_color_definition.permutations', 'objects/object_definitions.h:131',
     ('bipd+0x164+0x20', 'object_change_color_permutation')),
    ('weap+0x4f0', 0x48, 'reference', 'weapon_magazine_definition',
     'weapon_magazine_definition.chambering_effect', 'items/weapon_definitions.h:107'),
    ('weap+0x4fc', 0x5c, 'reference', 'weapon_trigger_definition',
     'weapon_trigger_definition.charging_effect', 'items/weapon_definitions.h:138'),
    ('weap+0x4fc+0x108', 0x74, 'reference', 'trigger_firing_effect',
     'trigger_firing_effect.damage_effects', 'items/weapons.c:355'),
    ('wphi', 0xa0, 'block', 'weapon_hud_interface_definition',
     'weapon_hud_interface_definition.warning_sounds', 'interface/hud_weapon.c:302',
     ('unhi+0x3c0', 'hud_sound_definition')),
    ('wphi+0x60+0x7c', 0x84, 'reference', 'multitexture_overlay_hud_element_definition',
     'multitexture_overlay_hud_element_definition.map', 'interface/hud_draw.c:342'),
    ('wphi+0x6c', 0x7c, 'block', 'weapon_hud_meter_element',
     'meter_hud_element_definition.multitexture_overlays', 'interface/unit_hud_interface_definition.h:83', None),
)

# Declared by the source, contradicted by the maps, and so not above:
#   grhi+0x1b8, grenade_hud_interface_definition.messaging_icon_bitmap
#       (interface/hud_weapon.c:276): every element holds 'tmbi' 0x1800f079
#       0 0, which is no tag reference.
#   weap+0x4c0, _weapon_definition.unused_block
#       (items/weapon_definitions.h:218): 17 of 316 elements hold a stale
#       definition word, {0, 0, 0x599808}.
#   bitm+0x1c, bitmap_group.import_bitmap (bitmaps/bitmap_group.h:85) and
#       snd!+0x98+0x3c+0x40, sound_permutation.samples
#       (sound/sound_definitions.h:89): a size with a null address in every
#       element; the data is kept outside the tag data.
#   udlg, 90 of the tag references of dialogue_definition.vocalizations and
#       unused_vocalizations (units/unit_dialogue.c:157): sixteen zero
#       bytes, index 0, where a null reference holds a group tag and the
#       index NONE (as the 28 unused vocalizations above do): padding
#       between the vocalizations.
#   antr+0x68: its elements are animation_graph_node, not the
#       collision_node whose bsps block a match by field name suggests; the
#       bytes there are floats.

ELEMENTS_UNKNOWN = 'elements unknown'
SCALAR_ELEMENTS = {'long': 4}       # block elements that are no structure

# what the walker reads of each kind of field, around its offset
FIELD_EXTENTS = {'block': (0, 12), 'reference': (0, 16), 'data': (0, 20), 'vertex_header': (0, 4),
                 'index_header': (0, 4), 'index_data': (0, 8), 'structure_bsp': (-4, 4)}


def path_key(path):
    """the key of a layout named as key_name names it"""
    parts = path.split('+0x')
    return (parts[0],) + tuple(int(x, 16) + POINTER_WORD['block'] for x in parts[1:])


def declares_member(where, kind, member, cache={}):
    """whether source/<file>:<line> declares the member as that kind of tag
    structure"""
    name, line = where.rsplit(':', 1)
    if name not in cache:
        path = os.path.join(ROOT, 'source', *name.split('/'))
        cache[name] = read_source(path).split('\n') if os.path.isfile(path) else []
    lines, line = cache[name], int(line)
    return 0 < line <= len(lines) and re.match(
        r'\s*struct tag_%s\s+%s\b' % (kind, re.escape(member.rsplit('.', 1)[1])), lines[line - 1]) is not None


def declared_layout(layouts, key, elements, problems):
    """the layout of the elements of a declared block: a learned one, one
    built from a structure of the source, or one-byte elements with no
    fields; returns its key"""
    if elements is not None and not isinstance(elements[1], int):
        source = path_key(elements[0])
        if source in layouts and not layouts[source].origin:
            return source
        problems.append('declared field %s: no layout %s was learned for its elements' % (key_name(key), elements[0]))
        elements = None
    L = layouts[key] = Layout(key)
    if elements is None:
        L.size, L.origin = 1, ELEMENTS_UNKNOWN
        return key
    name, size, fields = elements
    L.size, L.origin = size, name
    for field in fields:
        offset, kind, member = field[:3]
        word = offset + POINTER_WORD[kind]
        L.fields[word] = kind
        L.declared[word] = '%s.%s' % (name, member)
        if kind == 'block':
            L.children[word] = declared_layout(layouts, key + (word,), field[3], problems)
    return key


def add_declared_fields(layouts):
    """add DECLARED_FIELDS to the learned layouts; returns what is wrong
    with the table"""
    problems = []
    for entry in DECLARED_FIELDS:
        path, offset, kind, structure, member, where = entry[:6]
        name = '%s+%#x (%s)' % (path, offset, member)
        key = path_key(path)
        L = layouts.get(key)
        if L is None or L.origin:
            problems.append('declared field %s: no layout %s was learned' % (name, path))
            continue
        word = offset + POINTER_WORD[kind]
        if word in L.fields and word not in L.declared:
            problems.append('declared field %s: the maps have a %s there; take it out of DECLARED_FIELDS' % (
                name, L.fields[word]))
            continue
        low, high = offset + FIELD_EXTENTS[kind][0], offset + FIELD_EXTENTS[kind][1]
        clash = [o for o, k in L.fields.items()
                 if o - POINTER_WORD[k] + FIELD_EXTENTS[k][0] < high and low < o - POINTER_WORD[k] + FIELD_EXTENTS[k][1]]
        if offset & 3 or high > L.size or clash:
            problems.append('declared field %s: a %s does not fit there in the %#x-byte elements of %s' % (
                name, kind, L.size, path))
            continue
        if not declares_member(where, kind, member):
            problems.append('declared field %s: source/%s does not declare a tag %s of that name' % (name, where, kind))
            continue
        L.fields[word] = kind
        L.declared[word] = member
        if kind == 'block':
            L.children[word] = declared_layout(layouts, key + (word,), entry[6], problems)
    return problems


def is_group_tag(value):
    return all(32 <= c < 127 for c in value.to_bytes(4, 'big'))


# ---------- walking a map through the layouts (the C walker's twin)

class Table:
    """the layouts as the walker uses them: key -> (size, [(pointer word
    offset, kind, child key)])"""

    def __init__(self, layouts):
        self.layouts = {}
        self.field = {}         # (key, pointer word offset) -> (kind, child key)
        self.declared = set()   # the fields among them that no map uses (DECLARED_FIELDS)
        self.unknown = set()    # the layouts of elements the source does not describe
        for key, L in layouts.items():
            self.layouts[key] = (L.size, [(off, L.fields[off], L.children.get(off)) for off in sorted(L.fields)])
            for off in L.fields:
                self.field[(key, off)] = (L.fields[off], L.children.get(off))
            self.declared.update((key, off) for off in L.declared)
            if L.origin == ELEMENTS_UNKNOWN:
                self.unknown.add(key)

    def groups(self):
        return sorted(k[0] for k in self.layouts if len(k) == 1)


class Walk:
    """the pointer words of one region found through the layouts; invalid
    pointer fields are counted, not fatal, so that a learning pass can see
    every one of them"""

    def __init__(self, table, region, tag_cache=None):
        self.table, self.r = table, region
        self.moved = set()
        self.fields = collections.defaultdict(lambda: [0, 0, 0, None])   # (key, off) -> [null, valid, invalid, example]
        self.errors = []
        self.layouts = set()                              # the layouts of the arrays walked
        self.by_field = collections.defaultdict(list)    # (key, off) -> addresses moved
        self.invalid = collections.defaultdict(list)     # (key, off) -> addresses not moved
        # declared fields: (key, off) -> [elements, not empty, example, the first element's pointer word]
        self.declared = collections.defaultdict(lambda: [0, 0, None, None])
        self.tag_cache = tag_cache       # [start, end) of the linked tag cache below a BSP
        self.vertex_headers = []         # [(start, count)]
        self.index_headers = []

    def word(self, a):
        return self.r.word(a)

    def inside(self, a, size=4):
        return self.r.contains(a, size)

    def move(self, a):
        self.moved.add(a)

    def header_at(self, arrays, v):
        for start, count in arrays:
            if start <= v < start + count * D3D_HEADER_SIZE and (v - start) % D3D_HEADER_SIZE == 0:
                return True
        return False

    def d3d_array(self, arrays, start, count, what):
        if not count:
            return
        if not self.inside(start, count * D3D_HEADER_SIZE):
            self.errors.append('%s: %d headers at %08x' % (what, count, start))
            return
        arrays.append((start, count))
        for k in range(count):
            a = start + k * D3D_HEADER_SIZE + 4
            v = self.word(a)
            if v:
                if not self.inside(v, 1):
                    self.errors.append('%s %d: Data %08x' % (what, k, v))
                    continue
                self.move(a)

    def check(self, kind, a, v):
        """whether the non-null pointer word v at a is valid for its kind"""
        if kind == 'block':
            count, definition = self.word(a - 4), self.word(a + 4)
            return 0 < count and definition == 0
        if kind == 'reference':
            if self.tag_cache is not None:
                return self.tag_cache[0] <= v < self.tag_cache[1]
            if not self.inside(v, 1):
                return False
            o = v - self.r.base
            return self.r.data.find(b'\0', o, min(self.r.size, o + 256)) >= 0
        if kind == 'data':
            size = self.word(a - 12)
            return size < 0x80000000 and self.inside(v, size or 1)
        if kind == 'vertex_header':
            return self.header_at(self.vertex_headers, v)
        if kind == 'index_header':
            return self.header_at(self.index_headers, v)
        if kind == 'index_data':
            h = self.word(a + 4)
            return self.header_at(self.index_headers, h) and self.word(h + 4) == v
        if kind == 'structure_bsp':
            size = self.word(a - 4)
            return self.r.end <= v and v + size <= self.r.base + TAG_CACHE_SIZE
        raise MapError('unknown kind %s' % kind)

    def empty(self, kind, a):
        """whether the tag block, tag reference or tag data whose pointer
        word is at a is the null form of its kind, as every null pointer
        field of the retail maps is"""
        start = a - POINTER_WORD[kind]
        if kind == 'block':
            return not (self.word(start) or self.word(start + 4) or self.word(start + 8))
        if kind == 'reference':
            group = self.word(start)
            return (group in (0, NONE32) or is_group_tag(group)) and not self.word(start + 4) and \
                not self.word(start + 8) and self.word(start + 12) == NONE32
        # (the file offset of a null tag data is set in the maps)
        return not (self.word(start) or self.word(start + 12) or self.word(start + 16))

    def array(self, key, start, count, where):
        size, fields = self.table.layouts[key]
        if not self.inside(start, count * size):
            self.errors.append('%s: %d elements of %#x at %08x leave the region' % (where, count, size, start))
            return
        self.layouts.add(key)
        if not fields:
            return
        if start & 3:
            self.errors.append('%s: an array of pointers at %08x' % (where, start))
            return
        for i in range(count):
            e = start + i * size
            for off, kind, child in fields:
                a = e + off
                v = self.word(a)
                stat = self.fields[(key, off)]
                if (key, off) in self.table.declared:
                    seen = self.declared[(key, off)]
                    seen[0] += 1
                    if seen[3] is None:
                        seen[3] = a
                    if not self.empty(kind, a):
                        seen[1] += 1
                        if seen[2] is None:
                            seen[2] = '%s at %08x' % (' '.join(
                                '%08x' % self.word(a - POINTER_WORD[kind] + 4 * i)
                                for i in range(FIELD_EXTENTS[kind][1] // 4)), a - POINTER_WORD[kind])
                if v == 0 or a in self.moved:
                    if v == 0:
                        stat[0] += 1
                    continue
                if not self.check(kind, a, v):
                    self.invalid[(key, off)].append(a)
                    stat[2] += 1
                    if stat[3] is None:
                        stat[3] = '%08x at %08x' % (v, a)
                    continue
                stat[1] += 1
                self.by_field[(key, off)].append(a)
                self.move(a)
                if kind == 'block':
                    self.array(child, v, self.word(a - 4), '%s+%#x at %08x' % (key_name(key), off, a))


def walk_tag_data(table, tags):
    """the pointer words of the tag data (linked at tags.base)"""
    w = Walk(table, tags)
    th = struct.unpack_from('<9I', tags.data, 0)
    instances, count, vb_count, vbs, ib_count, ibs = th[0], th[3], th[4], th[5], th[6], th[7]
    if th[8] != TAG_HEADER_SIGNATURE or not w.inside(instances, count * TAG_INSTANCE_SIZE):
        raise MapError('the tag header is broken')
    for off in (0, 0x14, 0x1C):
        if w.word(tags.base + off):
            w.move(tags.base + off)
    w.d3d_array(w.vertex_headers, vbs, vb_count, 'vertex buffer header')
    w.d3d_array(w.index_headers, ibs, ib_count, 'index buffer header')
    groups = set(table.groups())
    for i in range(count):
        t = instances + i * TAG_INSTANCE_SIZE
        group, name, base = w.word(t), w.word(t + 0x10), w.word(t + 0x14)
        if not w.check('reference', t + 0x10, name):
            w.errors.append('tag %d: name %08x' % (i, name))
        else:
            w.move(t + 0x10)
        if not base:
            continue
        if fourcc(group) not in groups:
            w.errors.append('tag %d: no layout for the group %s' % (i, fourcc(group)))
            continue
        w.move(t + 0x14)
        w.array((fourcc(group),), base, 1, 'tag %d' % i)
    return w


def walk_structure_bsp(table, bsp):
    """the pointer words of a structure BSP (linked at bsp.base)"""
    w = Walk(table, bsp, (bsp.end - TAG_CACHE_SIZE, bsp.base))
    hb = struct.unpack_from('<6I', bsp.data, 0)
    if hb[5] != BSP_HEADER_SIGNATURE:
        raise MapError('the BSP header is broken')
    for off in (0, 8, 0x10):
        if w.word(bsp.base + off):
            w.move(bsp.base + off)
    w.d3d_array(w.vertex_headers, hb[2], hb[1], 'vertex buffer header')
    w.d3d_array(w.vertex_headers, hb[4], hb[3], 'lightmap vertex buffer header')
    w.array(('sbsp',), hb[0], 1, 'the BSP')
    return w


# ---------- learning and proving the layouts on every map

def map_paths(maps_dir, names):
    if not names:
        names = sorted(f for f in os.listdir(maps_dir) if f.endswith('.map'))
    return [n if os.path.isabs(n) else os.path.join(maps_dir, n if n.endswith('.map') else n + '.map') for n in names]


def map_name(path):
    return os.path.splitext(os.path.basename(path))[0]


def load(path):
    info, full = read_map(path)
    S = oracle(full, info)
    del full
    return S


def walk_map(table, S):
    walks = {'tags': walk_tag_data(table, S['tags'])}
    for b in S['bsps']:
        walks[b['region'].name] = walk_structure_bsp(table, b['region'])
    return walks


def check_hole(S, orphan):
    """an orphan pointer is accepted when nothing in its region (or, for the
    tag data, in any BSP) points into the hole it lies in"""
    mapname, region, a, kind, v, hole, nxt = orphan
    regions = S['regions'] if region == 'tags' else [S['regions_by_name'][region]]
    for r in regions:
        for w in r.words:
            if hole <= w < nxt:
                return '%s holds %08x' % (r.name, w)
    return None


def study(paths, log, strict=True):
    """learn the layouts from the maps, add the declared fields, then walk
    every map through them: returns the layouts and a per-map account of
    the walk against the oracle. strict: whatever is wrong with
    DECLARED_FIELDS is a conflict (learning from some maps only, a layout
    the table names may not exist)."""
    graphs = {}
    for path in paths:
        t = time.time()
        S = load(path)
        graphs[map_name(path)] = map_graph(S)
        log('  %-15s oracle %6d pointers (%.1f s)' % (map_name(path), sum(len(r.pointers) for r in S['regions']),
                                                     time.time() - t))
    excluded = {m: set() for m in graphs}     # oracle pointers the layouts showed are no pointers
    lookalike_fields = []
    for attempt in range(4):
        layouts, conflicts, notes, orphans, removed = learn(graphs)
        (conflicts if strict else notes).extend(add_declared_fields(layouts))
        table = Table(layouts)
        stats = collections.defaultdict(lambda: [0, 0, 0, None])
        declared = collections.defaultdict(lambda: [0, 0, None])    # (key, off) -> [elements, not empty, example]
        reached = set()
        accounts = {}
        for path in paths:
            m = map_name(path)
            S = load(path)
            walks = walk_map(table, S)
            account = {'orphans': [], 'regions': {}}
            for o in orphans:
                if o[0] == m:
                    why = check_hole(S, o)
                    if why:
                        conflicts.append('%s: %s %08x at %08x lies in no array, but %s, inside the hole %08x-%08x' % (
                            m, o[3], o[4], o[2], why, o[5], o[6]))
                    account['orphans'].append(o)
            for name, w in walks.items():
                reg = S['regions_by_name'][name]
                for k, st in w.fields.items():
                    s = stats[k]
                    s[0] += st[0]
                    s[1] += st[1]
                    s[2] += st[2]
                    if s[3] is None and st[3]:
                        s[3] = '%s %s %s' % (m, name, st[3])
                for k, seen in w.declared.items():
                    d = declared[k]
                    d[0] += seen[0]
                    d[1] += seen[1]
                    if d[2] is None and seen[2]:
                        d[2] = '%s %s %s' % (m, name, seen[2])
                reached.update(w.layouts)
                for e in w.errors:
                    conflicts.append('%s %s: %s' % (m, name, e))
                account['regions'][name] = {
                    'base': reg.base, 'size': reg.size,
                    'oracle': set(reg.pointers),
                    'moved': w.moved, 'by_field': w.by_field, 'invalid': w.invalid,
                    'declared': {k: seen[3] for k, seen in w.declared.items()}}
            account['excluded'] = excluded[m]
            accounts[m] = account
            del S, walks
        dropped = sorted(k for k, s in stats.items() if s[2])
        if not dropped:
            break
        # a field that holds anything but its null form or a valid pointer in
        # some element is no pointer field: the oracle's shapes there were
        # look-alikes; learn again without them
        for k in dropped:
            s = stats[k]
            lookalike_fields.append((key_name(k[0]), k[1], s[2], s[1], s[3]))
            log('  %s, pointer word +%#x: %d values are no pointer (%s), %d look like one: no pointer field' % (
                key_name(k[0]), k[1], s[2], s[3], s[1]))
            if s[1] > s[2]:
                conflicts.append('%s, pointer word +%#x: %d valid and %d invalid values' % (
                    key_name(k[0]), k[1], s[1], s[2]))
            if dict((f[0], f[1]) for f in table.layouts[k[0]][1])[k[1]] == 'block':
                conflicts.append('%s, pointer word +%#x: a block field with invalid values' % (key_name(k[0]), k[1]))
        for m, account in accounts.items():
            for name, r in account['regions'].items():
                for k in dropped:
                    for a in r['by_field'].get(k, []) + r['invalid'].get(k, []):
                        if a in r['oracle']:
                            excluded[m].add((name, a, key_name(k[0]), k[1]))
        exclude_from_graphs(graphs, excluded)
        log('  learning again without %d look-alikes' % sum(len(x) for x in excluded.values()))
    else:
        raise MapError('the layouts did not settle')
    # a declared field is empty in every element of every map, and some map
    # has an element of its layout to show it
    for key, L in sorted(layouts.items()):
        for off in sorted(L.declared):
            d = declared.get((key, off))
            name = '%s+%#x (%s)' % (key_name(key), off - POINTER_WORD[L.fields[off]], L.declared[off])
            if d and d[1]:
                conflicts.append('declared field %s: %d of %d elements are not empty (%s)' % (name, d[1], d[0], d[2]))
            elif not d and not L.origin and strict:
                conflicts.append('declared field %s: no map has an element of the layout' % name)
    return {'layouts': layouts, 'table': table, 'conflicts': conflicts, 'notes': notes, 'stats': stats,
            'declared': dict(declared),
            'lookalike_fields': lookalike_fields,
            'reached': reached, 'accounts': accounts, 'excluded': excluded, 'removed': removed}


def exclude_from_graphs(graphs, excluded):
    for m, items in excluded.items():
        g = graphs[m]
        drop = set((name, a) for name, a, key, off in items)
        targets = collections.Counter()
        for region, pointers in g['pointers'].items():
            for a, kind, target, v in pointers:
                if kind in ('block', 'block_or_data', 'data') and (region, a) not in drop:
                    targets[(target or region, v)] += 1
        for region in list(g['pointers']):
            kept = []
            for a, kind, target, v in g['pointers'][region]:
                if (region, a) in drop:
                    t = (target or region, v)
                    # the allocation only this look-alike made is none
                    if kind in ('block', 'block_or_data', 'data') and not targets[t] and t not in g['roots_set']:
                        if t[1] in g['starts'][t[0]] and g['kinds'][t[0]].get(t[1]) in ('block', 'data'):
                            g['starts'][t[0]].remove(t[1])
                            g['nodes'].pop(t, None)
                    continue
                kept.append((a, kind, target, v))
            g['pointers'][region] = kept


def account_map(account):
    """the walk against the oracle, region by region: (name, oracle,
    accepted orphans, look-alikes, walker, missing, extra)"""
    rows = []
    for name, r in account['regions'].items():
        orphans = set(o[2] for o in account['orphans'] if o[1] == name)
        lookalikes = set(a for n, a, k, off in account.get('excluded', ()) if n == name)
        expected = r['oracle'] - orphans - lookalikes
        rows.append((name, len(r['oracle']), len(orphans), len(lookalikes), len(r['moved']),
                     sorted(expected - r['moved']), sorted(r['moved'] - expected)))
    return rows


# ---------- the generated header

C_KINDS = {
    'block': '_xbox_map_field_tag_block',
    'reference': '_xbox_map_field_tag_reference',
    'data': '_xbox_map_field_tag_data',
    'vertex_header': '_xbox_map_field_vertex_buffer_format',
    'index_header': '_xbox_map_field_index_buffer_format',
    'index_data': '_xbox_map_field_index_buffer_data',
    'structure_bsp': '_xbox_map_field_structure_bsp',
}


def compact(layouts):
    """the layouts with identical subtrees merged, numbered in the order a
    walk over the sorted group tags meets them: [(size, [(offset, kind,
    child number)], [paths], [keys])], {group: number}; the paths are the
    fields that reach a layout, the keys the layouts merged in it"""
    canon = {}

    def form(key):
        if key not in canon:
            L = layouts[key]
            fields = []
            for off in sorted(L.fields):
                kind = L.fields[off]
                child = form(L.children[off]) if kind == 'block' else None
                fields.append((off - POINTER_WORD[kind], kind, child))
            # (elements the source does not describe merge with no elements a map has)
            canon[key] = (L.size, tuple(sorted(fields)), L.origin == ELEMENTS_UNKNOWN)
        return canon[key]

    number, table = {}, []

    def visit(key, path):
        f = form(key)
        L = layouts[key]
        if f in number:
            entry = table[number[f]]
            if key not in entry[3]:
                entry[3].append(key)
            if path not in entry[2]:
                entry[2].append(path)
                for off in sorted(L.fields):
                    if L.fields[off] == 'block':
                        visit(L.children[off], '%s+%#x' % (path, off - POINTER_WORD['block']))
            return number[f]
        number[f] = len(table)
        entry = [f[0], [], [path], [key]]
        table.append(entry)
        for off in sorted(L.fields):
            kind = L.fields[off]
            child = visit(L.children[off], '%s+%#x' % (path, off - POINTER_WORD[kind])) if kind == 'block' else 0
            entry[1].append((off - POINTER_WORD[kind], kind, child))
        entry[1].sort()
        return number[f]

    roots = {}
    for key in sorted(k for k in layouts if len(k) == 1):
        roots[key[0]] = visit(key, key[0])
    for size, fields, paths, keys in table:
        if not 0 < size < 0x80000000:
            raise MapError('a %#x-byte element (%s)' % (size, paths[0]))
        for off, kind, child in fields:
            low, high = FIELD_EXTENTS[kind]
            if off & 3 or size & 3 or off + low < 0 or off + high > size or off > 0xFFFF:
                raise MapError('a %s field at %#x of a %#x-byte element (%s)' % (kind, off, size, paths[0]))
    return table, roots


def header_text(layouts, map_count):
    table, roots = compact(layouts)
    fields = sum(len(f) for s, f, p, k in table)
    groups = sorted((fourcc_value(g), g, n) for g, n in roots.items() if g != 'sbsp')

    def declared_by(keys, off, kind):
        """the members that declare a field, and how many of the layouts
        merged there have it declared"""
        word = off + POINTER_WORD[kind]
        members = [layouts[k].declared[word] for k in keys if word in layouts[k].declared]
        return sorted(set(members)), len(members)

    declared = sum(1 for s, f, p, keys in table for off, kind, child in f
                   if declared_by(keys, off, kind)[1] == len(keys))
    out = []
    w = out.append
    w('/*')
    w('XBOX_MAP_LAYOUTS.H')
    w('')
    w('Generated by tools/xbox_map_layouts.py from the %d retail maps (build' % map_count)
    w('%s); do not edit. Regenerate with' % RETAIL_BUILD)
    w('')
    w('  python3 tools/xbox_map_layouts.py')
    w('')
    w('Where the maps keep absolute addresses: the fixed structures of the tag')
    w('data and of a structure BSP, and for each tag group and each tag block')
    w('field the element size and the fields that hold a pointer. A tag group\'s')
    w('layout is its own (a \'senv\' shader repeats the \'shdr\' fields); blocks')
    w('reached through different fields share a layout when everything below')
    w('them is the same. The comment on each layout names the first field that')
    w('reaches it (group tag, then the offset of each block field on the way).')
    w('')
    w('A field marked \'declared\' holds a pointer in no retail map: the game\'s')
    w('structures declare a tag block, tag reference or tag data there (the comment')
    w('names the member; DECLARED_FIELDS in the tool says where it is declared) and')
    w('every retail element leaves it empty, so a map of the same build that fills')
    w('it has that address moved too. \'declared in 2 of 5\' is a field the maps use')
    w('in the other three of the layouts merged there. Where the source does not')
    w('say what the elements of a declared block are, the layout is one-byte')
    w('elements with no fields: the block\'s address moves and nothing inside it.')
    w('')
    w('Used by xbox_map_rebase.c.')
    w('*/')
    w('')
    w('#ifndef XBOX_MAP_LAYOUTS_H')
    w('#define XBOX_MAP_LAYOUTS_H')
    w('')
    w('/* ---------- fixed structures */')
    w('')
    for name, value in (
            ('TAG_CACHE_SIZE', TAG_CACHE_SIZE),
            ('TAG_HEADER_SIZE', 0x24),
            ('TAG_HEADER_INSTANCES', 0x0),
            ('TAG_HEADER_INSTANCE_COUNT', 0xC),
            ('TAG_HEADER_VERTEX_BUFFER_COUNT', 0x10),
            ('TAG_HEADER_VERTEX_BUFFERS', 0x14),
            ('TAG_HEADER_INDEX_BUFFER_COUNT', 0x18),
            ('TAG_HEADER_INDEX_BUFFERS', 0x1C),
            ('TAG_HEADER_SIGNATURE', 0x20),
            ('TAG_INSTANCE_SIZE', TAG_INSTANCE_SIZE),
            ('TAG_INSTANCE_GROUP', 0x0),
            ('TAG_INSTANCE_NAME', 0x10),
            ('TAG_INSTANCE_ADDRESS', 0x14),
            ('BSP_HEADER_SIZE', 0x18),
            ('BSP_HEADER_ADDRESS', 0x0),
            ('BSP_HEADER_VERTEX_BUFFER_COUNT', 0x4),
            ('BSP_HEADER_VERTEX_BUFFERS', 0x8),
            ('BSP_HEADER_LIGHTMAP_VERTEX_BUFFER_COUNT', 0xC),
            ('BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS', 0x10),
            ('BSP_HEADER_SIGNATURE', 0x14),
            ('D3D_HEADER_SIZE', D3D_HEADER_SIZE),
            ('D3D_HEADER_DATA', 0x4)):
        w('#define XBOX_MAP_%s 0x%X' % (name, value))
    w('#define XBOX_MAP_TAG_HEADER_SIGNATURE_VALUE 0x%08XUL /* tags */' % TAG_HEADER_SIGNATURE)
    w('#define XBOX_MAP_BSP_HEADER_SIGNATURE_VALUE 0x%08XUL /* sbsp */' % BSP_HEADER_SIGNATURE)
    w('/* the build of the maps the layouts were learned from (the map header\'s) */')
    w('#define XBOX_MAP_LAYOUTS_BUILD "%s"' % RETAIL_BUILD)
    w('')
    w('/* ---------- layouts */')
    w('')
    w('enum xbox_map_field_kind')
    w('{')
    w('\t_xbox_map_field_tag_block, /* {count, address, definition} */')
    w('\t_xbox_map_field_tag_reference, /* {group tag, name, name length, index} */')
    w('\t_xbox_map_field_tag_data, /* {size, flags, file offset, address, definition} */')
    w('\t_xbox_map_field_vertex_buffer_format, /* a vertex buffer\'s D3D header */')
    w('\t_xbox_map_field_index_buffer_format, /* a triangle buffer\'s D3D header */')
    w('\t_xbox_map_field_index_buffer_data, /* a triangle buffer\'s indices: its D3D header\'s Data, which follows */')
    w('\t_xbox_map_field_structure_bsp, /* a structure BSP\'s load address, after its file offset and size */')
    w('\tNUMBER_OF_XBOX_MAP_FIELD_KINDS')
    w('};')
    w('')
    w('struct xbox_map_field')
    w('{')
    w('\tunsigned short offset; /* of the tag_block, tag_reference or tag_data; else of the pointer */')
    w('\tunsigned char kind;')
    w('\tunsigned short layout; /* of the elements, for a tag block */')
    w('};')
    w('')
    w('struct xbox_map_layout')
    w('{')
    w('\tunsigned long size; /* of an element (of the root, for a tag group) */')
    w('\tunsigned short first_field;')
    w('\tunsigned short field_count;')
    w('};')
    w('')
    w('struct xbox_map_group_layout')
    w('{')
    w('\tunsigned long group_tag;')
    w('\tunsigned short layout;')
    w('};')
    w('')
    w('#define NUMBER_OF_XBOX_MAP_LAYOUTS %d' % len(table))
    w('#define NUMBER_OF_XBOX_MAP_FIELDS %d /* %d of them declared only */' % (fields, declared))
    w('#define NUMBER_OF_XBOX_MAP_GROUP_LAYOUTS %d' % len(groups))
    w('#define XBOX_MAP_STRUCTURE_BSP_LAYOUT %d' % roots['sbsp'])
    w('')
    w('static const struct xbox_map_layout xbox_map_layouts[NUMBER_OF_XBOX_MAP_LAYOUTS] =')
    w('{')
    first = 0
    for n, (size, flds, paths, keys) in enumerate(table):
        more = ' and %d more' % (len(paths) - 1) if len(paths) > 1 else ''
        origins = sorted(set(layouts[k].origin or '' for k in keys))
        note = ''
        if origins == [ELEMENTS_UNKNOWN]:
            note = '; declared, elements unknown: only the block\'s address moves'
        elif '' not in origins:
            note = '; declared, elements: ' + ', '.join(origins)
        w('\t{ 0x%X, %d, %d }, /* %d: %s%s%s */' % (size, first, len(flds), n, sorted(paths)[0], more, note))
        first += len(flds)
    w('};')
    w('')
    w('static const struct xbox_map_field xbox_map_fields[NUMBER_OF_XBOX_MAP_FIELDS] =')
    w('{')
    first = 0
    for n, (size, flds, paths, keys) in enumerate(table):
        for off, kind, child in flds:
            members, count = declared_by(keys, off, kind)
            notes = ['%d' % n] if first == 0 else []
            if count:
                notes.append('declared%s: %s' % ('' if count == len(keys) else ' in %d of %d' % (count, len(keys)),
                                                 ', '.join(members)))
            w('\t{ 0x%X, %s, %d },%s' % (off, C_KINDS[kind], child,
                                       ' /* %s */' % '; '.join(notes) if notes else ''))
            first += 1
        first = 0
    w('};')
    w('')
    w('/* sorted by group tag */')
    w('static const struct xbox_map_group_layout xbox_map_group_layouts[NUMBER_OF_XBOX_MAP_GROUP_LAYOUTS] =')
    w('{')
    for value, g, n in groups:
        w('\t{ 0x%08XUL, %d }, /* %s */' % (value, n, g))
    w('};')
    w('')
    w('#endif')
    return '\n'.join(out) + '\n'


def write_text(path, text):
    """write text keeping the line endings of the file it replaces (CRLF for a
    new file, as the sources on this checkout)"""
    newline = '\r\n'
    if os.path.exists(path):
        with open(path, 'rb') as f:
            old = f.read()
        newline = '\r\n' if b'\r\n' in old else '\n'
    data = text.replace('\n', newline).encode('ascii')
    if os.path.exists(path):
        with open(path, 'rb') as f:
            if f.read() == data:
                return False
    with open(path, 'wb') as f:
        f.write(data)
    return True


# ---------- the game's own structures

def source_files(suffixes):
    for directory, _, files in os.walk(os.path.join(ROOT, 'source')):
        for f in sorted(files):
            if f.endswith(suffixes):
                yield os.path.join(directory, f)


def read_source(path):
    with open(path, encoding='latin-1') as f:
        return f.read().replace('\r\n', '\n')


def tag_headers():
    """the headers that define tag structures and the structures they define"""
    headers, names = [], set()
    for path in source_files(('.h',)):
        text = read_source(path)
        if re.search(r'struct tag_(block|reference|data)\s+\w+\s*;', text):
            headers.append(os.path.relpath(path, os.path.join(ROOT, 'source')).replace(os.sep, '/'))
            names.update(re.findall(r'^struct\s+(\w+)\s*\n\{', text, re.M))
    return sorted(headers), sorted(names)


def record_layout_command(clang, nxdk):
    """clang reading a unit as the Xbox build compiles the game (i386, MSVC
    layout); the flag that dumps the record layouts and the unit follow"""
    includes = ['-I' + os.path.join(ROOT, 'source', d) for d in sorted(os.listdir(os.path.join(ROOT, 'source')))
                if os.path.isdir(os.path.join(ROOT, 'source', d))]
    return [clang, '--target=i386-pc-win32', '-march=pentium3', '-ffreestanding', '-fms-extensions',
            '-std=gnu89', '-w', '-Wno-builtin-macro-redefined', '-D__STDC__=1', '-U__STDC_NO_THREADS__',
            '-DNXDK', '-include', os.path.join(ROOT, 'port/xbox/include/halo_xbox_prefix.h'),
            '-include', os.path.join(ROOT, 'build/xbox/halo_msvc_tags.h'), '-DDEBUG', '-Dxbox',
            '-I' + os.path.join(ROOT, 'port/xbox/include/crt'), '-I' + os.path.join(ROOT, 'port/xbox/include'),
            '-I' + os.path.join(ROOT, 'source')] + includes + [
            '-I' + os.path.join(ROOT, 'port/include/xdk'),
            '-I' + os.path.join(nxdk, 'lib/pdclib/platform/xbox/include'),
            '-I' + os.path.join(nxdk, 'lib/xboxrt/libc_extensions'),
            '-isystem', os.path.join(nxdk, 'lib/pdclib/include'),
            '-fsyntax-only', '-Xclang']


def parse_record_layouts(text):
    """clang's record layout dump: struct name -> (size, [(offset, depth,
    type, name)])"""
    records = {}
    current = None
    for line in text.splitlines():
        m = re.match(r'\s*(\d+) \|( *)(.*?)\s*$', line)
        if m:
            depth = (len(m.group(2)) - 1) // 2
            text = m.group(3)
            if depth == 0:
                rm = re.match(r'(struct|union) (\w+)$', text)
                current = None
                if rm:
                    current = records.setdefault(rm.group(2), [None, []])
                    if current[1]:
                        current = None      # dumped before
                continue
            if current is not None:
                fm = re.match(r'(.*?) ?(\w*)$', text)
                current[1].append((int(m.group(1)), depth, fm.group(1).strip(), fm.group(2)))
            continue
        m = re.match(r'\s*\| \[sizeof=(\d+)', line)
        if m and current is not None and current[0] is None:
            current[0] = int(m.group(1))
    return {k: (v[0], v[1]) for k, v in records.items() if v[0] is not None}


def record_layouts(clang, nxdk, scratch, headers=None, names=None):
    """struct name -> (size, [(offset, depth, type, name)]) from clang's
    record layout dump of the structures of some headers (every tag
    structure of the headers, unless given), compiled as the Xbox build
    compiles the game (i386, MSVC layout)"""
    if headers is None:
        headers, names = tag_headers()
    unit = os.path.join(scratch, 'xbox_map_layouts_structs.c')
    with open(unit, 'w') as f:
        f.write('#include "cseries.h"\n')
        for h in headers:
            f.write('#include "%s"\n' % h)
        for n in names:
            f.write('typedef char xbox_map_layouts_probe_%s[sizeof(struct %s)];\n' % (n, n))
    result = subprocess.run(record_layout_command(clang, nxdk) + ['-fdump-record-layouts', unit],
                            cwd=ROOT, capture_output=True, text=True)
    os.remove(unit)
    if result.returncode:
        raise MapError('the tag structures do not compile:\n' + result.stderr[-3000:])
    return parse_record_layouts(result.stdout)


TAG_STRUCT_KINDS = {'struct tag_block': 'block', 'struct tag_reference': 'reference', 'struct tag_data': 'data'}


def record_fields(records, name, base=0, depth_limit=8):
    """the fields of a structure at any depth, arrays expanded:
    [(offset, kind, field name, structure declaring it)], kind 'block',
    'reference' or 'data' for the tag structures, else the C type"""
    if depth_limit == 0 or name not in records:
        return []
    size, lines = records[name]
    out, stack, skip = [], [], None
    for k, (offset, depth, typ, field) in enumerate(lines):
        if skip is not None and depth > skip:
            continue
        skip = None
        while stack and stack[-1][0] >= depth:
            stack.pop()
        owner = stack[-1][1] if stack else name
        am = re.match(r'(struct|union) (\w+)\[(\d+)\]$', typ)
        if typ in TAG_STRUCT_KINDS:
            out.append((base + offset, TAG_STRUCT_KINDS[typ], field, owner))
            skip = depth
        elif am and 'struct ' + am.group(2) in TAG_STRUCT_KINDS:
            step = {'tag_block': 12, 'tag_reference': 16, 'tag_data': 20}[am.group(2)]
            for i in range(int(am.group(3))):
                out.append((base + offset + i * step, TAG_STRUCT_KINDS['struct ' + am.group(2)], field, owner))
            skip = depth
        elif am and am.group(2) in records:
            element = records[am.group(2)][0]
            for i in range(int(am.group(3))):
                out += record_fields(records, am.group(2), base + offset + i * element, depth_limit - 1)
            skip = depth
        else:
            rm = re.match(r'(?:struct|union) (\w+)$', typ)
            leaf = k + 1 == len(lines) or lines[k + 1][1] <= depth
            if leaf:
                out.append((base + offset, typ, field, owner))
            stack.append((depth, rm.group(1) if rm else owner))
    return out


def element_types():
    """what the source says about the elements of tag blocks: (structure,
    field) -> types from the comments of the field, field -> types from
    TAG_BLOCK_GET_ELEMENT uses, by (owner structure or None, field)"""
    by_comment, by_use = collections.defaultdict(set), collections.defaultdict(collections.Counter)
    for path in source_files(('.h', '.c')):
        text = read_source(path)
        for sm in re.finditer(r'^struct\s+(\w+)\s*\n\{(.*?)\n\};', text, re.M | re.S):
            for fm in re.finditer(r'struct tag_block\s+(\w+)\s*;[ \t]*(?://(.*)|/\*(.*?)\*/)', sm.group(2)):
                comment = fm.group(2) or fm.group(3) or ''
                for word in re.findall(r'\w+', comment):
                    by_comment[(sm.group(1), fm.group(1))].add(word)
        for um in re.finditer(r'TAG_BLOCK_GET_ELEMENT\s*\(', text):
            args, depth, start, i = [], 1, um.end(), um.end()
            while i < len(text) and depth:
                c = text[i]
                if c in '([':
                    depth += 1
                elif c in ')]':
                    depth -= 1
                if (c == ',' and depth == 1) or depth == 0:
                    args.append(text[start:i].strip())
                    start = i + 1
                i += 1
            if len(args) != 3:
                continue
            fm = re.search(r'(?:(\w+)\s*(?:->|\.))?\s*(\w+)\s*\)*\s*$', args[0])
            tm = re.match(r'struct\s+(\w+)$', args[2])
            if fm and tm:
                # the structure of the block's owner, where the variable is
                # declared before the use
                owner = None
                if fm.group(1):
                    dm = None
                    for dm in re.finditer(r'struct\s+(\w+)\s*\*\s*(?:const\s+)?%s\b' % fm.group(1), text[:um.start()]):
                        pass
                    owner = dm.group(1) if dm else None
                by_use[(owner, fm.group(2))][tm.group(1)] += 1
    return by_comment, by_use


def group_structures(records):
    """group tag -> the structure the source reads a tag of that group as
    (the largest one, where a group is also read as its parent)"""
    constants = collections.defaultdict(set)
    for path in source_files(('.h', '.c')):
        text = read_source(path)
        for m in re.finditer(r"(\w+_TAG)\s*=\s*'([^']{4})'", text):
            constants[m.group(1)].add(m.group(2))
        for m in re.finditer(r"#define\s+(\w+_TAG)\s+'([^']{4})'", text):
            constants[m.group(1)].add(m.group(2))
    # a constant the source gives two values (POINT_PHYSICS_DEFINITION_TAG
    # is 'phys' in one header and 'pphy' in another) names no group
    constants = {k: v.pop() for k, v in constants.items() if len(v) == 1}
    votes = collections.defaultdict(set)
    for path in source_files(('.h', '.c')):
        text = read_source(path)
        for m in re.finditer(r"\(\s*struct\s+(\w+)\s*\*\s*\)\s*tag_get\s*\(\s*(\w+|'[^']{4}')", text):
            g = m.group(2)
            g = g[1:-1] if g.startswith("'") else constants.get(g)
            if g and m.group(1) in records:
                votes[g].add(m.group(1))
    return {g: max(names, key=lambda n: (records[n][0], n)) for g, names in votes.items()}


def cross_check(layouts, records):
    """the learned layouts against the game's structures: per layout the
    structure the source gives for it (or why none), the two sizes and each
    pointer field: 'match' (the structure has that tag structure there),
    'opaque' (the structure has only untyped bytes there) or 'conflict'"""
    by_comment, by_use = element_types()
    groups = group_structures(records)
    declared = collections.Counter()
    for name in records:
        for off, kind, field, owner in record_fields(records, name):
            if kind == 'block' and owner == name:
                declared[field] += 1
    rows = []

    def element_structure(c, parent):
        """the structure of a tag_block field's elements, or (None, why)"""
        kind, field, owner = c
        names = set(n for n in by_comment.get((owner, field), ()) if n in records)
        if len(names) == 1:
            return names.pop(), 'comment on %s.%s' % (owner, field)
        typed = sorted(n for o in (owner, parent) for n in by_use.get((o, field), {}) if n in records)
        if len(set(typed)) == 1:
            return typed[0], 'TAG_BLOCK_GET_ELEMENT(&%s->%s, ..., struct %s)' % (owner, field, typed[0])
        uses = sorted(set(n for (o, f), c in by_use.items() if f == field and o is None for n in c if n in records))
        if len(uses) == 1 and declared[field] <= 1 and not typed:
            return uses[0], 'TAG_BLOCK_GET_ELEMENT(&...->%s, ..., struct %s)' % (field, uses[0])
        if not uses:
            # the naming convention: scenario.bipeds holds struct scenario_biped
            singulars = [field[:-3] + 'y'] if field.endswith('ies') else []
            singulars += [field[:-1]] if field.endswith('s') else [field]
            prefixes = [owner, owner.lstrip('_')]
            for suffix in ('_definition', '_group', '_data'):
                if owner.endswith(suffix):
                    prefixes.append(owner[:-len(suffix)].lstrip('_'))
            named = sorted(set(p + '_' + n for p in prefixes for n in singulars if p + '_' + n in records))
            if len(named) == 1:
                return named[0], 'named after %s.%s' % (owner, field)
        if uses:
            return None, '%s.%s: the field name is used for %s' % (
                owner, field, ', '.join(uses) if declared[field] <= 1 else 'several blocks')
        return None, 'nothing names the elements of %s.%s' % (owner, field)

    def visit(key, struct, how):
        L = layouts[key]
        row = {'key': key, 'size': L.size, 'struct': struct, 'how': how, 'c_size': None, 'size_result': None,
               'fields': []}
        rows.append(row)
        cfields = []
        if struct is not None:
            row['c_size'] = records[struct][0]
            if struct in ('tag_block', 'tag_reference', 'tag_data'):
                cfields = [(0, TAG_STRUCT_KINDS['struct ' + struct], struct, struct)]
            else:
                cfields = record_fields(records, struct)
            row['size_result'] = ('match' if row['c_size'] == L.size else
                                  'smaller' if row['c_size'] < L.size else 'larger')
        at = {}
        for c in cfields:
            at.setdefault(c[0], c)
        starts = sorted(at)
        for off in sorted(L.fields):
            if off in L.declared:
                continue        # (check_declared's)
            kind = L.fields[off]
            soff = off - POINTER_WORD[kind]
            child_struct, child_how = None, 'no structure for the parent'
            if struct is not None:
                c = at.get(soff)
                if kind in TAG_STRUCT_KINDS.values():
                    if c and c[1] == kind:
                        result = 'match'
                    else:
                        k = bisect.bisect_right(starts, soff) - 1
                        cover = at[starts[k]] if k >= 0 else None
                        # past the structure's end, or inside an array or
                        # a field the source leaves unnamed
                        opaque = soff >= row['c_size'] or (cover is not None and (
                            '[' in cover[1] or cover[2].startswith(('unknown', 'pad', 'reserved', 'unused'))))
                        result = 'opaque' if opaque else 'conflict'
                        if c and c[0] == soff and c[1] in TAG_STRUCT_KINDS.values():
                            result = 'conflict'
                    row['fields'].append((soff, kind, result, c))
                if kind == 'block':
                    if c and c[1] == 'block':
                        child_struct, child_how = element_structure((c[1], c[2], c[3]), struct)
                    else:
                        child_how = 'no tag_block at %#x of %s' % (soff, struct)
            if kind == 'block':
                visit(L.children[off], child_struct, child_how)

    for key in sorted(k for k in layouts if len(k) == 1):
        g = key[0]
        visit(key, groups.get(g), 'tag_get(%s) cast' % g if g in groups else 'no tag_get of the group is cast')
    return rows


def cross_check_summary(rows):
    s = collections.Counter()
    for r in rows:
        if r['struct'] is None:
            s['layouts without a structure'] += 1
            continue
        s['layouts with a structure'] += 1
        s['size ' + r['size_result']] += 1
        for f in r['fields']:
            s['pointer fields ' + f[2]] += 1
    return s


def declared_structures():
    """the structures DECLARED_FIELDS names"""
    names = set()

    def elements(e):
        if e is None:
            return
        names.add(e[1] if not isinstance(e[1], int) else e[0])
        if isinstance(e[1], int):
            for field in e[2]:
                if field[1] == 'block':
                    elements(field[3])

    for entry in DECLARED_FIELDS:
        names.add(entry[3])
        if entry[2] == 'block':
            elements(entry[6])
    return names - set(SCALAR_ELEMENTS)


def unit_record_layouts(clang, nxdk, scratch, names, records):
    """the record layouts of the .c files that define one of the named
    structures: {file under source/: records}. The game reads several tag
    groups through structures a .c file defines for itself, and some
    structures are defined by more than one file (a variant in each). The
    named structures of headers that records (the tag headers') lacks come
    as one more unit."""
    units = {}
    headers, others = [], set()
    for path in source_files(('.h',)):
        defined = set(re.findall(r'^struct\s+(\w+)\s*\n\{', read_source(path), re.M)) & names - set(records)
        if defined:
            headers.append(os.path.relpath(path, os.path.join(ROOT, 'source')).replace(os.sep, '/'))
            others |= defined
    if headers:
        units['other headers'] = record_layouts(clang, nxdk, scratch, headers, sorted(others))
    command = record_layout_command(clang, nxdk) + ['-fdump-record-layouts-complete']
    for path in source_files(('.c',)):
        defined = set(re.findall(r'^struct\s+(\w+)\s*\n\{', read_source(path), re.M))
        if not defined & names:
            continue
        # (a unit whose code clang rejects has its structures dumped all the same)
        result = subprocess.run(command + [path], cwd=ROOT, capture_output=True, text=True, errors='replace')
        dumped = parse_record_layouts(result.stdout)
        units[os.path.relpath(path, os.path.join(ROOT, 'source')).replace(os.sep, '/')] = {
            n: dumped[n] for n in dumped if n in defined or n not in names}
    return units


def check_declared(layouts, records, units):
    """DECLARED_FIELDS against clang's record layouts: the structure of
    each layout has the member at the offset, as that kind of tag
    structure, and the elements of each block are what the table says;
    returns the entries that fail"""
    def variants(name):
        return ([records] if name in records else []) + [units[u] for u in sorted(units) if name in units[u]]

    def tag_fields(recs, name):
        if name in ('tag_block', 'tag_reference', 'tag_data'):
            return [(0, TAG_STRUCT_KINDS['struct ' + name], name, name)]
        return [f for f in record_fields(recs, name) if f[1] in TAG_STRUCT_KINDS.values()]

    def elements_problem(e):
        if e is None:
            return None
        if not isinstance(e[1], int):
            path, name = e
            L = layouts.get(path_key(path))
            if L is None:
                return 'no layout %s' % path
            have = {off - POINTER_WORD[kind]: kind for off, kind in L.fields.items()}
            for recs in variants(name):
                if recs[name][0] == L.size and all(have.get(f[0]) == f[1] for f in tag_fields(recs, name)):
                    return None
            return 'no struct %s is %#x bytes with its tag fields in the layout %s' % (name, L.size, path)
        name, size, fields = e
        if name in SCALAR_ELEMENTS:
            return None if SCALAR_ELEMENTS[name] == size and not fields else 'a %s is %d bytes' % (
                name, SCALAR_ELEMENTS[name])
        want = sorted(f[:3] for f in fields)
        if not any(recs[name][0] == size and sorted((f[0], f[1], f[2]) for f in tag_fields(recs, name)) == want
                   for recs in variants(name)):
            return 'no struct %s is %#x bytes with exactly those tag fields' % (name, size)
        for field in fields:
            if field[1] == 'block':
                problem = elements_problem(field[3])
                if problem:
                    return problem
        return None

    problems = []
    for entry in DECLARED_FIELDS:
        path, offset, kind, structure, member, where = entry[:6]
        owner, field = member.rsplit('.', 1)
        name = '%s+%#x (%s)' % (path, offset, member)
        if not any((offset, kind, field, owner) in record_fields(recs, structure) for recs in variants(structure)):
            problems.append('declared field %s: no struct %s has that tag %s at %#x' % (name, structure, kind, offset))
        elif kind == 'block':
            problem = elements_problem(entry[6])
            if problem:
                problems.append('declared field %s: %s' % (name, problem))
    return problems


# ---------- proving the C walker

def find_clang(given):
    for c in ([given] if given else []) + ['clang', 'C:/msys64/mingw64/bin/clang.exe', '/mingw64/bin/clang']:
        path = shutil.which(c) or (c if os.path.isfile(c) else None)
        if path:
            return path
    raise MapError('no clang found (pass --clang)')


def build_harness(clang, walker, header_dir, scratch, log):
    """the harness and the walker, built for the host (unsigned long must be
    32 bits: an i686 or LLP64 target); the walker also alone as C89 and for
    the Windows 9x build's target"""
    exe = os.path.join(scratch, 'xbox_map_rebase_test.exe')
    includes = ['-I' + header_dir, '-I' + os.path.join(ROOT, 'port', 'linux', 'src')]
    steps = [
        ('the walker as C89', [clang, '-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror', '-fsyntax-only'] +
         includes + [walker]),
        ('the walker for i686 (pentium3)', [clang, '--target=i686-w64-windows-gnu', '-march=pentium3', '-O2',
                                            '-std=c89', '-Wall', '-Wextra', '-Werror', '-c'] + includes +
         [walker, '-o', os.path.join(scratch, 'xbox_map_rebase_i686.o')]),
        ('the harness', [clang, '-O2', '-Wall', '-Wextra', '-Werror'] + includes +
         [HARNESS, walker, '-o', exe]),
    ]
    for what, command in steps:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise MapError('%s does not build:\n%s\n%s' % (what, ' '.join(command), result.stdout + result.stderr))
        log('  built %s' % what)
    return exe


def write_words(path, values):
    with open(path, 'wb') as f:
        f.write(array('I', values).tobytes() if sys.byteorder == 'little' else
                b''.join(struct.pack('<I', v) for v in values))


def check_map(path, result, exe, scratch, log):
    """dump the tag data and the BSPs of one map, run the walker on them
    with two deltas, with the declared fields filled and with damaged data;
    returns per region (moved, milliseconds) for the first delta, the
    harness output, the declared fields filled and the pointers written
    for that"""
    m = map_name(path)
    S = load(path)
    account = result['accounts'][m]
    rows = {row[0]: row for row in account_map(account)}
    folder = tempfile.mkdtemp(prefix='rebase_%s_' % m, dir=scratch)
    jobs = []
    try:
        for reg in S['regions']:
            r = account['regions'][reg.name]
            row = rows[reg.name]
            if row[5] or row[6]:
                raise MapError('%s %s: the Python walker differs from the oracle' % (m, reg.name))
            data = os.path.join(folder, reg.name + '.bin')
            expected = os.path.join(folder, reg.name + '.words')
            with open(data, 'wb') as f:
                f.write(reg.data)
            write_words(expected, sorted(a - reg.base for a in r['moved']))
            kind = 'tags' if reg.kind == 'tags' else 'bsp'
            for delta in (-0x40000000, 0x10000000):
                jobs.append('%s %s %x %d %s %s:%s:%+#x\n' % (kind, data, reg.base, delta, expected, m, reg.name, delta))
        if result.get('damage') == m:
            jobs += damage_jobs(S, account, result['table'], folder)
        copies, filled, written = filled_jobs(S, account, result['table'], folder, m, result.get('damage') == m)
        jobs += copies
        job_file = os.path.join(folder, 'jobs.txt')
        with open(job_file, 'w') as f:
            f.writelines(jobs)
        run = subprocess.run([exe, job_file], capture_output=True, text=True)
        lines = run.stdout.splitlines()
        if run.returncode or len(lines) != len(jobs):
            raise MapError('%s: the harness failed:\n%s%s' % (m, run.stdout, run.stderr))
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    timings = {}
    for line in lines:
        mm = re.match(r'(\S+?):(\S+?):(\S+) ok (\d+) words moved, ([\d.]+) ms', line)
        if mm and mm.group(3) == '-0x40000000':
            timings[mm.group(2)] = (int(mm.group(4)), float(mm.group(5)))
    return timings, lines, filled, written


def damage_jobs(S, account, table, folder):
    """damaged copies the walker must refuse: no signature, a tag block, a
    tag reference and a tag data pointing outside, a group with no layout,
    a BSP whose definition is outside"""
    tags = S['tags']
    jobs = []
    data = os.path.join(folder, 'tags.bin')
    expected = os.path.join(folder, 'tags.words')
    kinds = {}
    for key, (size, fields) in table.layouts.items():
        for off, kind, child in fields:
            kinds[(key, off)] = kind
    by_kind = {}
    for k, addresses in sorted(account['regions']['tags']['by_field'].items()):
        if addresses:
            by_kind.setdefault(kinds[k], min(addresses))
    outside = tags.end + 0x1000
    pokes = [('signature', 0x20, 0)]
    for kind in ('block', 'reference', 'data'):
        if kind in by_kind:
            pokes.append((kind, by_kind[kind] - tags.base, outside))
    instances = tags.word(tags.base)
    for i in range(tags.word(tags.base + 0xC)):
        if tags.word(instances + i * TAG_INSTANCE_SIZE + 0x14):
            pokes.append(('group', instances + i * TAG_INSTANCE_SIZE - tags.base, fourcc_value('zzzz')))
            break
    for what, offset, value in pokes:
        jobs.append('tags %s %x %d %s damaged:tags:%s %x %x\n' % (data, tags.base, -0x40000000, expected, what, offset, value))
    bsp = S['bsps'][0]['region']
    jobs.append('bsp %s %x %d %s damaged:%s:definition %x %x\n' % (
        os.path.join(folder, bsp.name + '.bin'), bsp.base, -0x40000000,
        os.path.join(folder, bsp.name + '.words'), bsp.name, 0, bsp.end + 0x1000))
    return jobs


class Filled:
    """a copy of a region in which declared fields hold pointers, as a map
    that uses them would: a tag reference names an existing tag, a tag data
    points at bytes appended to the region, a tag block at one new element
    appended to it, whose own pointer fields are filled the same way.
    Appended raw bytes, and the elements of a block the layouts know
    nothing about, are words that look like addresses; none may move."""

    def __init__(self, region, table, tag):
        self.r, self.table, self.tag = region, table, tag
        self.data = bytearray(region.data) + bytes((-region.size) % 4)
        self.moved = set()                      # the pointer words written
        self.filled = collections.Counter()     # the declared fields among them: (key, offset) -> times
        self.examples = {}                      # what -> a declared field's pointer word, for the damaged copies

    def put(self, address, *words):
        struct.pack_into('<%dI' % len(words), self.data, address - self.r.base, *words)

    def append(self, size, word=0):
        address = self.r.base + len(self.data)
        self.data += struct.pack('<I', word) * ((size + 3) // 4)
        return address

    def fill(self, key, off, a):
        """fill the field of the layout whose pointer word is at a"""
        kind, child = self.table.field[(key, off)]
        if kind not in TAG_STRUCT_KINDS.values() or a in self.moved:
            return
        start = a - POINTER_WORD[kind]
        what = kind
        if kind == 'reference':
            self.put(start, self.tag[0], self.tag[1], 0, self.tag[2])
        elif kind == 'data':
            self.put(start, 16)
            self.put(a, self.append(16, self.r.base))
        elif child in self.table.unknown:
            what = 'block of unknown elements'
            self.put(start, 20, self.append(20, self.r.base), 0)
        else:
            element = self.append(self.table.layouts[child][0])
            self.put(start, 1, element, 0)
            for field_off, field_kind, field_child in self.table.layouts[child][1]:
                self.fill(child, field_off, element + field_off)
        self.moved.add(a)
        if (key, off) in self.table.declared:
            self.filled[(key, off)] += 1
            self.examples.setdefault(what, a)


def filled_jobs(S, account, table, folder, m, damage):
    """the declared fields in use: per region a copy with the first element
    of every declared field the walk reached filled (Filled), which must
    move exactly the words the region moves and the pointer words written.
    The Python walker is checked here, the C walker by the jobs returned;
    with damage, copies of the tag data and of the first BSP whose filled
    fields point outside, to be refused. Returns the jobs, the declared
    fields filled, (key, offset) -> times, and the pointers written."""
    tag = next(t for t in S['instances'] if t['base'] and t['name'] is not None)
    jobs, filled, written = [], collections.Counter(), 0
    for reg in S['regions']:
        r = account['regions'][reg.name]
        if not r['declared']:
            continue
        f = Filled(reg, table, (tag['group'], tag['name_ptr'], tag['index']))
        for a, k in sorted((a, k) for k, a in r['declared'].items()):
            f.fill(k[0], k[1], a)
        expected = r['moved'] | f.moved
        copy = Region(reg.name, reg.base, bytes(f.data), reg.kind)
        w = walk_tag_data(table, copy) if reg.kind == 'tags' else walk_structure_bsp(table, copy)
        if w.errors or w.moved != expected:
            raise MapError('%s %s with the declared fields filled: the Python walker %s' % (
                m, reg.name, w.errors[0] if w.errors else 'moves %d words more and %d fewer than expected' % (
                    len(w.moved - expected), len(expected - w.moved))))
        data = os.path.join(folder, reg.name + '.filled.bin')
        words = os.path.join(folder, reg.name + '.filled.words')
        with open(data, 'wb') as file:
            file.write(f.data)
        write_words(words, sorted(a - reg.base for a in expected))
        kind = 'tags' if reg.kind == 'tags' else 'bsp'
        for delta in (-0x40000000, 0x10000000):
            jobs.append('%s %s %x %d %s filled:%s:%s:%+#x\n' % (kind, data, reg.base, delta, words, m, reg.name, delta))
        filled.update(f.filled)
        written += len(f.moved)
        if damage and reg in (S['tags'], S['bsps'][0]['region']):
            outside = reg.base + len(f.data) + 0x1000
            pokes = [('declared-%s' % what.replace(' ', '-'), a, outside) for what, a in sorted(f.examples.items())]
            if 'block of unknown elements' in f.examples:
                pokes.append(('declared-block-of-unknown-elements-count', f.examples['block of unknown elements'] - 4,
                              len(f.data) + 1))
            for what, a, value in pokes:
                jobs.append('%s %s %x %d %s damaged:%s:%s %x %x\n' % (
                    kind, data, reg.base, -0x40000000, words, reg.name, what, a - reg.base, value))
    return jobs, filled, written


# ---------- the report

def report(result, rows, log):
    accounts = result['accounts']
    log('')
    log('per map: words the walker moves against the oracle (orphans: pointer shapes in unreferenced')
    log('holes; look-alikes: oracle shapes in fields that hold non-pointers elsewhere)')
    log('  %-15s %-6s %7s %7s %7s %7s %7s %7s' % ('map', 'region', 'oracle', 'orphan', 'alike', 'walker', 'missing', 'extra'))
    totals = collections.Counter()
    for m in sorted(accounts):
        per = collections.Counter()
        for name, o, orph, look, walker, missing, extra in account_map(accounts[m]):
            region = 'tags' if name == 'tags' else 'bsps'
            per[(region, 'oracle')] += o
            per[(region, 'orphans')] += orph
            per[(region, 'lookalikes')] += look
            per[(region, 'walker')] += walker
            per[(region, 'missing')] += len(missing)
            per[(region, 'extra')] += len(extra)
            per[(region, 'count')] += 1
        for region in ('tags', 'bsps'):
            log('  %-15s %-6s %7d %7d %7d %7d %7d %7d' % (
                m, region if region == 'tags' else '%d bsp' % per[(region, 'count')], per[(region, 'oracle')],
                per[(region, 'orphans')], per[(region, 'lookalikes')], per[(region, 'walker')],
                per[(region, 'missing')], per[(region, 'extra')]))
        for k, v in per.items():
            totals[k[1]] += v
    log('  %-22s %7d %7d %7d %7d %7d %7d' % ('all maps', totals['oracle'], totals['orphans'], totals['lookalikes'],
                                              totals['walker'], totals['missing'], totals['extra']))
    layouts = result['layouts']
    learned = [L for L in layouts.values() if not L.origin]
    table, roots = compact(layouts)
    evidence = collections.Counter()
    for L in learned:
        evidence['exact' if L.exact else 'several' if any(n['count'] >= 2 for m, n in L.nodes) else 'single'] += 1
    log('')
    log('layouts: %d learned (%d tag groups, %d block fields) and %d for the elements of declared blocks,'
        ' %d after merging identical ones, %d pointer fields' % (
            len(learned), len(roots), len(learned) - len(roots), len(layouts) - len(learned), len(table),
            sum(len(f) for s, f, p, k in table)))
    log('  element sizes: %d fit arrays of two or more elements exactly, %d come from arrays of one element'
        ' (the smallest gap), %d from longer arrays none of which fits exactly' % (
            evidence['exact'], evidence['single'], evidence['several']))
    fit = collections.Counter()
    for L in layouts.values():
        for m, n in L.nodes:
            if n['count'] >= 2:
                room = n['gap'] - n['count'] * L.size
                fit['exact' if 0 <= room <= 3 else 'room' if room > 3 else 'overlap'] += 1
    log('  arrays of two or more elements: %d end where the next allocation starts (0-3 bytes of padding),'
        ' %d have more room after them, %d overlap the next allocation' % (fit['exact'], fit['room'], fit['overlap']))
    for L in sorted(layouts.values(), key=lambda L: L.key):
        if not L.exact and any(n['count'] >= 2 for m, n in L.nodes):
            log('    %s: %#x' % (L.name(), L.size))
    stats = result['stats']
    log('  pointer fields: %d over the %d layouts; in every element of every map each holds its null form or'
        ' a valid pointer (%d valid, %d null, %d invalid)' % (
            len(stats), len(layouts), sum(s[1] for s in stats.values()), sum(s[0] for s in stats.values()),
            sum(s[2] for s in stats.values())))
    declared = result['declared']
    kinds, groups = collections.Counter(), collections.Counter()
    for key, L in layouts.items():
        for off in L.declared:
            kind = {'reference': 'tag references', 'data': 'tag data'}.get(L.fields[off])
            if kind is None:
                child = layouts[L.children[off]]
                kind = ('tag blocks of unknown elements (address only)' if child.origin == ELEMENTS_UNKNOWN else
                        'tag blocks of a structure of the source' if child.origin else
                        'tag blocks of learned elements')
            kinds[kind] += 1
            groups[key[0]] += 1
    log('  declared fields: %d that the source declares and every retail map leaves empty (%s); the %d in learned'
        ' layouts hold the null form of their kind in all %d elements of the maps' % (
            sum(kinds.values()), ', '.join('%d %s' % (kinds[k], k) for k in sorted(kinds)), len(declared),
            sum(d[0] for d in declared.values())))
    log('    by tag group: %s' % ', '.join(
        '%s %d' % (g, n) for g, n in sorted(groups.items(), key=lambda x: (-x[1], x[0]))))
    unreached = [key_name(k) for k in layouts if k not in result['reached'] and not layouts[k].origin]
    log('  layouts never walked: %s' % (', '.join(unreached) or 'none'))
    log('  conflicts: %s' % ('none' if not result['conflicts'] else len(result['conflicts'])))
    for c in result['conflicts']:
        log('    ' + c)
    log('  decisions:')
    for n in result['notes']:
        log('    ' + n)
    for name, off, invalid, valid, example in result['lookalike_fields']:
        log('    %s, pointer word +%#x: no pointer field (%d values are no pointer, e.g. %s; %d look like one)' % (
            name, off, invalid, example, valid))
    for m in sorted(accounts):
        for o in accounts[m]['orphans']:
            log('    %s %s: %s %08x at %08x lies in no array, in the hole %08x-%08x nothing points into' % (
                m, o[1], o[3], o[4], o[2], o[5], o[6]))
        for name, a, key, off in sorted(accounts[m]['excluded']):
            log('    %s %s: the oracle\'s pointer at %08x is a look-alike (%s, pointer word +%#x)' % (m, name, a, key, off))
    for m, removed in sorted(result['removed'].items()):
        for region, start in sorted(removed):
            log('    %s %s: the array at %08x is reached only from orphans' % (m, region, start))
    if rows is not None:
        s = cross_check_summary(rows)
        log('')
        log('the game\'s structures (clang record layouts of the tag headers, i386 MSVC layout):')
        log('  %d layouts have a structure in the source (by tag_get casts, block comments,'
            ' TAG_BLOCK_GET_ELEMENT uses, names), %d have none' % (
                s['layouts with a structure'], s['layouts without a structure']))
        log('  sizes: %d match, %d of the structures are shorter (partial definitions or a parent\'s view),'
            ' %d longer' % (s['size match'], s['size smaller'], s['size larger']))
        log('  pointer fields: %d match a tag_block/tag_reference/tag_data of the structure, %d fall in bytes'
            ' the source leaves unnamed, %d conflict' % (
                s['pointer fields match'], s['pointer fields opaque'], s['pointer fields conflict']))
        for r in rows:
            if r['struct'] is None:
                continue
            odd = [f for f in r['fields'] if f[2] != 'match']
            if r['size_result'] != 'match' or any(f[2] == 'conflict' for f in odd):
                log('    %s (%#x) and struct %s (%#x, %s): %s' % (
                    key_name(r['key']), r['size'], r['struct'], r['c_size'], r['how'],
                    ', '.join('%s at %#x %s%s' % (f[1], f[0], f[2], ' (%s %s)' % (f[3][1], f[3][2]) if f[3] else '')
                              for f in odd) or 'the fields agree'))
        if result.get('declared_units') is not None:
            log('  declared fields: %d entries, each a tag block, tag reference or tag data of that name at that offset'
                ' of its structure (the headers and %d .c files that define structures of their own); the elements'
                ' of %d blocks are structures of the stated size and fields' % (
                    len(DECLARED_FIELDS), result['declared_units'],
                    sum(1 for e in DECLARED_FIELDS if e[2] == 'block' and e[6] is not None)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--maps', default=DEFAULT_MAPS, help='the retail maps (default build/xbox/disc/maps)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='the header to write')
    ap.add_argument('--check', action='store_true', help='prove the C walker on every map')
    ap.add_argument('--walker', default=DEFAULT_WALKER, help='the walker source for --check')
    ap.add_argument('--clang', help='clang for --check and the structure cross-check')
    ap.add_argument('--nxdk', default=os.environ.get('NXDK_DIR'), help='nxdk, for the structure cross-check')
    ap.add_argument('--scratch', default=tempfile.gettempdir(), help='where --check dumps the maps (deleted after)')
    ap.add_argument('maps_list', nargs='*', metavar='map')
    args = ap.parse_args()

    def log(text):
        print(text, flush=True)

    paths = map_paths(args.maps, args.maps_list)
    every_map = not args.maps_list
    log('learning from %d maps' % len(paths))
    started = time.time()
    result = study(paths, log, every_map)
    log('  %.0f s' % (time.time() - started))
    rows = None
    clang = None
    try:
        clang = find_clang(args.clang)
    except MapError as e:
        log('  %s: no structure cross-check' % e)
    if clang and args.nxdk and os.path.isdir(args.nxdk):
        records = record_layouts(clang, args.nxdk, args.scratch)
        rows = cross_check(result['layouts'], records)
        units = unit_record_layouts(clang, args.nxdk, args.scratch, declared_structures(), records)
        result['declared_units'] = sum(1 for u in units if u.endswith('.c'))
        (result['conflicts'] if every_map else result['notes']).extend(
            check_declared(result['layouts'], records, units))
    else:
        log('  no NXDK_DIR (--nxdk): no structure cross-check')
    report(result, rows, log)
    if result['conflicts']:
        log('conflicts: no header written')
        return 1
    text = header_text(result['layouts'], len(paths))
    if not args.check:
        if not every_map:
            log('learned from some maps only: %s not written' % args.out)
            return 0
        log('%s %s' % (args.out, 'written' if write_text(args.out, text) else 'unchanged'))
        return 0

    # --check
    if every_map:
        with open(args.out, 'rb') as f:
            if f.read().replace(b'\r\n', b'\n').decode('ascii') != text:
                log('%s is not what the maps give now: regenerate it first' % args.out)
                return 1
        log('%s is up to date' % args.out)
    exe = build_harness(clang, os.path.abspath(args.walker), os.path.dirname(os.path.abspath(args.out)),
                        args.scratch, log)
    result['damage'] = map_name(paths[0])
    failures = 0
    log('')
    log('the C walker (%s) on every map, delta -0x40000000 (and +0x10000000):' % args.walker)
    log('  %-15s %8s %9s %8s %9s %9s' % ('map', 'tag data', 'ms', 'BSPs', 'BSP words', 'ms'))
    totals = collections.Counter()
    filled = collections.Counter()
    for path in paths:
        try:
            timings, lines, fields, written = check_map(path, result, exe, args.scratch, log)
        except MapError as e:
            log('  ' + str(e))
            failures += 1
            continue
        tags = timings.get('tags', (0, 0.0))
        bsps = [v for k, v in timings.items() if k != 'tags']
        log('  %-15s %8d %9.2f %8d %9d %9.2f' % (map_name(path), tags[0], tags[1], len(bsps),
                                                 sum(b[0] for b in bsps), sum(b[1] for b in bsps)))
        totals['tag words'] += tags[0]
        totals['bsp words'] += sum(b[0] for b in bsps)
        totals['tag ms'] += tags[1]
        totals['bsp ms'] += sum(b[1] for b in bsps)
        filled.update(fields)
        totals['written'] += written
        for line in lines:
            if line.startswith('damaged:'):
                log('    ' + line)
            mm = re.match(r'filled:\S+:-0x40000000 ok (\d+) words moved', line)
            if mm:
                totals['copies'] += 1
                totals['copy words'] += int(mm.group(1))
    log('  %-15s %8d %9.2f %8s %9d %9.2f' % ('all maps', totals['tag words'], totals['tag ms'], '',
                                            totals['bsp words'], totals['bsp ms']))
    table = result['table']
    kinds = collections.Counter()
    for (key, off), times in filled.items():
        kind, child = table.field[(key, off)]
        if kind == 'block':
            kind = 'block of unknown elements' if child in table.unknown else 'block of known elements'
        kinds[kind] += 1
        totals['times'] += times
    never = sorted('%s+%#x' % (key_name(k), off - POINTER_WORD[table.field[(k, off)][0]])
                   for k, off in table.declared if (k, off) not in filled)
    log('')
    log('the declared fields in use: copies of the tag data and the BSPs with the first element of every declared')
    log('field filled (a reference to an existing tag, a block of one appended element with its own fields filled,')
    log('tag data at appended bytes); each must move the words of the map and the pointers written, and no others')
    log('  %d copies pass (each with both deltas): %d words moved, %d of them pointers written' % (
        totals['copies'], totals['copy words'], totals['written']))
    log('  %d of the %d declared fields were filled (%s), %d times in all; never filled: %s' % (
        len(filled), len(table.declared), ', '.join('%d %s' % (kinds[k], k) for k in sorted(kinds)),
        totals['times'], ', '.join(never) or 'none'))
    if never and every_map:
        failures += 1
    os.remove(exe)
    for o in ('xbox_map_rebase_i686.o',):
        if os.path.exists(os.path.join(args.scratch, o)):
            os.remove(os.path.join(args.scratch, o))
    log('%d of %d maps pass' % (len(paths) - failures, len(paths)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
