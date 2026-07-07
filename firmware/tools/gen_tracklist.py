#!/usr/bin/env python3
"""Build the sim 'SD card' manifest: one line per .M file, "<path>\t<group>".
Paths are relative to the repo root (QEMU runs there); group is the Japanese
game title, written as Shift-JIS bytes so the firmware renders it natively."""
import os, sys, glob

ROOT = "songs/touhou_pc98"
OUT = sys.argv[1] if len(sys.argv) > 1 else "sim_tracklist.txt"

GAMES = {
    "TH1": "東方靈異伝",
    "TH2": "東方封魔録",
    "TH3": "東方夢時空",
    "TH4": "東方幻想郷",
    "TH5": "東方怪綺談",
}

paths = sorted(glob.glob(os.path.join(ROOT, "**", "*.M"), recursive=True))
with open(OUT, "wb") as f:
    for p in paths:
        d = os.path.basename(os.path.dirname(p))       # e.g. "TH1 - HRtP"
        key = d[:3].upper()                            # "TH1"
        grp = GAMES.get(key, "")
        f.write(p.encode("ascii") + b"\t" + grp.encode("shift_jis") + b"\n")
print(f"wrote {OUT}: {len(paths)} tracks")
