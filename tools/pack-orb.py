#!/usr/bin/env python3
"""Pack a theme folder into a .orb file, the format Orb Studio saves and orb-ponderer takes.

    python3 tools/pack-orb.py <theme folder> [out.orb]

The folder is what sits at /themes/<slug>/ on the card: theme.json, the *_style.json files,
PNG artwork and font_*.bin files. The slug is the folder's name. `_installed` is added when
missing, since the Orb only counts a folder that has it. The reverse of read-orb-bundle.py
and orb-to-sim.py.
"""
import os
import re
import struct
import sys


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = os.path.abspath(os.path.expanduser(sys.argv[1]))
    slug = os.path.basename(src.rstrip("/"))
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,30}", slug):
        raise SystemExit(f"'{slug}' is not a usable slug: lower-case letters, digits, - and _, at most 31")
    out = sys.argv[2] if len(sys.argv) > 2 else slug + ".orb"

    files = {}
    for name in sorted(os.listdir(src)):
        path = os.path.join(src, name)
        if name.startswith(".") or not os.path.isfile(path):
            continue
        if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9._-]{0,39}", name):
            raise SystemExit(f"'{name}': file names are at most 40 of letters, digits, . _ -")
        with open(path, "rb") as f:
            files[name] = f.read()
        # The firmware's theme-art decoders only handle 8-bit RGBA and draw anything else
        # as black (menu_sprite.cpp line_cb and its siblings). IHDR: depth at 24, type at 25.
        d = files[name]
        if name.lower().endswith(".png") and d[:8] == b"\x89PNG\r\n\x1a\n" and (d[24], d[25]) != (8, 6):
            print(f"warning: {name} is not an 8-bit RGBA PNG (depth {d[24]}, colour type {d[25]}); the Orb will draw it black")
    files.setdefault("_installed", b"1\n")
    if "theme.json" not in files:
        print("warning: no theme.json; the Orb will show the slug as the theme's name")

    blob = b"ORBTHM01" + struct.pack("<H", len(slug)) + slug.encode() + struct.pack("<H", len(files))
    for name, data in files.items():
        blob += struct.pack("<H", len(name)) + name.encode() + struct.pack("<I", len(data)) + data
    with open(out, "wb") as f:
        f.write(blob)
    print(f"{out}: {len(files)} files, {len(blob) / 1024:.0f} KB, slug '{slug}'")


main()
