#!/usr/bin/env python3
"""A reference interpreter of NV2A vertex programs.

It executes what the GLSL that port/linux/src/nv2a_vsh.c generates for a
program says, statement by statement, in single precision (every operation
rounded to a 32-bit float, results too small for a normal number flushed to
zero, as the evaluators of port/linux/src/nv2a_vsh_cpu.c run). It is written
from nv2a_vsh.c and shares nothing with nv2a_vsh_cpu.c, whose results
tools/nv2a_vsh_cpu_test.py compares with it.

Where GLSL leaves an expression's evaluation open, this is the reading:
  dot(a, b)       products added from left to right
  min(x, y)       y < x ? y : x          max(x, y)   x < y ? y : x
  clamp(x, a, b)  min(max(x, a), b)
  int(floor(x))   the smallest int when no int holds the value
  floor(log2(x)) and x / exp2(floor(log2(x)))   computed exactly (the
                  exponent and the mantissa), not through a rounded logarithm
  inversesqrt(x)  1.0 / sqrt(x), both rounded

usage: nv2a_vsh_reference.py [PROGRAM...]   disassembles the game's programs
"""

import math
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONSTANT_COUNT = 192
CONSTANT_BIAS = 96

MAC = ["nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl"]
ILU = ["nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"]
OUTPUTS = {0: "oPos", 3: "oD0", 4: "oD1", 5: "oFog", 6: "oPts", 7: "oB0", 8: "oB1",
           9: "oT0", 10: "oT1", 11: "oT2", 12: "oT3"}

FLT_MIN = 1.1754943508222875e-38
INT_MIN = -2147483648
INF = math.inf
NAN = math.nan


# ---------- the game's programs

def game_programs():
    """the instruction words of the game's vertex programs, in the order of
    source/rasterizer/xbox/rasterizer_xbox_vertex_shaders.c's table"""
    directory = REPO / "source" / "rasterizer" / "xbox"
    data = (directory / "rasterizer_xbox_vertex_shaders_data.inc").read_text()
    data = re.sub(r"/\*.*?\*/", "", data, flags=re.S)
    words = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]{8})", data)]
    table = (directory / "rasterizer_xbox_vertex_shaders.c").read_text()
    programs = []
    for offset, _size in re.findall(r"VERTEX_SHADER_ENTRY\(0x([0-9A-F]+), 0x([0-9A-F]+)\)", table):
        base = int(offset, 16) // 4
        # header: program type in the low word, instruction count in the high
        count = words[base] >> 16
        programs.append(words[base + 1: base + 1 + count * 4])
    return programs


# ---------- single precision

def f32(value):
    """the nearest 32-bit float, or zero for a result below the normal range"""
    try:
        rounded = struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(INF, value)
    if rounded != 0.0 and abs(rounded) < FLT_MIN:
        return math.copysign(0.0, rounded)
    return rounded


def add(a, b):
    return f32(a + b)


def mul(a, b):
    if (math.isinf(a) and b == 0.0) or (math.isinf(b) and a == 0.0):
        return NAN
    return f32(a * b)


def div(a, b):
    if b == 0.0:
        if a == 0.0 or a != a:
            return NAN
        return math.copysign(INF, a) * math.copysign(1.0, b)
    if math.isinf(a) and math.isinf(b):
        return NAN
    return f32(a / b)


def glsl_min(x, y):
    return y if y < x else x


def glsl_max(x, y):
    return y if x < y else x


def glsl_clamp(x, low, high):
    return glsl_min(glsl_max(x, low), high)


def glsl_floor(x):
    if x != x or math.isinf(x) or x == 0.0:
        return x
    return float(math.floor(x))


def glsl_exp2(x):
    if x != x:
        return NAN
    if x == INF:
        return INF
    if x == -INF:
        return 0.0
    try:
        return f32(2.0 ** x)
    except OverflowError:
        return INF


def glsl_log2(x):
    if x != x or x < 0.0:
        return NAN
    if x == 0.0:
        return -INF
    if x == INF:
        return INF
    return f32(math.log2(x))


def glsl_pow(x, y):
    """GLSL leaves pow(0, y <= 0) open; this is C's powf, which the
    evaluator calls"""
    try:
        return f32(math.pow(x, y))
    except OverflowError:
        return INF
    except ValueError:
        if x == 0.0 and y < 0.0:
            return math.copysign(INF, x) if (y == math.floor(y) and math.floor(y) % 2 == 1) else INF
        return NAN


def nv2a_rcc(x):
    r = div(1.0, x)
    if r > 0.0:
        return glsl_clamp(r, f32(5.42101e-20), f32(1.884467e+19))
    return glsl_clamp(r, f32(-1.884467e+19), f32(-5.42101e-20))


def nv2a_exp(x):
    whole = glsl_floor(x)
    return [glsl_exp2(whole), add(x, -whole), glsl_exp2(x), 1.0]


def nv2a_log(x):
    x = abs(x)
    if x == 0.0:
        return [f32(-1.0e30), 1.0, f32(-1.0e30), 1.0]
    if x != x or math.isinf(x):
        e = glsl_floor(glsl_log2(x))
        return [e, div(x, glsl_exp2(e)), glsl_log2(x), 1.0]
    # exactly: a double holds the logarithm of a float well enough to floor it
    e = float(math.floor(math.log2(x)))
    while 2.0 ** e > x:
        e -= 1.0
    while 2.0 ** (e + 1.0) <= x:
        e += 1.0
    return [e, f32(x / 2.0 ** e), glsl_log2(x), 1.0]


def nv2a_lit(s):
    specular = 0.0
    if s[0] > 0.0:
        specular = glsl_pow(glsl_max(s[1], 0.0), glsl_clamp(s[3], f32(-127.9961), f32(127.9961)))
    return [1.0, glsl_max(s[0], 0.0), specular, 1.0]


def floor_to_int(x):
    """int(floor(x))"""
    if x != x or math.isinf(x) or x >= 2147483648.0 or x < -2147483648.0:
        return INT_MIN
    return math.floor(x)


# ---------- instructions

def field(instruction, word, low_bit, bit_count):
    return (instruction[word] >> low_bit) & ((1 << bit_count) - 1)


def operand_fields(instruction, which):
    """(negate, swizzle, index, mux) of operand A, B or C"""
    if which == "A":
        return (field(instruction, 1, 8, 1),
                [field(instruction, 1, 6, 2), field(instruction, 1, 4, 2), field(instruction, 1, 2, 2),
                 field(instruction, 1, 0, 2)],
                field(instruction, 2, 28, 4), field(instruction, 2, 26, 2))
    if which == "B":
        return (field(instruction, 2, 25, 1),
                [field(instruction, 2, 23, 2), field(instruction, 2, 21, 2), field(instruction, 2, 19, 2),
                 field(instruction, 2, 17, 2)],
                field(instruction, 2, 13, 4), field(instruction, 2, 11, 2))
    return (field(instruction, 2, 10, 1),
            [field(instruction, 2, 8, 2), field(instruction, 2, 6, 2), field(instruction, 2, 4, 2),
             field(instruction, 2, 2, 2)],
            (field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2), field(instruction, 3, 28, 2))


def mask_indices(mask):
    """bit 3 is x"""
    return [index for index, bit in enumerate((8, 4, 2, 1)) if mask & bit]


class Result:
    """what the shader's main() leaves behind for one vertex"""
    __slots__ = ("registers", "position", "captured", "clip")


def run_program(words, constants, inputs):
    """Executes the program for one vertex. constants: 192 lists of four
    floats; inputs: 16 lists of four floats (v0..v15, already unpacked).
    Returns the registers by name, as main() has them before its last
    statements."""
    r = {"r%d" % index: [0.0, 0.0, 0.0, 0.0] for index in range(12)}
    for name in ("oPos", "oD0", "oD1", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3"):
        r[name] = [0.0, 0.0, 0.0, 1.0]
    r["oFog"] = [1.0, 1.0, 1.0, 1.0]
    # point_size is the uniform; no test reads it
    r["oPts"] = [1.0, 1.0, 1.0, 1.0]
    r["oUnused"] = [0.0, 0.0, 0.0, 0.0]
    a0 = 0
    clip_position = [0.0, 0.0, 0.0, 0.0]
    clip_captured = False
    mac = [0.0, 0.0, 0.0, 0.0]
    ilu = [0.0, 0.0, 0.0, 0.0]

    for base in range(0, len(words), 4):
        instruction = words[base:base + 4]
        relative = field(instruction, 3, 1, 1)

        def operand(which):
            negate, swizzle, index, mux = operand_fields(instruction, which)
            if mux == 1:
                # r12 reads back the position output
                source = r["oPos"] if index == 12 else r.get("r%d" % index, [0.0, 0.0, 0.0, 0.0])
            elif mux == 2:
                source = inputs[field(instruction, 1, 9, 4)]
            elif mux == 3:
                number = field(instruction, 1, 13, 8)
                if relative:
                    number = min(max(a0 + number, 0), CONSTANT_COUNT - 1)
                # c[] has 192 elements; a program that names one past them
                # reads the last (what the evaluator does with it)
                source = constants[min(number, CONSTANT_COUNT - 1)]
            else:
                source = [0.0, 0.0, 0.0, 0.0]
            value = [source[component] for component in swizzle]
            return [-x for x in value] if negate else value

        A = operand("A")
        B = operand("B")
        C = operand("C")
        mac_op = field(instruction, 1, 21, 4)
        ilu_op = field(instruction, 1, 25, 3)
        mac_mask = field(instruction, 3, 24, 4)
        temporary = field(instruction, 3, 20, 4)
        ilu_mask = field(instruction, 3, 16, 4)
        output_mask = field(instruction, 3, 12, 4)
        output_is_register = field(instruction, 3, 11, 1)
        output_address = field(instruction, 3, 3, 8)
        output_from_ilu = field(instruction, 3, 2, 1)

        name = MAC[mac_op] if mac_op < len(MAC) else "unknown"
        if name in ("mov", "arl"):
            mac = list(A)
        elif name == "mul":
            mac = [mul(a, b) for a, b in zip(A, B)]
        elif name == "add":
            mac = [add(a, c) for a, c in zip(A, C)]
        elif name == "mad":
            mac = [add(mul(a, b), c) for a, b, c in zip(A, B, C)]
        elif name == "dp3":
            mac = [add(add(mul(A[0], B[0]), mul(A[1], B[1])), mul(A[2], B[2]))] * 4
        elif name == "dph":
            mac = [add(add(add(mul(A[0], B[0]), mul(A[1], B[1])), mul(A[2], B[2])), B[3])] * 4
        elif name == "dp4":
            mac = [add(add(add(mul(A[0], B[0]), mul(A[1], B[1])), mul(A[2], B[2])), mul(A[3], B[3]))] * 4
        elif name == "dst":
            mac = [1.0, mul(A[1], B[1]), A[2], B[3]]
        elif name == "min":
            mac = [glsl_min(a, b) for a, b in zip(A, B)]
        elif name == "max":
            mac = [glsl_max(a, b) for a, b in zip(A, B)]
        elif name == "slt":
            mac = [1.0 if a < b else 0.0 for a, b in zip(A, B)]
        elif name == "sge":
            mac = [1.0 if a >= b else 0.0 for a, b in zip(A, B)]
        elif name == "unknown":
            mac = [0.0, 0.0, 0.0, 0.0]

        name = ILU[ilu_op]
        if name == "mov":
            ilu = list(C)
        elif name == "rcp":
            ilu = [div(1.0, C[0])] * 4
        elif name == "rcc":
            ilu = [nv2a_rcc(C[0])] * 4
        elif name == "rsq":
            magnitude = abs(C[0])
            ilu = [div(1.0, f32(math.sqrt(magnitude)) if magnitude == magnitude else NAN)] * 4
        elif name == "exp":
            ilu = nv2a_exp(C[0])
        elif name == "log":
            ilu = nv2a_log(C[0])
        elif name == "lit":
            ilu = nv2a_lit(C)
        if ilu_op == 3 and field(instruction, 3, 28, 2) == 1 and \
                ((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2)) == 12:
            clip_position = list(r["oPos"])
            clip_captured = True

        # results are written only after both units have read their inputs
        if mac_op == 13:
            a0 = floor_to_int(add(mac[0], f32(0.001)))
        elif mac_op != 0 and mac_mask:
            target = r["oPos"] if temporary == 12 else r.get("r%d" % temporary)
            if target is not None:
                for index in mask_indices(mac_mask):
                    target[index] = mac[index]
        if ilu_op != 0 and ilu_mask:
            ilu_temporary = 1 if mac_op != 0 else temporary
            target = r["oPos"] if ilu_temporary == 12 else r.get("r%d" % ilu_temporary)
            if target is not None:
                for index in mask_indices(ilu_mask):
                    target[index] = ilu[index]
        if output_mask and (ilu_op if output_from_ilu else mac_op) != 0 and output_is_register:
            source = ilu if output_from_ilu else mac
            target = r[OUTPUTS.get(output_address, "oUnused")]
            for index in mask_indices(output_mask):
                target[index] = source[index]
        if field(instruction, 3, 0, 1):
            break

    result = Result()
    result.registers = r
    result.captured = clip_captured
    result.clip = clip_position
    return result


def position(result, constants, viewport_scale, viewport_offset, screen_offset, captured, flip_y, signed_depth):
    """gl_Position: the screen-space conversion undone. `captured` picks
    the text nv2a_vsh.c generates under HALO_ANDROID (the position kept from
    before the conversion, where the program has one); flip_y and
    signed_depth are that build's last two statements."""
    o = result.registers["oPos"]
    scale = [viewport_scale[index] if viewport_scale[index] != 0.0 else 1.0 for index in range(3)]
    bias = [add(0.5, screen_offset), 0.5, 0.0]
    if captured and result.captured:
        clip = result.clip
        c58 = constants[CONSTANT_BIAS - 38]
        c59 = constants[CONSTANT_BIAS - 37]
        out = []
        for index in range(3):
            addend = add(add(c59[index], bias[index]), -viewport_offset[index])
            out.append(div(add(mul(clip[index], c58[index]), mul(addend, clip[3])), scale[index]))
        out.append(clip[3])
    else:
        ndc = [div(add(add(o[0], bias[0]), -viewport_offset[0]), scale[0]),
               div(add(add(o[1], bias[1]), -viewport_offset[1]), scale[1]),
               div(add(o[2], -viewport_offset[2]), scale[2])]
        out = [mul(ndc[0], o[3]), mul(ndc[1], o[3]), mul(ndc[2], o[3]), o[3]]
    if flip_y:
        out[1] = -out[1]
    if signed_depth:
        out[2] = add(mul(2.0, out[2]), -out[3])
    return out


# ---------- vertex elements (the formats of d3d8_gl.c's attribute_format,
# converted as OpenGL 4.5 converts them, and unpack_normpacked3)

FLOAT1, FLOAT2, FLOAT3, FLOAT4, FLOAT2H = 0x12, 0x22, 0x32, 0x42, 0x72
D3DCOLOR = 0x40
SHORT1, SHORT2, SHORT3, SHORT4 = 0x15, 0x25, 0x35, 0x45
NORMSHORT1, NORMSHORT2, NORMSHORT3, NORMSHORT4 = 0x11, 0x21, 0x31, 0x41
NORMPACKED3 = 0x16
PBYTE1, PBYTE2, PBYTE3, PBYTE4 = 0x14, 0x24, 0x34, 0x44

TYPE_BYTES = {
    FLOAT1: 4, FLOAT2: 8, FLOAT3: 12, FLOAT4: 16, FLOAT2H: 12, D3DCOLOR: 4,
    SHORT1: 2, SHORT2: 4, SHORT3: 6, SHORT4: 8,
    NORMSHORT1: 2, NORMSHORT2: 4, NORMSHORT3: 6, NORMSHORT4: 8,
    NORMPACKED3: 4, PBYTE1: 1, PBYTE2: 2, PBYTE3: 3, PBYTE4: 4,
}


def read_element(element_type, data, offset):
    """v: four floats; missing components are 0, 0, 1"""
    value = [0.0, 0.0, 0.0, 1.0]
    if element_type in (FLOAT1, FLOAT2, FLOAT3, FLOAT4, FLOAT2H):
        count = {FLOAT1: 1, FLOAT2: 2, FLOAT3: 3, FLOAT4: 4, FLOAT2H: 3}[element_type]
        value[:count] = struct.unpack_from("<%df" % count, data, offset)
    elif element_type == D3DCOLOR:
        # GL_BGRA: blue first in memory
        blue, green, red, alpha = data[offset:offset + 4]
        value = [f32(red / 255.0), f32(green / 255.0), f32(blue / 255.0), f32(alpha / 255.0)]
    elif element_type in (SHORT1, SHORT2, SHORT3, SHORT4):
        count = (element_type >> 4)
        value[:count] = [float(x) for x in struct.unpack_from("<%dh" % count, data, offset)]
    elif element_type in (NORMSHORT1, NORMSHORT2, NORMSHORT3, NORMSHORT4):
        count = (element_type >> 4)
        value[:count] = [max(f32(x / 32767.0), -1.0) for x in struct.unpack_from("<%dh" % count, data, offset)]
    elif element_type == NORMPACKED3:
        p = struct.unpack_from("<I", data, offset)[0]

        def signed(bits, width):
            return bits - (1 << width) if bits & (1 << (width - 1)) else bits
        x = signed(p & 0x7FF, 11)
        y = signed((p >> 11) & 0x7FF, 11)
        z = signed(p >> 22, 10)
        value = [f32(x / 1023.0), f32(y / 1023.0), f32(z / 511.0), 1.0]
    elif element_type in (PBYTE1, PBYTE2, PBYTE3, PBYTE4):
        count = (element_type >> 4)
        value[:count] = [f32(x / 255.0) for x in data[offset:offset + count]]
    else:
        value = list(struct.unpack_from("<4f", data, offset))
    return value


def color_byte(value, scale=None):
    """an output register's component as the evaluator stores a colour:
    clamp(x, 0.0, 1.0) as the GLSL's last statements do, the array's scale,
    and the nearest of 256 levels (the framebuffer's conversion)"""
    value = glsl_clamp(value, 0.0, 1.0)
    if scale is not None:
        value = glsl_clamp(mul(value, scale), 0.0, 1.0)
    if value != value:
        # GLSL leaves the clamp of a NaN open; the evaluator stores 0
        return 0
    # the nearest level, a tie to the even one: 2^23 added and taken away
    # again, which is how a float is rounded to a whole number
    return int(add(add(mul(value, 255.0), 8388608.0), -8388608.0))


# ---------- disassembly

def disassemble(words):
    def mask(m):
        return "".join(c for bit, c in ((8, "x"), (4, "y"), (2, "z"), (1, "w")) if m & bit)

    def operand(instruction, which, relative):
        negate, swizzle, index, mux = operand_fields(instruction, which)
        text = "-" if negate else ""
        if mux == 1:
            text += "oPos" if index == 12 else "r%d" % index
        elif mux == 2:
            text += "v%d" % field(instruction, 1, 9, 4)
        elif mux == 3:
            text += ("c[a0%+d]" if relative else "c[%d]") % (field(instruction, 1, 13, 8) - CONSTANT_BIAS)
        else:
            text += "0"
        letters = "".join("xyzw"[component] for component in swizzle)
        if letters != "xyzw":
            text += "." + (letters[0] if len(set(letters)) == 1 else letters)
        return text

    reads = {"mov": "A", "mul": "AB", "add": "AC", "mad": "ABC", "dp3": "AB", "dph": "AB", "dp4": "AB", "dst": "AB",
             "min": "AB", "max": "AB", "slt": "AB", "sge": "AB", "arl": "A"}
    lines = []
    for base in range(0, len(words), 4):
        instruction = words[base:base + 4]
        mac_op = field(instruction, 1, 21, 4)
        ilu_op = field(instruction, 1, 25, 3)
        relative = field(instruction, 3, 1, 1)
        mac_mask = field(instruction, 3, 24, 4)
        temporary = field(instruction, 3, 20, 4)
        ilu_mask = field(instruction, 3, 16, 4)
        output_mask = field(instruction, 3, 12, 4)
        output = OUTPUTS.get(field(instruction, 3, 3, 8), "o?") if field(instruction, 3, 11, 1) else None
        from_ilu = field(instruction, 3, 2, 1)
        parts = []
        if mac_op:
            name = MAC[mac_op] if mac_op < len(MAC) else "mac%d" % mac_op
            targets = []
            if name == "arl":
                targets.append("a0")
            elif mac_mask:
                targets.append(("oPos" if temporary == 12 else "r%d" % temporary) + "." + mask(mac_mask))
            if output_mask and not from_ilu and output:
                targets.append(output + "." + mask(output_mask))
            parts.append("%s %s, %s" % (name, "/".join(targets) or "-",
                                         ", ".join(operand(instruction, which, relative) for which in reads.get(name, ""))))
        if ilu_op:
            targets = []
            ilu_temporary = 1 if mac_op else temporary
            if ilu_mask:
                targets.append(("oPos" if ilu_temporary == 12 else "r%d" % ilu_temporary) + "." + mask(ilu_mask))
            if output_mask and from_ilu and output:
                targets.append(output + "." + mask(output_mask))
            parts.append("%s %s, %s" % (ILU[ilu_op], "/".join(targets) or "-", operand(instruction, "C", relative)))
        lines.append("%3d  %s" % (base // 4, "  ||  ".join(parts)))
        if field(instruction, 3, 0, 1):
            break
    return lines


if __name__ == "__main__":
    programs = game_programs()
    for argument in (sys.argv[1:] or range(len(programs))):
        print("---- vs%02d" % int(argument))
        print("\n".join(disassemble(programs[int(argument)])))
