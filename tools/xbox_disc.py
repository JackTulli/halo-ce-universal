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
import os
import shutil
import subprocess
import sys
from pathlib import Path

BUILD = Path("build/xbox")
DISC = BUILD / "disc"
ISO = BUILD / "halo-xbox.iso"


def extract_xiso() -> Path:
    for candidate in (os.environ.get("NXDK_DIR"), Path.home() / "nxdk"):
        if candidate and (Path(candidate) / "tools/extract-xiso/build/extract-xiso").is_file():
            return Path(candidate) / "tools/extract-xiso/build/extract-xiso"
    sys.exit("nxdk's extract-xiso is not built (make in one of nxdk's samples builds it)")


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
        help="a setting for the disc's config.toml (D:\\config.toml), such as "
             "network.master_server=10.0.2.2:27920; none removes the file")
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
        init.write_text("".join(command.strip() + "\r\n" for command in args.init.split(";") if command.strip()))
    performance = DISC / "performance.txt"
    performance.unlink(missing_ok=True)
    if args.performance:
        performance.write_text("performance mode (port/xbox/README.md): this file's presence turns the costliest effects off\r\n")
    config = DISC / "config.toml"
    config.unlink(missing_ok=True)
    if args.setting:
        sections = {}
        for setting in args.setting:
            name, _, value = setting.partition("=")
            section, _, key = name.strip().partition(".")
            if not key or not value.strip():
                sys.exit(f"--setting {setting}: expected section.key=value")
            value = value.strip()
            if value not in ("true", "false") and not value.replace(".", "", 1).lstrip("-").isdigit():
                value = '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
            sections.setdefault(section.strip(), []).append(f"{key.strip()} = {value}")
        config.write_text("".join(f"[{section}]\r\n" + "".join(line + "\r\n" for line in lines) + "\r\n"
                                  for section, lines in sections.items()))
    new = iso.with_suffix(".iso.new")
    new.unlink(missing_ok=True)
    subprocess.run([str(tool), "-c", str(DISC), str(new)], check=True, stdout=subprocess.DEVNULL)
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
