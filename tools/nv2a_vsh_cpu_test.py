#!/usr/bin/env python3
"""Tests the vertex program evaluators of the light renderer.

port/linux/src/nv2a_vsh_cpu.c runs NV2A vertex programs on the processor;
this compares it with tools/nv2a_vsh_reference.py, an interpreter of the
GLSL that nv2a_vsh.c generates for the same programs. The evaluators are
driven through port/win9x/tests/renderer/t_vsh_cpu.c, built beforehand
(the commands are in that directory's notes; EXE is the result).

What is run:
  - each of the game's 67 programs on random vertices and constants, in
    random vertex formats (every element type, missing and constant
    elements, odd strides), for every set of result arrays a pass of the
    light renderer asks for;
  - random programs (every operation, operand source, swizzle, write mask
    and output; a0-relative constants; both units in one instruction);
  - every value of the integer element formats;
  - rcp, rcc and rsq alone over the whole range of floats.

What is checked:
  - the scalar and the SSE evaluator give the same bytes, exact and with
    estimates (t_vsh_cpu compares them);
  - neither changes MXCSR (t_vsh_cpu checks);
  - with exact reciprocals the results are the reference's to the bit
    (results of exp, log and lit, which come from the C library, within
    LIBRARY_TOLERANCE);
  - with estimates, rcp, rcc and rsq are within ESTIMATE_TOLERANCE of the
    exact results; the difference this makes to whole programs is reported;
  - bytes outside the arrays asked for are not written;
  - the matrices of affine outputs reproduce the reference's outputs.

usage: nv2a_vsh_cpu_test.py EXE [--work DIR] [--sets N] [--vertices N] [--fuzz N] [--seed N]
       nv2a_vsh_cpu_test.py EXE --classes
       nv2a_vsh_cpu_test.py EXE --bench [--vertices N]
       nv2a_vsh_cpu_test.py EXE --count [--vertices N]
"""

import argparse
import math
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nv2a_vsh_reference as ref  # noqa: E402

RECORD_BYTES = 56
RECORD_FILL = 0xCD

EXACT, CAPTURED, FLIP_Y, SIGNED_DEPTH = 1, 2, 4, 8
NONE = 255
O_POS, O_D0, O_D1, O_FOG, O_T0, O_T1, O_T2, O_T3 = 0, 3, 4, 5, 9, 10, 11, 12

# rcp, rcc and rsq from estimates against the exact results (2^-20,
# NV2A_VSH_CPU_ESTIMATE_TOLERANCE)
ESTIMATE_TOLERANCE = 2.0 ** -20
# exp2, log2 and pow of two C libraries (the evaluator's and Python's)
LIBRARY_TOLERANCE = 4.0e-6
# results flushed to zero by one side and not the other
TINY = 1.0e-37

MODES = ("scalar exact", "sse exact", "scalar estimates", "sse estimates")


# ---------- array sets (the passes of the light renderer)

class Layout:
    def __init__(self, name, flags=0, position=True, texcoords=((NONE, 0), (NONE, 0)), color=None, scale=None,
                 fog=False):
        self.name = name
        self.flags = flags
        self.position = position
        self.texcoords = texcoords
        # (rgb source, alpha source, alpha component)
        self.color = color
        self.scale = scale
        self.fog = fog

    def pack(self):
        color = self.color or (NONE, NONE, 0)
        return struct.pack("<11I4fI", self.flags, int(self.position), self.texcoords[0][0], self.texcoords[1][0],
                           self.texcoords[0][1], self.texcoords[1][1], int(self.color is not None), color[0],
                           color[1], color[2], int(self.scale is not None), *(self.scale or (0.0, 0.0, 0.0, 0.0)),
                           int(self.fog))


GL = FLIP_Y | SIGNED_DEPTH
LAYOUTS = [
    # depth-only and stencil passes; the colour copy of a lifted pass
    Layout("position", flags=0),
    Layout("position, captured, GL clip space", flags=CAPTURED | GL),
    Layout("colour only", position=False, color=(O_D0, O_D0, 3)),
    # HUD, text, decals, particles, lens flares, models' base map
    Layout("position T0 D0", flags=GL, texcoords=((O_T0, 2), (NONE, 0)), color=(O_D0, O_D0, 3)),
    # two texture units: chicago, detail, lightmap x base
    Layout("position T0 T1 D0", flags=CAPTURED | GL, texcoords=((O_T0, 2), (O_T1, 2)), color=(O_D0, O_D0, 3)),
    Layout("position T0 T2 (lightmap)", flags=CAPTURED, texcoords=((O_T0, 2), (O_T2, 2))),
    # fog passes: two ramps with q, the fog factor
    Layout("position T0 T1 q fog", flags=GL, texcoords=((O_T0, 4), (O_T1, 4)), fog=True),
    # models: fog factor in the primary colour's alpha, a folded constant
    Layout("position T0 D0 alpha=fog scaled", flags=CAPTURED | GL, texcoords=((O_T0, 2), (NONE, 0)),
           color=(O_D0, O_FOG, 0), scale=(0.5, 1.5, 0.75, 1.0)),
    # decals' fog bit (oD1.w), transparent fades
    Layout("position T0 D0 alpha=D1.w", flags=FLIP_Y, texcoords=((O_T0, 3), (NONE, 0)), color=(O_D0, O_D1, 3)),
    # the rest of the outputs
    Layout("position T2 T3 D1 fog", flags=CAPTURED | SIGNED_DEPTH, texcoords=((O_T2, 4), (O_T3, 3)),
           color=(O_D1, O_D1, 3), fog=True),
    Layout("T1 T3 only", position=False, texcoords=((O_T1, 4), (O_T3, 4))),
]


# ---------- programs made here

def assemble(mac=0, ilu=0, constant=0, input=0, a=None, b=None, c=None, mac_mask=0, temporary=0, ilu_mask=0,
             output_mask=0, output_is_register=1, output_address=0, output_from_ilu=0, relative=0, last=0):
    """one instruction; an operand is (mux, index, swizzle, negate)"""
    a = a or (0, 0, (0, 1, 2, 3), 0)
    b = b or (0, 0, (0, 1, 2, 3), 0)
    c = c or (0, 0, (0, 1, 2, 3), 0)
    w1 = (ilu << 25) | (mac << 21) | (constant << 13) | (input << 9) | (a[3] << 8) | \
        (a[2][0] << 6) | (a[2][1] << 4) | (a[2][2] << 2) | a[2][3]
    w2 = (a[1] << 28) | (a[0] << 26) | (b[3] << 25) | (b[2][0] << 23) | (b[2][1] << 21) | (b[2][2] << 19) | \
        (b[2][3] << 17) | (b[1] << 13) | (b[0] << 11) | (c[3] << 10) | (c[2][0] << 8) | (c[2][1] << 6) | \
        (c[2][2] << 4) | (c[2][3] << 2) | (c[1] >> 2)
    w3 = ((c[1] & 3) << 30) | (c[0] << 28) | (mac_mask << 24) | (temporary << 20) | (ilu_mask << 16) | \
        (output_mask << 12) | (output_is_register << 11) | (output_address << 3) | (output_from_ilu << 2) | \
        (relative << 1) | last
    return [0, w1, w2, w3]


XYZW = (0, 1, 2, 3)
XXXX = (0, 0, 0, 0)


def input_operand(swizzle=XYZW, negate=0):
    return (2, 0, swizzle, negate)


def program_moves(count):
    """oT0 = v0, oT1 = v1, ..."""
    words = []
    for index in range(count):
        words += assemble(mac=1, input=index, a=input_operand(), output_mask=15, output_address=O_T0 + index,
                          last=int(index == count - 1))
    return words


def program_function(ilu):
    """oT0.x = function(v0.x)"""
    return assemble(ilu=ilu, input=0, c=input_operand(XXXX), output_mask=8, output_address=O_T0, output_from_ilu=1,
                    last=1)


def random_program(rng, library):
    words = []
    count = rng.randint(1, 20)
    for index in range(count):
        def operand():
            mux = rng.choices((1, 2, 3, 0), (45, 30, 24, 1))[0]
            register = rng.choice((0, 1, 1, 2, 3, 4, 5, 8, 11, 12, 12)) if rng.random() < 0.97 else rng.randint(13, 15)
            swizzle = XYZW if rng.random() < 0.4 else tuple(rng.randint(0, 3) for _ in range(4))
            return (mux, register, swizzle, int(rng.random() < 0.25))
        mac = rng.choices(range(16), (10, 8, 8, 8, 8, 5, 5, 5, 3, 4, 4, 3, 3, 4, 1, 1))[0]
        ilus = (60, 8, 6, 6, 6, 5, 5, 4) if library else (60, 12, 10, 10, 8, 0, 0, 0)
        ilu = rng.choices(range(8), ilus)[0]
        address = rng.choice((0, 3, 4, 5, 9, 9, 10, 10, 11, 12)) if rng.random() < 0.93 else \
            rng.choice((1, 2, 6, 7, 8, 13, 40))
        mask = lambda: rng.choice((0, 15, 15, 8, 4, 2, 1, 12, 14, rng.randint(0, 15), rng.randint(0, 15)))
        words += assemble(
            mac=mac, ilu=ilu,
            constant=rng.randint(0, 191) if rng.random() < 0.97 else rng.randint(192, 255),
            input=rng.choice((0, 0, 1, 2, 3, 4, 5, 9, 15)),
            a=operand(), b=operand(), c=operand(),
            mac_mask=mask(), temporary=rng.choice((0, 1, 1, 2, 3, 4, 5, 8, 11, 12, 12, 13)),
            ilu_mask=mask(), output_mask=mask(), output_is_register=int(rng.random() < 0.92),
            output_address=address, output_from_ilu=int(rng.random() < 0.4), relative=int(rng.random() < 0.2),
            last=int(index == count - 1))
    return words


def program_inputs(words):
    """the input registers a program can read"""
    used = set()
    for base in range(0, len(words), 4):
        instruction = words[base:base + 4]
        for which in "ABC":
            if ref.operand_fields(instruction, which)[3] == 2:
                used.add(ref.field(instruction, 1, 9, 4))
        if ref.field(instruction, 3, 0, 1):
            break
    return sorted(used)


def uses_library(words):
    for base in range(0, len(words), 4):
        if ref.field(words[base:base + 4], 1, 25, 3) >= 5:
            return True
    return False


# ---------- cases

FLOAT_TYPES = (ref.FLOAT1, ref.FLOAT2, ref.FLOAT3, ref.FLOAT4, ref.FLOAT2H)
ALL_TYPES = tuple(ref.TYPE_BYTES)
NATIVE_TYPES = {0: ref.FLOAT3, 1: ref.NORMPACKED3, 2: ref.NORMPACKED3, 3: ref.NORMPACKED3, 4: ref.FLOAT2,
                5: ref.PBYTE2, 6: ref.NORMSHORT1, 7: ref.NORMPACKED3, 8: ref.NORMSHORT2, 9: ref.D3DCOLOR,
                10: ref.D3DCOLOR}


def random_float(rng):
    kind = rng.random()
    if kind < 0.55:
        return rng.uniform(-1.0, 1.0)
    if kind < 0.8:
        return rng.uniform(-100.0, 100.0)
    if kind < 0.9:
        return float(rng.randint(-4, 4))
    if kind < 0.95:
        return rng.uniform(-1.0, 1.0) * 10.0 ** rng.randint(-6, 6)
    return rng.choice((0.0, 1.0, -1.0, 0.5))


def element_bytes(rng, element_type):
    if element_type in FLOAT_TYPES:
        count = ref.TYPE_BYTES[element_type] // 4
        return struct.pack("<%df" % count, *(ref.f32(random_float(rng)) for _ in range(count)))
    if rng.random() < 0.05:
        # extremes
        return bytes(rng.choice((0x00, 0xFF, 0x80, 0x7F)) for _ in range(ref.TYPE_BYTES[element_type]))
    return bytes(rng.getrandbits(8) for _ in range(ref.TYPE_BYTES[element_type]))


class Case:
    def __init__(self, name, words, strict=True, check_affine=False):
        self.name = name
        self.words = words
        self.strict = strict
        self.check_affine = check_affine
        self.constants = [[0.0] * 4 for _ in range(ref.CONSTANT_COUNT)]
        self.viewport_scale = [1.0, 1.0, 1.0, 0.0]
        self.viewport_offset = [0.0, 0.0, 0.0, 0.0]
        self.screen_offset = 0.0
        self.vertex_count = 0
        self.first = 0
        # per register: (kind, type, offset, value); kind 0 value, 1 stream, 2 the first vertex for all
        self.inputs = [(0, ref.FLOAT4, 0, [0.0, 0.0, 0.0, 1.0]) for _ in range(16)]
        self.stride = 0
        self.stream = b""
        self.layouts = LAYOUTS
        self.compare_reference = True

    def pack(self):
        out = [struct.pack("<I", len(self.words) // 4), struct.pack("<%dI" % len(self.words), *self.words)]
        for constant in self.constants:
            out.append(struct.pack("<4f", *constant))
        out.append(struct.pack("<4f4ff", *self.viewport_scale, *self.viewport_offset, self.screen_offset))
        out.append(struct.pack("<II", self.vertex_count, self.first))
        for kind, element_type, offset, value in self.inputs:
            out.append(struct.pack("<III4f", kind, element_type, offset, *value))
        out.append(struct.pack("<II", self.stride, len(self.stream)))
        out.append(self.stream)
        out.append(struct.pack("<I", len(self.layouts)))
        out += [layout.pack() for layout in self.layouts]
        return b"".join(out)

    def vertex_inputs(self, vertex):
        """v0..v15 of a vertex of the run, as the shader sees them"""
        values = []
        for kind, element_type, offset, value in self.inputs:
            if kind == 0:
                values.append(list(value))
            elif kind == 2:
                values.append(ref.read_element(element_type, self.stream, offset))
            else:
                values.append(ref.read_element(element_type, self.stream,
                                               offset + (self.first + vertex) * self.stride))
        return values


def random_constants(rng, case):
    for constant in case.constants:
        kind = rng.random()
        for component in range(4):
            if kind < 0.6:
                constant[component] = ref.f32(rng.uniform(-2.0, 2.0))
            elif kind < 0.8:
                constant[component] = ref.f32(rng.uniform(-300.0, 300.0))
            elif kind < 0.9:
                constant[component] = ref.f32(rng.uniform(-1.0, 1.0) * 1.0e-3)
            else:
                constant[component] = float(rng.randint(-2, 2))
    # the viewport, as d3d8_gl.c computes it; half of the cases with the
    # program's own copy (c[-38], c[-37]) equal to the device's, as in the game
    width, height = rng.choice(((640.0, 480.0), (720.0, 480.0), (512.0, 384.0), (128.0, 128.0)))
    x, y = rng.choice(((0.0, 0.0), (16.0, 32.0)))
    zscale = rng.choice((16777215.0, 65535.0, 1.0))
    case.viewport_scale = [ref.f32(width * 0.5), ref.f32(-height * 0.5), ref.f32(zscale), 0.0]
    case.viewport_offset = [ref.f32(x + width * 0.5), ref.f32(y + height * 0.5), 0.0, 0.0]
    case.screen_offset = rng.choice((0.0, 0.0, 107.0))
    if rng.random() < 0.5:
        case.constants[ref.CONSTANT_BIAS - 38] = list(case.viewport_scale)
        case.constants[ref.CONSTANT_BIAS - 37] = list(case.viewport_offset)
    if rng.random() < 0.05:
        case.viewport_scale[2] = 0.0


def random_stream(rng, case, vertex_count, native=False):
    registers = program_inputs(case.words)
    case.vertex_count = vertex_count
    case.first = 0 if native else rng.choice((0, 0, 3, 17))
    elements = []
    order = list(registers)
    if not native:
        rng.shuffle(order)
    offset = 0
    for register in order:
        kind = 1 if native else rng.choices((1, 0, 2), (86, 9, 5))[0]
        if native:
            element_type = NATIVE_TYPES.get(register, ref.FLOAT4)
        elif rng.random() < 0.5:
            element_type = rng.choice((ref.FLOAT3, ref.FLOAT2, ref.NORMPACKED3, ref.NORMSHORT2, ref.D3DCOLOR,
                                       ref.PBYTE2, ref.FLOAT4, ref.NORMSHORT1))
        else:
            element_type = rng.choice(ALL_TYPES)
        value = [ref.f32(random_float(rng)) for _ in range(4)]
        if kind != 0:
            if not native:
                offset += rng.choice((0, 0, 0, 1, 2, 3, 4))
            elements.append((register, element_type, offset))
            case.inputs[register] = (kind, element_type, offset, value)
            offset += ref.TYPE_BYTES[element_type]
        else:
            case.inputs[register] = (0, element_type, 0, value)
    case.stride = max(offset, 1) if native else max(offset, 1) + rng.choice((0, 0, 1, 4, 7))
    total = case.first + vertex_count
    stream = bytearray(rng.getrandbits(8) for _ in range(total * case.stride))
    for vertex in range(total):
        base = vertex * case.stride
        for register, element_type, element_offset in elements:
            data = element_bytes(rng, element_type)
            stream[base + element_offset:base + element_offset + len(data)] = data
    case.stream = bytes(stream)


def exhaustive_cases():
    """every value of the integer element formats, through mov"""
    cases = []
    words = program_moves(4)
    case = Case("every short, byte and packed value", words)
    count = 65536
    stream = bytearray()
    for index in range(count):
        short = index - 32768
        x, y, z = index & 0x7FF, (index * 7 + 3) & 0x7FF, (index >> 3) & 0x3FF
        stream += struct.pack("<4h", short, -short - 1, (short * 3) % 32768, short ^ 0x55)
        stream += struct.pack("<I", x | (y << 11) | (z << 22))
        stream += bytes(((index + 64 * n) & 0xFF) for n in range(4))
        stream += struct.pack("<4h", short, (index * 5) % 65536 - 32768, 0, 0)
    case.stride = 24
    case.inputs[0] = (1, ref.NORMSHORT4, 0, [0.0] * 4)
    case.inputs[1] = (1, ref.NORMPACKED3, 8, [0.0] * 4)
    case.inputs[2] = (1, ref.D3DCOLOR, 12, [0.0] * 4)
    case.inputs[3] = (1, ref.SHORT4, 16, [0.0] * 4)
    case.stream = bytes(stream)
    case.vertex_count = count
    case.layouts = [Layout("T0 T1", position=False, texcoords=((O_T0, 4), (O_T1, 4)), flags=0),
                    Layout("T2 T3", position=False, texcoords=((O_T2, 4), (O_T3, 4)), flags=0)]
    cases.append(case)

    case = Case("every byte", words)
    case.stride = 4
    case.inputs[0] = (1, ref.PBYTE4, 0, [0.0] * 4)
    case.inputs[1] = (1, ref.PBYTE1, 1, [0.0] * 4)
    case.inputs[2] = (1, ref.NORMSHORT2, 0, [0.0] * 4)
    case.inputs[3] = (1, ref.SHORT1, 2, [0.0] * 4)
    case.stream = bytes((index + 85 * n) & 0xFF for index in range(256) for n in range(4))
    case.vertex_count = 256
    case.layouts = [Layout("T0 T1", position=False, texcoords=((O_T0, 4), (O_T1, 4)), flags=0),
                    Layout("T2 T3", position=False, texcoords=((O_T2, 4), (O_T3, 4)), flags=0)]
    cases.append(case)
    return cases


def function_cases(rng, count):
    """rcp, rcc and rsq of floats of every exponent, and the special values"""
    cases = []
    specials = [0.0, -0.0, math.inf, -math.inf, 1.0, -1.0, 3.4028234663852886e38, -3.4028234663852886e38,
                1.1754943508222875e-38, -1.1754943508222875e-38, 1.401298464324817e-45, 1.0e-39, -1.0e-40,
                1.8446744073709552e19, 5.421010862427522e-20, 1.0e38, 2.0e38, 1.0e-38, 8.5e37, 1.7e38, 3.0e38,
                2.5e-38, 4.0, 0.25]
    for name, ilu in (("rcp", 2), ("rcc", 3), ("rsq", 4)):
        case = Case("%s over the floats" % name, program_function(ilu))
        values = list(specials)
        while len(values) < count:
            exponent = rng.randint(1, 254)
            bits = (rng.getrandbits(1) << 31) | (exponent << 23) | rng.getrandbits(23)
            values.append(struct.unpack("<f", struct.pack("<I", bits))[0])
        case.stride = 4
        case.inputs[0] = (1, ref.FLOAT1, 0, [0.0] * 4)
        case.stream = struct.pack("<%df" % len(values), *values)
        case.vertex_count = len(values)
        case.layouts = [Layout("T0", position=False, texcoords=((O_T0, 2), (NONE, 0)), flags=0)]
        case.function = name
        cases.append(case)
    return cases


def write_cases(path, cases):
    with open(path, "wb") as out:
        out.write(struct.pack("<II", 0x54485356, len(cases)))
        for case in cases:
            out.write(case.pack())


# ---------- comparison

def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


class Tally:
    def __init__(self):
        self.floats = 0
        self.exact = 0
        self.nan = 0
        self.tiny = 0
        self.library = 0
        self.wrong = 0
        self.bytes = 0
        self.bytes_wrong = 0
        self.fill_wrong = 0
        self.messages = []
        self.notes = []

    def note(self, message):
        if len(self.notes) < 12:
            self.notes.append(message)

    def fail(self, message):
        if len(self.messages) < 25:
            self.messages.append(message)

    def compare_float(self, got, expected, soft, where):
        self.floats += 1
        if got != got and expected != expected:
            self.nan += 1
            return
        if got == expected and (got != 0.0 or math.copysign(1.0, got) == math.copysign(1.0, expected)):
            self.exact += 1
            return
        if got == expected:
            # zeros of different signs
            self.tiny += 1
            self.note("%s: got %r expected %r" % (where, got, expected))
            return
        if got == got and expected == expected and abs(got - expected) <= TINY:
            self.tiny += 1
            self.note("%s: got %r expected %r" % (where, got, expected))
            return
        if soft and got == got and expected == expected and not math.isinf(got) and not math.isinf(expected) and \
                abs(got - expected) <= LIBRARY_TOLERANCE * max(abs(got), abs(expected)):
            self.library += 1
            return
        self.wrong += 1
        self.fail("%s: got %r (%08x) expected %r (%08x)" % (where, got, bits(got) if got == got else 0, expected,
                                                            bits(expected) if expected == expected else 0))


class Spread:
    """how far results with estimates are from exact ones"""

    def __init__(self):
        self.count = 0
        self.same = 0
        self.worst = 0.0
        self.over = [0, 0, 0, 0]   # > 1e-6, 1e-5, 1e-4, 1e-3
        self.bytes = 0
        self.bytes_same = 0
        self.bytes_worst = 0

    def add(self, estimate, exact, scale):
        self.count += 1
        if estimate == exact or (estimate != estimate and exact != exact):
            self.same += 1
            return
        if estimate != estimate or exact != exact or math.isinf(estimate) or math.isinf(exact):
            self.worst = math.inf
            self.over = [n + 1 for n in self.over]
            return
        error = abs(estimate - exact) / max(scale, 1.0e-30)
        self.worst = max(self.worst, error)
        for index, limit in enumerate((1e-6, 1e-5, 1e-4, 1e-3)):
            if error > limit:
                self.over[index] += 1

    def add_byte(self, estimate, exact):
        self.bytes += 1
        if estimate == exact:
            self.bytes_same += 1
        self.bytes_worst = max(self.bytes_worst, abs(estimate - exact))


def record_fields(layout):
    """(offset, float count) of the arrays in a record, and the bytes no
    array covers"""
    fields = []
    if layout.position:
        fields.append(("position", 0, 4))
    for index, (source, components) in enumerate(layout.texcoords):
        if source != NONE:
            fields.append(("texcoord%d" % index, 16 + index * 16, max(components, 2)))
    if layout.fog:
        fields.append(("fog", 52, 1))
    covered = bytearray(RECORD_BYTES)
    for _name, offset, count in fields:
        covered[offset:offset + count * 4] = b"\x01" * (count * 4)
    if layout.color:
        covered[48:52] = b"\x01" * 4
    return fields, covered


def expected_record(case, layout, result):
    """the reference's values for a record: {field: floats}, colour bytes"""
    names = ref.OUTPUTS
    expected = {}
    if layout.position:
        expected["position"] = ref.position(result, case.constants, case.viewport_scale, case.viewport_offset,
                                            case.screen_offset, bool(layout.flags & CAPTURED),
                                            bool(layout.flags & FLIP_Y), bool(layout.flags & SIGNED_DEPTH))
    for index, (source, components) in enumerate(layout.texcoords):
        if source != NONE:
            expected["texcoord%d" % index] = result.registers[names[source]][:max(components, 2)]
    if layout.fog:
        expected["fog"] = result.registers["oFog"][:1]
    color = None
    if layout.color:
        rgb, alpha, alpha_component = layout.color
        values = result.registers[names[rgb]][:3] + [result.registers[names[alpha]][alpha_component]]
        color = [ref.color_byte(values[index], layout.scale[index] if layout.scale else None) for index in range(4)]
    return expected, color


def check_case(case, results, offset, tally, spreads, function_errors):
    """compares a case's records; returns the offset after them"""
    count = case.vertex_count
    size = count * RECORD_BYTES
    soft = uses_library(case.words)
    references = None
    if case.compare_reference:
        references = [ref.run_program(case.words, case.constants, case.vertex_inputs(vertex))
                      for vertex in range(count)]
    for layout in case.layouts:
        modes = [results[offset + mode * size: offset + (mode + 1) * size] for mode in range(4)]
        offset += 4 * size
        fields, covered = record_fields(layout)
        spread = spreads.setdefault(case.kind, Spread())
        for vertex in range(count):
            base = vertex * RECORD_BYTES
            exact = modes[0][base:base + RECORD_BYTES]
            estimate = modes[2][base:base + RECORD_BYTES]
            where = "%s, %s, vertex %d" % (case.name, layout.name, vertex)
            for index in range(RECORD_BYTES):
                if not covered[index] and (exact[index] != RECORD_FILL or estimate[index] != RECORD_FILL):
                    tally.fill_wrong += 1
                    tally.fail("%s: byte %d outside the arrays written" % (where, index))
                    break
            if references is not None:
                expected, color = expected_record(case, layout, references[vertex])
            for name, field_offset, float_count in fields:
                got = struct.unpack_from("<%df" % float_count, exact, field_offset)
                fast = struct.unpack_from("<%df" % float_count, estimate, field_offset)
                if references is not None:
                    for component in range(float_count):
                        tally.compare_float(got[component], expected[name][component], soft,
                                            "%s %s[%d]" % (where, name, component))
                scale = max(abs(x) for x in got if x == x and not math.isinf(x)) if \
                    any(x == x and not math.isinf(x) for x in got) else 0.0
                for component in range(float_count):
                    spread.add(fast[component], got[component], scale)
                if getattr(case, "function", None) and name == "texcoord0":
                    a, b = fast[0], got[0]
                    if a != b and not (a != a and b != b):
                        error = math.inf if (math.isinf(a) or math.isinf(b) or a != a or b != b or b == 0.0) else \
                            abs(a - b) / abs(b)
                        function_errors[case.function] = max(function_errors.get(case.function, 0.0), error)
                    function_errors.setdefault(case.function, 0.0)
            if layout.color:
                got = exact[48:52]
                fast = estimate[48:52]
                for component in range(4):
                    spread.add_byte(fast[component], got[component])
                    if references is None or color[component] is None:
                        continue
                    tally.bytes += 1
                    if got[component] != color[component]:
                        tally.bytes_wrong += 1
                        tally.fail("%s colour[%d]: got %d expected %d" % (where, component, got[component],
                                                                          color[component]))

    # the affine outputs' matrices
    affine_count = struct.unpack_from("<I", results, offset)[0]
    offset += 4
    for _ in range(affine_count):
        address, register = struct.unpack_from("<II", results, offset)
        matrix = struct.unpack_from("<20f", results, offset + 8)
        offset += 88
        if not case.check_affine or references is None:
            continue
        tally.affine_outputs += 1
        for vertex in range(0, count, 7):
            result = references[vertex]
            if address == O_POS:
                expected = result.clip if result.captured else result.registers["oPos"]
            else:
                expected = result.registers[ref.OUTPUTS[address]]
            v = case.vertex_inputs(vertex)[register] if register != NONE else [0.0, 0.0, 0.0, 0.0]
            for row in range(4):
                terms = [matrix[row * 5 + column] * v[column] for column in range(4)] + [matrix[row * 5 + 4]]
                value = sum(terms)
                size_of_terms = sum(abs(term) for term in terms)
                tally.affine_values += 1
                if not abs(value - expected[row]) <= 2.0e-5 * size_of_terms + 1.0e-30:
                    tally.affine_wrong += 1
                    tally.fail("%s: affine %s row %d of vertex %d: matrix gives %r, the program %r" %
                               (case.name, ref.OUTPUTS[address], row, vertex, value, expected[row]))
    return offset


# ---------- commands

def game_cases(rng, sets, vertices):
    cases = []
    for number, words in enumerate(ref.game_programs()):
        for index in range(sets):
            case = Case("vs%02d set %d" % (number, index), words, check_affine=True)
            case.kind = "game programs"
            random_constants(rng, case)
            # one set in three in the game's own vertex formats
            # (and not always a multiple of four vertices)
            random_stream(rng, case, vertices - index % 4, native=index % 3 == 2)
            cases.append(case)
    return cases


def fuzz_cases(rng, count, vertices):
    cases = []
    for index in range(count):
        library = index % 2 == 1
        case = Case("random program %d" % index, random_program(rng, library))
        case.kind = "random programs with exp, log, lit" if library else "random programs"
        random_constants(rng, case)
        # from one vertex up
        random_stream(rng, case, 1 + index % vertices)
        cases.append(case)
    return cases


def run_tests(arguments):
    rng = random.Random(arguments.seed)
    work = Path(arguments.work)
    work.mkdir(parents=True, exist_ok=True)
    started = time.time()
    cases = game_cases(rng, arguments.sets, arguments.vertices)
    game_count = len(cases)
    fuzz = fuzz_cases(rng, arguments.fuzz, 48)
    cases += fuzz
    exhaustive = exhaustive_cases()
    for case in exhaustive:
        case.kind = "element formats"
    cases += exhaustive
    functions = function_cases(rng, arguments.function_values)
    for case in functions:
        case.kind = "rcp, rcc, rsq alone"
    cases += functions
    case_file = work / "cases.bin"
    result_file = work / "results.bin"
    write_cases(case_file, cases)
    print("# %d cases (%d of the game's programs x %d sets x %d vertices, %d random programs, %d others)" % (
        len(cases), game_count // max(arguments.sets, 1), arguments.sets, arguments.vertices, len(fuzz),
        len(exhaustive) + len(functions)))
    process = subprocess.run([arguments.exe, "run", str(case_file), str(result_file)], capture_output=True, text=True)
    print("# t_vsh_cpu: " + process.stdout.strip())
    failures = 0
    if process.returncode != 0:
        print("FAIL t_vsh_cpu returned %d %s" % (process.returncode, process.stderr.strip()))
        failures += 1
    results = result_file.read_bytes()
    tally = Tally()
    tally.affine_outputs = tally.affine_values = tally.affine_wrong = 0
    spreads = {}
    function_errors = {}
    offset = 0
    for case in cases:
        offset = check_case(case, results, offset, tally, spreads, function_errors)
    if offset != len(results):
        print("FAIL the results file has %d bytes, %d expected" % (len(results), offset))
        failures += 1
    print("# exact evaluator against the reference: %d floats: %d the same bits, %d NaN on both sides, "
          "%d within %g of each other (zeros, flushed results), %d within %g relative (C library), %d wrong" % (
              tally.floats, tally.exact, tally.nan, tally.tiny, TINY, tally.library, LIBRARY_TOLERANCE, tally.wrong))
    print("# colour bytes: %d compared, %d wrong; records with bytes written outside the arrays: %d" % (
        tally.bytes, tally.bytes_wrong, tally.fill_wrong))
    print("# affine outputs: %d matrices, %d values compared, %d wrong" % (
        tally.affine_outputs, tally.affine_values, tally.affine_wrong))
    for name in ("rcp", "rcc", "rsq"):
        error = function_errors.get(name, math.nan)
        print("# %s from the estimate: largest relative error %.3g (2^%.2f), tolerance 2^-20" % (
            name, error, math.log2(error) if error > 0.0 and not math.isinf(error) else -math.inf))
        if not error <= ESTIMATE_TOLERANCE:
            print("FAIL %s exceeds the tolerance" % name)
            failures += 1
    for kind, spread in spreads.items():
        print("# estimates against exact, %s: %d floats, %d the same bits, largest difference %.3g of the "
              "array's largest component; above 1e-6: %d, 1e-5: %d, 1e-4: %d, 1e-3: %d; colour bytes %d, "
              "%d the same, largest difference %d" % (
                  kind, spread.count, spread.same, spread.worst, spread.over[0], spread.over[1], spread.over[2],
                  spread.over[3], spread.bytes, spread.bytes_same, spread.bytes_worst))
    for message in tally.notes:
        print("# (within %g) %s" % (TINY, message))
    for message in tally.messages:
        print("FAIL " + message)
    failures += tally.wrong + tally.bytes_wrong + tally.fill_wrong + tally.affine_wrong
    print("# %.0f seconds; %d failures" % (time.time() - started, failures))
    return 1 if failures else 0


def program_case(words, name):
    case = Case(name, words)
    case.layouts = []
    return case


def run_classes(arguments):
    work = Path(arguments.work)
    work.mkdir(parents=True, exist_ok=True)
    cases = [program_case(words, "vs%02d" % number) for number, words in enumerate(ref.game_programs())]
    write_cases(work / "classes.bin", cases)
    process = subprocess.run([arguments.exe, "classes", str(work / "classes.bin")], capture_output=True, text=True)
    sys.stdout.write(process.stdout)
    return process.returncode


BENCH_LAYOUTS = [
    Layout("position T0 D0", flags=CAPTURED | GL, texcoords=((O_T0, 2), (NONE, 0)), color=(O_D0, O_D0, 3)),
    Layout("position T0 T1 D0 fog", flags=CAPTURED | GL, texcoords=((O_T0, 4), (O_T1, 4)), color=(O_D0, O_D0, 3),
           fog=True),
]


def bench_cases(arguments):
    rng = random.Random(arguments.seed)
    cases = []
    for number, words in enumerate(ref.game_programs()):
        case = Case("vs%02d" % number, words)
        random_constants(rng, case)
        # no values that trap: constants of ordinary size
        for constant in case.constants:
            for component in range(4):
                constant[component] = ref.f32(rng.uniform(0.25, 2.0) * rng.choice((-1.0, 1.0)))
        case.constants[ref.CONSTANT_BIAS - 38] = list(case.viewport_scale)
        case.constants[ref.CONSTANT_BIAS - 37] = list(case.viewport_offset)
        random_stream(rng, case, arguments.vertices, native=True)
        case.layouts = BENCH_LAYOUTS
        cases.append(case)
    return cases


def run_bench(arguments):
    """vertices a second on this machine: PROGRAM ARRAYS SCALAR SSE, then the
    reduced program's operations (see t_vsh_cpu.c)"""
    work = Path(arguments.work)
    work.mkdir(parents=True, exist_ok=True)
    write_cases(work / "bench.bin", bench_cases(arguments))
    process = subprocess.run([arguments.exe, "bench", str(work / "bench.bin")], capture_output=True, text=True)
    sys.stdout.write(process.stdout)
    return process.returncode


# A Pentium III (Coppermine) by its instruction tables, as far as they are
# remembered here (Intel's optimisation manual and Agner Fog's tables for
# the P6 core): micro-operations of each kind of instruction t_vsh_cpu
# counts. The core issues and retires three micro-operations a cycle,
# decodes at most one instruction of more than one micro-operation a cycle
# (and up to two single ones beside it), reads memory once and writes it
# once a cycle. A 128-bit SSE operation is two 64-bit micro-operations.
P3_MICRO_OPERATIONS = {
    "packed_load": 2, "packed_store": 4, "half_store": 2, "packed_register": 2, "packed_memory": 4, "shuffle": 3,
    "divide": 2, "scalar_sse": 2, "x87": 2, "indirect_jump": 1, "call": 4,
}
P3_KINDS = ("packed_load", "packed_store", "half_store", "packed_register", "packed_memory", "shuffle", "divide",
            "scalar_sse", "x87", "indirect_jump", "call")
# a mispredicted branch (the pipeline's length), and divps or sqrtps
P3_MISPREDICT_CYCLES = 12
P3_DIVIDE_CYCLES = 36
P3_MHZ = 900.0


def p3_cycles(counts):
    """(the least cycles the instruction counts allow, the same with every
    indirect jump mispredicted) for a dict of instruction counts"""
    kinds = sum(counts[kind] for kind in P3_KINDS)
    plain = counts["total"] - kinds
    micro = plain + sum(counts[kind] * P3_MICRO_OPERATIONS[kind] for kind in P3_KINDS)
    several = kinds - counts["indirect_jump"]
    single = plain + counts["indirect_jump"]
    decode = several + max(0.0, single - 2.0 * several) / 3.0
    loads = counts["packed_load"] * 2 + counts["packed_memory"] * 2
    stores = counts["packed_store"] * 2 + counts["half_store"]
    least = max(micro / 3.0, decode, loads, stores) + counts["divide"] * P3_DIVIDE_CYCLES
    return least, least + counts["indirect_jump"] * P3_MISPREDICT_CYCLES


def run_count(arguments):
    """instructions a vertex of each program, counted by single-stepping the
    evaluators (built for the Pentium III), and what they cost that processor"""
    work = Path(arguments.work)
    work.mkdir(parents=True, exist_ok=True)
    write_cases(work / "count.bin", bench_cases(arguments))
    process = subprocess.run([arguments.exe, "count", str(work / "count.bin")], capture_output=True, text=True)
    if process.returncode != 0:
        print("FAIL t_vsh_cpu returned %d" % process.returncode)
        return 1
    print("# each further vertex of a run: instructions, SSE register operations (packed arithmetic, loads, stores, shuffles), "
          "micro-operations, cycles at least / with every dispatch mispredicted, "
          "million vertices a second at %d MHz" % P3_MHZ)
    totals = {}
    for line in process.stdout.splitlines():
        fields = line.split()
        program, layout, mode, vertices = int(fields[0]), int(fields[1]), fields[2], int(fields[3])
        # counted for a quarter of the vertices and for all of them
        kinds = 1 + len(P3_KINDS)
        quarter = [int(x) for x in fields[4:4 + kinds]]
        whole = [int(x) for x in fields[4 + kinds:4 + 2 * kinds]]
        numbers = [(b - a) / (vertices - vertices // 4) for a, b in zip(quarter, whole)]
        counts = dict(zip(("total",) + P3_KINDS, numbers))
        call = whole[0] - numbers[0] * vertices
        least, likely = p3_cycles(counts)
        packed = counts["packed_load"] + counts["packed_store"] + counts["packed_register"] + \
            counts["packed_memory"] + counts["shuffle"] + counts["half_store"]
        kinds = sum(counts[kind] for kind in P3_KINDS)
        micro = counts["total"] - kinds + sum(counts[kind] * P3_MICRO_OPERATIONS[kind] for kind in P3_KINDS)
        print("vs%02d %-22s %-6s %7.1f instructions %6.1f SSE %7.1f micro-operations %6.1f / %6.1f cycles "
              "%5.2f / %5.2f Mvert/s  (jumps %.2f, x87 %.1f, calls %.2f; %d instructions a run besides)" % (
                  program, BENCH_LAYOUTS[layout].name, mode, counts["total"], packed, micro, least, likely,
                  P3_MHZ / least, P3_MHZ / likely, counts["indirect_jump"], counts["x87"], counts["call"], call))
        total = totals.setdefault((layout, mode), [0.0, 0.0, 0.0, 0])
        total[0] += counts["total"]
        total[1] += least
        total[2] += likely
        total[3] += 1
    for (layout, mode), total in sorted(totals.items()):
        print("# mean of %d programs, %s, %s: %.0f instructions, %.0f / %.0f cycles a vertex" % (
            total[3], BENCH_LAYOUTS[layout].name, mode, total[0] / total[3], total[1] / total[3],
            total[2] / total[3]))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("exe")
    parser.add_argument("--work", default="build/win9x/tests/renderer")
    parser.add_argument("--sets", type=int, default=9)
    parser.add_argument("--vertices", type=int, default=256)
    parser.add_argument("--fuzz", type=int, default=600)
    parser.add_argument("--function-values", type=int, default=200000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--classes", action="store_true")
    parser.add_argument("--bench", action="store_true")
    parser.add_argument("--count", action="store_true")
    arguments = parser.parse_args()
    if arguments.classes:
        return run_classes(arguments)
    if arguments.bench:
        return run_bench(arguments)
    if arguments.count:
        return run_count(arguments)
    return run_tests(arguments)


if __name__ == "__main__":
    sys.exit(main())
