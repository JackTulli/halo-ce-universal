#!/usr/bin/env python3
"""Checks GLSL written by tools/gl2_shader_dump.c.

    gl2_shader_check.py glsl110 <folder>
        every .glsl file of the folder is "#version 110" and uses nothing
        newer than GLSL 1.10: no in/out/layout/invariant/precision
        qualifiers, no texture() or the other overloaded lookups, no
        unsigned or bit operations, no switch, no integer %, no float
        suffixes, no built-ins of later versions.

    gl2_shader_check.py same <folder> <folder>
        the two folders hold the same files with the same bytes.

Exit status 0 when everything holds; otherwise each failure is printed.
"""

import re
import sys
from pathlib import Path

# words GLSL 1.10 does not have (or has reserved), as whole tokens
FORBIDDEN_WORDS = {
    # qualifiers and declarations of 1.20 and later
    "in", "out", "layout", "invariant", "precision", "highp", "mediump", "lowp", "flat", "smooth",
    "noperspective", "centroid", "patch", "sample", "subroutine",
    # types
    "uint", "uvec2", "uvec3", "uvec4", "unsigned", "double", "dvec2", "dvec3", "dvec4",
    "isampler2D", "usampler2D", "sampler2DArray", "sampler2DRect", "samplerBuffer", "atomic_uint",
    "mat2x3", "mat2x4", "mat3x2", "mat3x4", "mat4x2", "mat4x3",
    # statements
    "switch", "case", "default",
    # lookups and built-ins that came later
    "texture", "textureLod", "textureProj", "textureGrad", "textureOffset", "texelFetch", "textureSize",
    "round", "roundEven", "trunc", "modf", "isnan", "isinf", "floatBitsToInt", "floatBitsToUint",
    "intBitsToFloat", "uintBitsToFloat", "fma", "inverse", "transpose", "determinant", "outerProduct",
    "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "bitCount", "bitfieldExtract",
    # outputs that are not gl_FragColor
    "fragment_color", "gl_VertexID", "gl_InstanceID",
}
FORBIDDEN_OPERATORS = ["<<", ">>", "%", "~", "^"]

TOKEN = re.compile(r"""
    (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<directive>\#[^\n]*)
  | (?P<number>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[A-Za-z]*)
  | (?P<word>[A-Za-z_]\w*)
  | (?P<operator><<=?|>>=?|&&|\|\||\^\^|[-+*/%<>=!&|^]=?|[~?:;,.(){}\[\]])
  | (?P<space>\s+)
""", re.S | re.X)


def tokens(text):
    position = 0
    while position < len(text):
        match = TOKEN.match(text, position)
        if not match:
            yield "unknown", text[position], position
            position += 1
            continue
        position = match.end()
        yield match.lastgroup, match.group(), match.start()


def check_glsl110(path):
    text = path.read_bytes().decode("ascii")
    failures = []

    def fail(what, position):
        failures.append("%s:%d: %s" % (path.name, text.count("\n", 0, position) + 1, what))

    if not text.startswith("#version 110\n"):
        fail("does not start with #version 110", 0)
    for kind, token, position in tokens(text):
        if kind in ("comment", "space"):
            continue
        if kind == "unknown":
            fail("unexpected character %r" % token, position)
        elif kind == "directive":
            if token != "#version 110":
                fail("directive %s" % token, position)
        elif kind == "word":
            if token in FORBIDDEN_WORDS:
                fail("'%s' is not GLSL 1.10" % token, position)
        elif kind == "number":
            if re.search(r"[A-Za-z]", re.sub(r"[eE][+-]?\d+", "", token)):
                fail("number suffix in %s" % token, position)
        elif kind == "operator":
            if token.rstrip("=") in FORBIDDEN_OPERATORS and token not in ("^^",):
                fail("operator %s is not GLSL 1.10" % token, position)
            # a single & or | is a bit operation; && and || are logical
            if token in ("&", "|", "&=", "|="):
                fail("operator %s is not GLSL 1.10" % token, position)

    # GLSL 1.10 converts nothing implicitly: an integer literal in a float
    # expression is an error on strict compilers. The translators' integers
    # are array sizes and indices and a0's arithmetic; any other is reported.
    stream = [(kind, token, position) for kind, token, position in tokens(text) if kind not in ("comment", "space")]
    for index, (kind, token, position) in enumerate(stream):
        if kind != "number" or not token.isdigit():
            continue
        before = stream[index - 1][1] if index else ""
        after = stream[index + 1][1] if index + 1 < len(stream) else ""
        line_start = text.rfind("\n", 0, position) + 1
        line = text[line_start:text.find("\n", position)]
        if before == "[" and after == "]":
            continue
        if re.search(r"\ba0\b", line):
            continue
        fail("integer literal %s outside an index or a0's arithmetic: %s" % (token, line.strip()), position)
    return failures


def main(arguments):
    if len(arguments) == 2 and arguments[0] == "glsl110":
        folder = Path(arguments[1])
        files = sorted(folder.glob("*.glsl"))
        failures = []
        for path in files:
            failures += check_glsl110(path)
        for failure in failures:
            print(failure)
        print("%d files checked in %s, %d failures" % (len(files), folder, len(failures)))
        return 1 if failures or not files else 0
    if len(arguments) == 3 and arguments[0] == "same":
        first, second = Path(arguments[1]), Path(arguments[2])
        names = sorted({path.name for path in first.glob("*.glsl")} | {path.name for path in second.glob("*.glsl")})
        different = 0
        for name in names:
            a, b = first / name, second / name
            if not a.exists() or not b.exists():
                print("%s is in one folder only" % name)
                different += 1
            elif a.read_bytes() != b.read_bytes():
                print("%s differs" % name)
                different += 1
        print("%d files compared, %d different" % (len(names), different))
        return 1 if different or not names else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
