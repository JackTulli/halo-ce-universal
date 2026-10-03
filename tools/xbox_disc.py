#!/usr/bin/env python3
"""Makes the Xbox build's disc image, build/xbox/halo-xbox.iso: the XBE with
the game's maps.

    python tools/xbox_disc.py --extract "Halo.xiso.iso"   # once: maps/ from the game's disc
    python tools/xbox_disc.py                             # after each build
    python tools/xbox_disc.py --out ~/xemu/halo-xbox.iso  # ... where the emulator reads it

The maps stay in build/xbox/disc/maps. The image is rewritten in place, so
an emulator that has it open boots the new build after a reset. Needs
nxdk's extract-xiso (NXDK_DIR, or ~/nxdk).
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BUILD = Path("build/xbox")
DISC = BUILD / "disc"
ISO = BUILD / "halo-xbox.iso"


def extract_xiso() -> Path:
    for candidate in (os.environ.get("NXDK_DIR"), Path.home() / "nxdk"):
        # (extract-xiso.exe where nxdk was built on Windows)
        for name in ("extract-xiso", "extract-xiso.exe"):
            if candidate and (Path(candidate) / "tools/extract-xiso/build" / name).is_file():
                return Path(candidate) / "tools/extract-xiso/build" / name
    sys.exit("nxdk's extract-xiso is not built (make in one of nxdk's samples builds it)")


CONFIG_SOURCE = Path("port/linux/src/port_config.c")


def _c_tokens(text: str):
    """The tokens of a C initializer: ("string", decoded text), ("word",
    identifier or number) and ("punct", character); comments dropped."""
    escapes = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "0": "\0"}
    index = 0
    while index < len(text):
        char = text[index]
        if char.isspace():
            index += 1
        elif text.startswith("/*", index):
            index = text.index("*/", index) + 2
        elif text.startswith("//", index):
            index = text.find("\n", index) if "\n" in text[index:] else len(text)
        elif char == '"':
            index += 1
            value = []
            while text[index] != '"':
                if text[index] == "\\":
                    value.append(escapes.get(text[index + 1], text[index + 1]))
                    index += 2
                else:
                    value.append(text[index])
                    index += 1
            index += 1
            yield ("string", "".join(value))
        elif char.isalnum() or char == "_":
            start = index
            while index < len(text) and (text[index].isalnum() or text[index] in "_."):
                index += 1
            yield ("word", text[start:index])
        else:
            index += 1
            yield ("punct", char)


def console_settings():
    """The settings the Xbox build reads (port_config.c's table, those for
    the console or every platform): (name, default as written, comment)."""
    source = CONFIG_SOURCE.read_text(encoding="utf-8")
    start = source.index("config_settings[] =")
    body = source[source.index("{", start) + 1:source.index("\n};", start)]
    settings = []
    fields, field, depth = [], [], 0
    for kind, value in _c_tokens(body):
        if kind == "punct" and value == "{":
            depth += 1
            fields, field = [], []
        elif kind == "punct" and value == "}":
            depth -= 1
            fields.append(field)
            name, _, default, _, _, platforms, comment = fields
            platforms = {token for kind, token in platforms if kind == "word"}
            if platforms & {"_platform_console", "_platform_all"}:
                settings.append(("".join(token for _, token in name), "".join(token for _, token in default),
                                 "".join(token for _, token in comment)))
        elif depth and kind == "punct" and value == ",":
            fields.append(field)
            field = []
        elif depth:
            field.append((kind, value))
    return settings


def typed_value(name: str, value: str, default) -> str:
    """A --setting's value as the file writes it, in the setting's own type
    (the default's): True, 1, yes and on are a boolean's true; a number is
    checked; a string is quoted. A wrong type the game would ignore, and a
    "720p disc" then ran at 480 with its 64 MB limit on."""
    if default is None:
        return value  # (config_text names the unknown setting)
    if default in ("true", "false"):
        lowered = value.lower()
        if lowered in ("true", "1", "yes", "on"):
            return "true"
        if lowered in ("false", "0", "no", "off"):
            return "false"
        sys.exit(f"--setting {name}={value}: expected true or false")
    if default.startswith('"'):
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    # in a spelling TOML reads as the setting's type: Python takes 1e2, .5,
    # 090 and inf, which TOML rejects, and a bad file leaves every setting
    # on the disc at its default
    if "." not in default:
        if not re.fullmatch(r"[+-]?(0|[1-9][0-9]*)", value) or not -2**63 <= int(value) < 2**63:
            sys.exit(f"--setting {name}={value}: expected a whole number")
        return str(int(value))
    try:
        number = float(value)
    except ValueError:
        sys.exit(f"--setting {name}={value}: expected a number")
    if not math.isfinite(number):
        sys.exit(f"--setting {name}={value}: expected a number")
    return repr(number)


def config_text(overrides: dict, performance: bool = False) -> str:
    """The disc's config.toml: every setting of the console's, with its
    explanation, at its default unless overridden (the game cannot add the
    missing ones itself on a read-only disc, as the desktop builds do)."""
    known = {name for name, _, _ in console_settings()}
    unknown = sorted(set(overrides) - known)
    if unknown:
        sys.exit(f"--setting: the console has no {', '.join(unknown)} (settings: {CONFIG_SOURCE})")
    lines = [
        "# Halo settings (the Xbox build's)",
        "#",
        "# Every setting the console reads, at its default unless the disc was",
        "# packed with --setting (marked below). tools/xbox_disc.py writes this",
        "# file; on a hard disk copy it can be edited next to default.xbe.",
    ]
    if performance:
        lines += [
            "#",
            "# This disc has performance.txt: shadows, soft shadows, dynamic lights,",
            "# specular, reflections, detail objects, bump mapping, sun rays and",
            "# camouflage quality stay off whatever [graphics] says (the settings",
            "# can only turn effects off).",
        ]
    section = None
    for name, default, comment in console_settings():
        group, _, key = name.partition(".")
        if group != section:
            section = group
            lines += ["", f"[{section}]"]
        lines.append("")
        lines += [f"# {line}" if line else "#" for line in comment.split("\n")]
        if name in overrides:
            lines.append(f"# (set when the disc was packed; the default is {default})")
        lines.append(f"{key} = {overrides.get(name, default)}")
    return "".join(line + "\r\n" for line in lines)


def native_path(path: Path) -> str:
    """An absolute path as extract-xiso reads it: on Windows it changes
    directory before it reads the folder and splits paths at backslashes
    only."""
    text = str(path.resolve())
    return text.replace("/", "\\") if os.name == "nt" else text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--extract", metavar="ISO", help="the game's disc image, whose maps/ to keep")
    parser.add_argument("--out", metavar="PATH", default=str(ISO),
        help=f"the image to write, in place (default {ISO})")
    parser.add_argument("--init", metavar="COMMANDS",
        help="console commands for the disc's init.txt, run at startup (';'-separated); a test disc")
    parser.add_argument("--performance", action="store_true",
        help="performance mode: a performance.txt on the disc (the costliest effects off; port/xbox/README.md)")
    parser.add_argument("--setting", metavar="SECTION.KEY=VALUE", action="append", default=[],
        help="a setting for the disc's config.toml (D:\\config.toml, which lists every setting "
             "the console reads), such as graphics.shadows=false")
    args = parser.parse_args()
    tool = extract_xiso()
    iso = Path(args.out).expanduser()
    if args.extract:
        scratch = BUILD / "disc-extract"
        shutil.rmtree(scratch, ignore_errors=True)
        subprocess.run([str(tool), "-x", "-d", str(scratch), args.extract], check=True, stdout=subprocess.DEVNULL)
        DISC.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(DISC / "maps", ignore_errors=True)
        shutil.move(str(scratch / "maps"), str(DISC / "maps"))
        shutil.rmtree(scratch, ignore_errors=True)
        print(f"{DISC / 'maps'}: {sum(1 for _ in (DISC / 'maps').iterdir())} files")
    if not (DISC / "maps").is_dir():
        sys.exit(f"no {DISC / 'maps'}: run with --extract first")
    shutil.copyfile(BUILD / "default.xbe", DISC / "default.xbe")
    init = DISC / "init.txt"
    init.unlink(missing_ok=True)
    if args.init:
        # (newline="": the line ends as written, not \r\r\n on Windows, which
        # the console's parsers take for an error)
        init.write_text("".join(command.strip() + "\r\n" for command in args.init.split(";") if command.strip()),
                        newline="")
    performance = DISC / "performance.txt"
    performance.unlink(missing_ok=True)
    if args.performance:
        performance.write_text("performance mode (port/xbox/README.md): this file's presence turns the costliest effects off\r\n",
                               newline="")
    overrides = {}
    defaults = {name: default for name, default, _ in console_settings()}
    for setting in args.setting:
        name, _, value = setting.partition("=")
        section, _, key = name.strip().partition(".")
        if not key or not value.strip():
            sys.exit(f"--setting {setting}: expected section.key=value")
        name = f"{section.strip()}.{key.strip()}"
        overrides[name] = typed_value(name, value.strip(), defaults.get(name))
    text = config_text(overrides, args.performance)
    # (a file the console cannot parse runs every setting at its default)
    try:
        import tomllib
    except ImportError:
        tomllib = None
    if tomllib:
        try:
            tomllib.loads(text)
        except tomllib.TOMLDecodeError as error:
            sys.exit(f"config.toml would not parse ({error}); not packed")
    (DISC / "config.toml").write_text(text, newline="")
    # 720p needs the top 64 MB of a 128 MB console: this disc's XBE loses
    # the kernel's 64 MB limit (the build's keeps it; port/xbox/README.md)
    if overrides.get("display.hd_720p") == "true":
        subprocess.run([sys.executable, str(Path(__file__).with_name("xbox_xbe_flags.py")), str(DISC / "default.xbe")],
                       check=True)
        print(f"{DISC / 'default.xbe'}: the 64 MB limit is off (720p)")
    new = iso.with_suffix(".iso.new")
    new.unlink(missing_ok=True)
    subprocess.run([str(tool), "-c", native_path(DISC), native_path(new)], check=True, stdout=subprocess.DEVNULL)
    if iso.is_file():
        with open(new, "rb") as source, open(iso, "r+b") as target:
            shutil.copyfileobj(source, target, 16 << 20)
            target.truncate(source.tell())
        new.unlink()
    else:
        new.rename(iso)
    print(f"{iso}: {iso.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
