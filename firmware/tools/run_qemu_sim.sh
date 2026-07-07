#!/bin/sh
# Build (if needed) and run the pocket-fmdsp firmware under QEMU (netduino2,
# Cortex-M3) with semihosting. Produces LCD screenshots (PPM) and audio (WAV)
# in build-fw/sim-run/, mirroring what the firmware would drive on the Primer2.
#
# Joystick script: arg 1 (default "DDD C R C" = down x3, play, page-down, play).
set -e
cd "$(dirname "$0")/../.."   # repo root
ELF=build-fw/pocketfm_sim.elf
OUTDIR=build-fw/sim-run
INPUT="${1:-DDD C R C}"

[ -f "$ELF" ] || { echo "build first: cmake --build build-fw"; exit 1; }

# regenerate fonts/strings/manifest so the run is reproducible
python3 firmware/tools/gen_tracklist.py sim_tracklist.txt
printf '%s' "$INPUT" > sim_input.txt

rm -f sim_shot_*.ppm sim_play_*.wav
echo "== QEMU run (input: $INPUT) =="
timeout 180 qemu-system-arm -M netduino2 -cpu cortex-m3 -nographic \
  -semihosting-config enable=on,target=native \
  -kernel "$ELF" || true

mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/*.ppm "$OUTDIR"/*.wav "$OUTDIR"/*.png
mv -f sim_shot_*.ppm sim_play_*.wav "$OUTDIR"/ 2>/dev/null || true
mv -f sim_tracklist.txt sim_input.txt "$OUTDIR"/ 2>/dev/null || true

# make a contact sheet of the screenshots if PIL is available
python3 - "$OUTDIR" <<'PY' || true
import sys, glob, os
try:
    from PIL import Image
except Exception:
    sys.exit(0)
d = sys.argv[1]
shots = sorted(glob.glob(os.path.join(d, "sim_shot_*.ppm")))
if not shots:
    sys.exit(0)
imgs = [Image.open(s).convert("RGB") for s in shots]
w, h = imgs[0].size
scale, pad, cols = 2, 8, min(6, len(imgs))
rows = (len(imgs) + cols - 1) // cols
sheet = Image.new("RGB", (cols*(w*scale+pad)+pad, rows*(h*scale+pad)+pad), (30, 30, 36))
for i, im in enumerate(imgs):
    im = im.resize((w*scale, h*scale), Image.NEAREST)
    r, c = divmod(i, cols)
    sheet.paste(im, (pad + c*(w*scale+pad), pad + r*(h*scale+pad)))
sheet.save(os.path.join(d, "contact_sheet.png"))
print("wrote", os.path.join(d, "contact_sheet.png"), "from", len(shots), "screenshots")
PY

echo "== artifacts in $OUTDIR =="
ls -1 "$OUTDIR"
