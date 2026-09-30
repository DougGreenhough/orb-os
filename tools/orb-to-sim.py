#!/usr/bin/env python3
"""Unpack a .orb theme file into the simulator's stand-in SD card, and wear it.

Orb Studio can save a theme as a .orb file instead of sending it down the cable. This puts
that file where the desktop simulator looks for themes (sim/sdcard/themes/<slug>/) and
points the simulator at it, so a design can be looked at with no device attached.

    python3 tools/orb-to-sim.py "~/Downloads/Steam Punk.orb"
    python3 tools/orb-to-sim.py "~/Downloads/Steam Punk.orb" --keep   # don't switch to it

Uses the same unpacker as read-orb-bundle.py.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SLUG_FILE = "/tmp/orb_sim_theme_slug"   # theme_select.cpp's NATIVE_SLUG_FILE

spec = importlib.util.spec_from_file_location("rob", os.path.join(HERE, "read-orb-bundle.py"))
src = open(spec.origin).read().replace("\nmain()\n", "\n")   # import without running it
rob = type(sys)("rob")
exec(compile(src, spec.origin, "exec"), rob.__dict__)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    slug, files = rob.unpack(os.path.expanduser(args[0]))
    dest = os.path.join(ROOT, "sim", "sdcard", "themes", slug)
    os.makedirs(dest, exist_ok=True)
    for name, data in files:
        path = os.path.normpath(os.path.join(dest, name))
        if not path.startswith(dest + os.sep):
            raise SystemExit(f"refusing a file outside the theme folder: {name}")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    print(f"{len(files)} files -> {os.path.relpath(dest, ROOT)}")
    if "--keep" not in sys.argv:
        with open(SLUG_FILE, "w") as f:
            f.write(slug + "\n")
        print(f"simulator will wear '{slug}' on its next start")


main()
